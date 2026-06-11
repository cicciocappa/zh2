/* sim_particles.c — implementation. See sim_particles.h for the model. */

#include "sim_particles.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ------------------------------------------------------------------ utils */

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* xorshift32 — deterministic, no libc rand */
static inline uint32_t rng_next(uint32_t *st) {
    uint32_t x = *st;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *st = x;
}
static inline float rng_f01(uint32_t *st) {           /* [0,1) */
    return (float)(rng_next(st) >> 8) * (1.0f / 16777216.0f);
}
static inline float rng_fsym(uint32_t *st) {          /* [-1,1) */
    return rng_f01(st) * 2.0f - 1.0f;
}

#define PHI_INF 1e30f
#define GRAV 9.81f            /* fake-z gravity (m/s^2) */
#define CORPSE_CAP 4096       /* fixed corpse pool: a cost guarantee */
#define TAKEOFF_VZ 1.0f       /* vertical speed that flips SIMP_FLYING */
#define COST_MIN (-0.8f)      /* user cost clamp: edges stay positive */
#define COST_MAX 100.0f       /* and never competitive with WALL_ENTER */
#define RHO_EMA 0.3f          /* density EMA gain per flow recompute */
#define CORPSE_RHO 2.0f       /* a corpse counts as this many agents */

/* --------------------------------------------------------------- the state */

struct SimP {
    /* nav grid */
    int gw, gh;
    float cell;            /* cell side (m) */
    float inv_cell;
    float world_w, world_h;
    uint8_t *solid;        /* gw*gh, 1 = wall */
    uint8_t *goal;         /* gw*gh, 1 = goal/drain cell */
    float   *phi;          /* cost-to-goal */
    float   *flow_x;       /* normalized direction field (0,0 in walls) */
    float   *flow_y;
    float   *sdf;          /* signed distance to walls (m), >0 = free */
    bool     nav_dirty;

    /* navigation costs (M3.5 + M3.6) */
    float   *cost_user;    /* gw*gh additive user cost, clamped on write */
    float   *rho_raw;      /* gw*gh scratch histogram (agents per cell) */
    float   *rho_s;        /* gw*gh smoothed density (blur + EMA) */
    float   *cost_mult;    /* gw*gh edge multiplier, filled per recompute */
    bool     cost_dirty;   /* cost_user changed since last flow recompute */
    bool     rho_active;   /* rho_s still holds non-negligible mass */
    float    flow_timer;   /* seconds since last throttled recompute */

    /* preallocated nav scratch (recomputes run inside simp_step: no malloc) */
    void    *heap_nodes;   /* HNode[gw*gh*8] for the Dijkstra */
    float   *flow_tx, *flow_ty;

    /* agents (SoA) */
    int   count, cap;
    float *px, *py;        /* positions */
    float *qx, *qy;        /* previous positions (pre-projection) */
    float *vx, *vy;
    float *rad;            /* per-agent radius */
    float *vpref;          /* per-agent preferred speed */
    float *invm;           /* inverse mass (~ 1/r^2) */
    uint32_t *seed;        /* per-agent RNG state */
    uint8_t *aflags;       /* behaviour flags (SIMP_DORMANT, SIMP_FLYING) */
    float *z, *vz;         /* fake third axis for ballistic flight */

    /* agents landed during the last step (handles: drain-safe) */
    SimPHandle *landed;
    int landed_n;

    /* corpse pool: static obstacle discs, TTL-managed */
    int   corpse_count;
    float *cpx, *cpy, *crad, *cttl;
    float corpse_rmax;     /* largest radius ever in pool (search reach) */

    /* slot map: stable handles over swap-and-pop (see header) */
    int      *slot_to_index;  /* cap : slot -> dense index, -1 if free   */
    int      *index_to_slot;  /* cap : dense index -> slot               */
    uint16_t *slot_gen;       /* cap : bumped on kill, 12 bits used      */
    int      *slot_free;      /* cap : LIFO stack of free slots          */
    int       slot_free_top;

    /* collision grid (uniform, rebuilt each step via counting sort).
     * Binned entries: grounded agents (dense index) + corpse "ghosts"
     * (index >= grid_total, ghost data lives past the agents in the SoA
     * arrays). Flying agents are NOT binned. */
    int   cgw, cgh;
    float ccell, inv_ccell;
    float grid_rmax;       /* largest agent radius the grid is sized for */
    int  *ccount;          /* cgw*cgh + 1 : counts then prefix sums */
    int  *cstart;          /* prefix sums (cgw*cgh + 1) */
    int  *corder;          /* cap + CORPSE_CAP : entries sorted by cell */
    int   grid_total;      /* agent count at the last rebuild */
    int   grid_ghosts;     /* corpse ghosts binned at the last rebuild */
    bool  grid_stale;      /* set by kill/corpse_add: indices unreliable */

    SimPParams params;
    uint32_t rng;          /* sim-level RNG (spawn jitter) */
    float diag_overlap_sum;
    int   diag_overlap_n;
};

/* ---------------------------------------------------- navigation: dijkstra */

/* simple binary min-heap of (cost, cell) */
typedef struct { float c; int i; } HNode;
typedef struct { HNode *a; int n; } Heap;

static void heap_push(Heap *h, float c, int i) {
    int k = h->n++;
    h->a[k].c = c; h->a[k].i = i;
    while (k > 0) {
        int p = (k - 1) >> 1;
        if (h->a[p].c <= h->a[k].c) break;
        HNode t = h->a[p]; h->a[p] = h->a[k]; h->a[k] = t;
        k = p;
    }
}
static HNode heap_pop(Heap *h) {
    HNode top = h->a[0];
    h->a[0] = h->a[--h->n];
    int k = 0;
    for (;;) {
        int l = 2 * k + 1, r = l + 1, m = k;
        if (l < h->n && h->a[l].c < h->a[m].c) m = l;
        if (r < h->n && h->a[r].c < h->a[m].c) m = r;
        if (m == k) break;
        HNode t = h->a[m]; h->a[m] = h->a[k]; h->a[k] = t;
        k = m;
    }
    return top;
}

/* Walls are passable at a HUGE cost rather than blocked (same trick as the
 * continuum core). Where any open route exists it always wins (a detour costs
 * far less than this), so phi still guides the horde AROUND obstacles. But a
 * fully walled-off goal is no longer unreachable: phi keeps decreasing toward
 * it through the walls, so the horde is drawn to PRESS against the surrounding
 * walls (siege) instead of ignoring the goal. Map detours top out well under
 * WALL_ENTER (phi is in cell units; the whole grid is a few hundred cells). */
#define WALL_ENTER 5000.0f

static void recompute_phi(SimP *s) {
    const int n = s->gw * s->gh;
    for (int i = 0; i < n; i++) s->phi[i] = PHI_INF;

    /* per-destination edge multiplier (M3.5 user cost + M3.6 density).
     * rho_max = packing density of a nav cell (agents whose center fits) */
    const float kd = s->params.k_density;
    const float r0 = s->params.radius;
    const float inv_rho_max =
        (3.14159265f * r0 * r0) / (s->cell * s->cell * 0.7f);
    for (int i = 0; i < n; i++) {
        float m = 1.0f + s->cost_user[i];
        if (kd > 0.0f) {
            float t = s->rho_s[i] * inv_rho_max;
            m += kd * (t > 1.0f ? 1.0f : t);
        }
        s->cost_mult[i] = m < 0.2f ? 0.2f : m;
    }

    /* heap scratch is preallocated: pushes are bounded by successful phi
     * improvements, n*8 has ample slack over the n*4 seen in practice */
    Heap h; h.a = (HNode *)s->heap_nodes; h.n = 0;
    for (int i = 0; i < n; i++)
        if (s->goal[i] && !s->solid[i]) { s->phi[i] = 0.0f; heap_push(&h, 0.0f, i); }

    static const int   DX[8] = { 1,-1, 0, 0, 1, 1,-1,-1 };
    static const int   DY[8] = { 0, 0, 1,-1, 1,-1, 1,-1 };
    static const float DC[8] = { 1,1,1,1, 1.41421356f,1.41421356f,1.41421356f,1.41421356f };

    while (h.n > 0) {
        HNode nd = heap_pop(&h);
        if (nd.c > s->phi[nd.i]) continue;          /* stale entry */
        int cx = nd.i % s->gw, cy = nd.i / s->gw;
        for (int k = 0; k < 8; k++) {
            int nx = cx + DX[k], ny = cy + DY[k];
            if (nx < 0 || ny < 0 || nx >= s->gw || ny >= s->gh) continue;
            int j = ny * s->gw + nx;
            float nc = nd.c + DC[k] * s->cost_mult[j];
            if (s->solid[j]) nc += WALL_ENTER;
            /* diagonal corner-cutting through walls pays the wall toll too */
            else if (k >= 4 && (s->solid[cy * s->gw + nx] || s->solid[ny * s->gw + cx]))
                nc += WALL_ENTER;
            if (nc < s->phi[j]) { s->phi[j] = nc; heap_push(&h, nc, j); }
        }
    }
}

static void recompute_flow(SimP *s) {
    const int gw = s->gw, gh = s->gh;
    /* direction = toward the lowest-phi reachable neighbour */
    for (int cy = 0; cy < gh; cy++) {
        for (int cx = 0; cx < gw; cx++) {
            int i = cy * gw + cx;
            s->flow_x[i] = 0.0f; s->flow_y[i] = 0.0f;
            if (s->solid[i] || s->phi[i] >= PHI_INF) continue;
            float best = s->phi[i];
            float bx = 0.0f, by = 0.0f;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) continue;
                int nx = cx + dx, ny = cy + dy;
                if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
                int j = ny * gw + nx;
                /* wall neighbours stay in the running: their phi is lower than
                 * ours only when our own phi went THROUGH that wall (sealed
                 * pocket), in which case the flow points INTO the wall and the
                 * horde presses against it (siege). On open routes wall phi is
                 * higher by >= WALL_ENTER, so it never wins. */
                if (dx && dy && !s->solid[j] &&
                    (s->solid[cy * gw + nx] || s->solid[ny * gw + cx])) continue;
                if (s->phi[j] < best) { best = s->phi[j]; bx = (float)dx; by = (float)dy; }
            }
            float len = sqrtf(bx * bx + by * by);
            if (len > 0.0f) { s->flow_x[i] = bx / len; s->flow_y[i] = by / len; }
        }
    }
    /* one smoothing pass (skip walls), then renormalize */
    float *tx = s->flow_tx;
    float *ty = s->flow_ty;
    for (int cy = 0; cy < gh; cy++) {
        for (int cx = 0; cx < gw; cx++) {
            int i = cy * gw + cx;
            if (s->solid[i]) { tx[i] = ty[i] = 0.0f; continue; }
            float ax = 0.0f, ay = 0.0f; int n = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int nx = cx + dx, ny = cy + dy;
                if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
                int j = ny * gw + nx;
                if (s->solid[j]) continue;
                ax += s->flow_x[j]; ay += s->flow_y[j]; n++;
            }
            if (n) { ax /= (float)n; ay /= (float)n; }
            float len = sqrtf(ax * ax + ay * ay);
            if (len > 1e-6f) { tx[i] = ax / len; ty[i] = ay / len; }
            else             { tx[i] = s->flow_x[i]; ty[i] = s->flow_y[i]; }
        }
    }
    memcpy(s->flow_x, tx, sizeof(float) * (size_t)gw * gh);
    memcpy(s->flow_y, ty, sizeof(float) * (size_t)gw * gh);
}

/* M3.6: refresh the smoothed density field from current agent positions.
 * Fresh histogram (grounded agents + corpse weight), 3x3 blur folded into a
 * temporal EMA: rho_s tracks the crowd with ~2 recompute periods of lag,
 * which is exactly what stops the flow from flickering between routes. */
static void density_update(SimP *s) {
    const int gw = s->gw, gh = s->gh, n = gw * gh;
    memset(s->rho_raw, 0, (size_t)n * sizeof(float));
    for (int i = 0; i < s->count; i++) {
        if (s->aflags[i] & SIMP_FLYING) continue;
        int cx = clampi((int)(s->px[i] * s->inv_cell), 0, gw - 1);
        int cy = clampi((int)(s->py[i] * s->inv_cell), 0, gh - 1);
        s->rho_raw[cy * gw + cx] += 1.0f;
    }
    for (int j = 0; j < s->corpse_count; j++) {
        int cx = clampi((int)(s->cpx[j] * s->inv_cell), 0, gw - 1);
        int cy = clampi((int)(s->cpy[j] * s->inv_cell), 0, gh - 1);
        s->rho_raw[cy * gw + cx] += CORPSE_RHO;
    }
    float peak = 0.0f;
    for (int cy = 0; cy < gh; cy++) {
        for (int cx = 0; cx < gw; cx++) {
            float acc = 0.0f; int cnt = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int nx = cx + dx, ny = cy + dy;
                if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
                acc += s->rho_raw[ny * gw + nx]; cnt++;
            }
            int i = cy * gw + cx;
            s->rho_s[i] = s->rho_s[i] * (1.0f - RHO_EMA) + (acc / (float)cnt) * RHO_EMA;
            if (s->rho_s[i] > peak) peak = s->rho_s[i];
        }
    }
    /* keeps the throttled recompute alive until an emptied map decays out */
    s->rho_active = peak > 0.01f;
}

/* two-pass chamfer distance transform (3-4 metric scaled to meters) */
static void recompute_sdf(SimP *s) {
    const int gw = s->gw, gh = s->gh;
    const float ORTH = s->cell, DIAG = s->cell * 1.41421356f;
    const float BIG = 1e9f;
    float *d = s->sdf;
    for (int i = 0; i < gw * gh; i++) d[i] = s->solid[i] ? 0.0f : BIG;
    /* forward */
    for (int cy = 0; cy < gh; cy++) for (int cx = 0; cx < gw; cx++) {
        int i = cy * gw + cx; float v = d[i];
        if (cx > 0           && d[i - 1]      + ORTH < v) v = d[i - 1]      + ORTH;
        if (cy > 0           && d[i - gw]     + ORTH < v) v = d[i - gw]     + ORTH;
        if (cx > 0 && cy > 0 && d[i - gw - 1] + DIAG < v) v = d[i - gw - 1] + DIAG;
        if (cx < gw-1 && cy > 0 && d[i - gw + 1] + DIAG < v) v = d[i - gw + 1] + DIAG;
        d[i] = v;
    }
    /* backward */
    for (int cy = gh - 1; cy >= 0; cy--) for (int cx = gw - 1; cx >= 0; cx--) {
        int i = cy * gw + cx; float v = d[i];
        if (cx < gw-1            && d[i + 1]      + ORTH < v) v = d[i + 1]      + ORTH;
        if (cy < gh-1            && d[i + gw]     + ORTH < v) v = d[i + gw]     + ORTH;
        if (cx < gw-1 && cy < gh-1 && d[i + gw + 1] + DIAG < v) v = d[i + gw + 1] + DIAG;
        if (cx > 0    && cy < gh-1 && d[i + gw - 1] + DIAG < v) v = d[i + gw - 1] + DIAG;
        d[i] = v;
    }
    /* distance measured cell-center to cell-center; walls occupy their full
     * cell, so shift by half a cell to approximate distance to the wall face */
    for (int i = 0; i < gw * gh; i++)
        d[i] = s->solid[i] ? -0.5f * s->cell : d[i] - 0.5f * s->cell;
}

static void nav_commit(SimP *s) {
    recompute_phi(s);
    recompute_flow(s);
    recompute_sdf(s);
    s->nav_dirty = false;
}

/* --------------------------------------------------- bilinear field access */

static inline float bilinear(const float *f, const SimP *s, float x, float y) {
    float gx = x * s->inv_cell - 0.5f, gy = y * s->inv_cell - 0.5f;
    int x0 = clampi((int)floorf(gx), 0, s->gw - 1);
    int y0 = clampi((int)floorf(gy), 0, s->gh - 1);
    int x1 = clampi(x0 + 1, 0, s->gw - 1);
    int y1 = clampi(y0 + 1, 0, s->gh - 1);
    float tx = clampf(gx - (float)x0, 0.0f, 1.0f);
    float ty = clampf(gy - (float)y0, 0.0f, 1.0f);
    float a = f[y0 * s->gw + x0], b = f[y0 * s->gw + x1];
    float c = f[y1 * s->gw + x0], dd = f[y1 * s->gw + x1];
    return (a + (b - a) * tx) + ((c + (dd - c) * tx) - (a + (b - a) * tx)) * ty;
}

void simp_sample_flow(const SimP *s, float x, float y, float *dx, float *dy) {
    float fx = bilinear(s->flow_x, s, x, y);
    float fy = bilinear(s->flow_y, s, x, y);
    float len = sqrtf(fx * fx + fy * fy);
    if (len > 1e-6f) { *dx = fx / len; *dy = fy / len; }
    else             { *dx = 0.0f;     *dy = 0.0f;     }
}

float simp_sample_sdf(const SimP *s, float x, float y) {
    return bilinear(s->sdf, s, x, y);
}

/* central-difference SDF gradient (points away from walls) */
static inline void sdf_grad(const SimP *s, float x, float y, float *gx, float *gy) {
    const float e = 0.5f * s->cell;
    float dx = bilinear(s->sdf, s, x + e, y) - bilinear(s->sdf, s, x - e, y);
    float dy = bilinear(s->sdf, s, x, y + e) - bilinear(s->sdf, s, x, y - e);
    float len = sqrtf(dx * dx + dy * dy);
    if (len > 1e-6f) { *gx = dx / len; *gy = dy / len; }
    else             { *gx = 0.0f;     *gy = 1.0f;     }
}

/* ------------------------------------------------------------- lifecycle */

SimP *simp_create(int gw, int gh, float cell_size, int max_agents) {
    SimP *s = (SimP *)calloc(1, sizeof(SimP));
    if (!s) return NULL;
    s->gw = gw; s->gh = gh;
    s->cell = cell_size; s->inv_cell = 1.0f / cell_size;
    s->world_w = (float)gw * cell_size;
    s->world_h = (float)gh * cell_size;

    s->params.v_max     = 1.4f;
    s->params.v_jitter  = 0.25f;
    s->params.a_max     = 8.0f;
    s->params.radius    = 0.30f;
    s->params.r_jitter  = 0.15f;
    s->params.noise_ang = 0.06f;
    s->params.damping   = 0.10f;
    s->params.v_clamp   = 20.0f;
    s->params.pbd_iters = 3;
    s->params.landing_damp = 0.30f;
    s->params.wall_h    = 2.0f;
    s->params.k_density = 2.0f;
    s->params.flow_period = 0.5f;

    const size_t n = (size_t)gw * gh;
    s->solid  = (uint8_t *)calloc(n, 1);
    s->goal   = (uint8_t *)calloc(n, 1);
    s->phi    = (float *)malloc(n * sizeof(float));
    s->flow_x = (float *)calloc(n, sizeof(float));
    s->flow_y = (float *)calloc(n, sizeof(float));
    s->sdf    = (float *)malloc(n * sizeof(float));
    s->cost_user = (float *)calloc(n, sizeof(float));
    s->rho_raw   = (float *)calloc(n, sizeof(float));
    s->rho_s     = (float *)calloc(n, sizeof(float));
    s->cost_mult = (float *)malloc(n * sizeof(float));
    s->heap_nodes = malloc(n * 8 * sizeof(HNode));
    s->flow_tx = (float *)malloc(n * sizeof(float));
    s->flow_ty = (float *)malloc(n * sizeof(float));
    s->cost_dirty = false;
    s->rho_active = false;
    s->flow_timer = 0.0f;

    s->cap = max_agents;
    /* px/py/rad/invm/seed carry CORPSE_CAP extra slots: corpse ghosts are
     * appended there before binning so the PBD kernel sees them as plain
     * zero-inverse-mass discs (no special cases in the hot loop) */
    const size_t capg = (size_t)max_agents + CORPSE_CAP;
    s->px = (float *)malloc(capg * sizeof(float));
    s->py = (float *)malloc(capg * sizeof(float));
    s->qx = (float *)malloc((size_t)max_agents * sizeof(float));
    s->qy = (float *)malloc((size_t)max_agents * sizeof(float));
    s->vx = (float *)calloc((size_t)max_agents, sizeof(float));
    s->vy = (float *)calloc((size_t)max_agents, sizeof(float));
    s->rad   = (float *)malloc(capg * sizeof(float));
    s->vpref = (float *)malloc((size_t)max_agents * sizeof(float));
    s->invm  = (float *)malloc(capg * sizeof(float));
    s->seed  = (uint32_t *)malloc(capg * sizeof(uint32_t));
    s->aflags = (uint8_t *)calloc((size_t)max_agents, 1);
    s->z  = (float *)calloc((size_t)max_agents, sizeof(float));
    s->vz = (float *)calloc((size_t)max_agents, sizeof(float));
    s->landed = (SimPHandle *)malloc((size_t)max_agents * sizeof(SimPHandle));
    s->landed_n = 0;

    s->cpx  = (float *)malloc(CORPSE_CAP * sizeof(float));
    s->cpy  = (float *)malloc(CORPSE_CAP * sizeof(float));
    s->crad = (float *)malloc(CORPSE_CAP * sizeof(float));
    s->cttl = (float *)malloc(CORPSE_CAP * sizeof(float));
    s->corpse_count = 0;
    s->corpse_rmax = 0.0f;

    /* slot map: all slots free, prefilled so the first pops give 0,1,2... */
    s->slot_to_index = (int *)malloc((size_t)max_agents * sizeof(int));
    s->index_to_slot = (int *)malloc((size_t)max_agents * sizeof(int));
    s->slot_gen      = (uint16_t *)calloc((size_t)max_agents, sizeof(uint16_t));
    s->slot_free     = (int *)malloc((size_t)max_agents * sizeof(int));
    for (int k = 0; k < max_agents; k++) {
        s->slot_to_index[k] = -1;
        s->slot_free[k] = max_agents - 1 - k;
    }
    s->slot_free_top = max_agents;

    /* collision grid: cell side = 2 * max plausible radius (grows if a
     * larger typed agent is ever spawned — see simp_spawn_desc) */
    float rmax = s->params.radius * (1.0f + s->params.r_jitter);
    s->grid_rmax = rmax;
    s->ccell = 2.0f * rmax * 1.05f;
    s->inv_ccell = 1.0f / s->ccell;
    s->cgw = (int)ceilf(s->world_w * s->inv_ccell) + 1;
    s->cgh = (int)ceilf(s->world_h * s->inv_ccell) + 1;
    /* zeroed so grid-validity checks (cstart[nc] vs count) read defined data
     * before the first rebuild: an empty grid is a valid grid for count 0 */
    s->ccount = (int *)calloc((size_t)s->cgw * s->cgh + 1, sizeof(int));
    s->cstart = (int *)calloc((size_t)s->cgw * s->cgh + 1, sizeof(int));
    s->corder = (int *)malloc(capg * sizeof(int));
    s->grid_total = 0;
    s->grid_ghosts = 0;
    s->grid_stale = false;

    s->rng = 0x9E3779B9u;
    s->nav_dirty = true;
    return s;
}

void simp_destroy(SimP *s) {
    if (!s) return;
    free(s->solid); free(s->goal); free(s->phi);
    free(s->flow_x); free(s->flow_y); free(s->sdf);
    free(s->cost_user); free(s->rho_raw); free(s->rho_s); free(s->cost_mult);
    free(s->heap_nodes); free(s->flow_tx); free(s->flow_ty);
    free(s->px); free(s->py); free(s->qx); free(s->qy);
    free(s->vx); free(s->vy);
    free(s->rad); free(s->vpref); free(s->invm); free(s->seed);
    free(s->aflags); free(s->z); free(s->vz); free(s->landed);
    free(s->cpx); free(s->cpy); free(s->crad); free(s->cttl);
    free(s->slot_to_index); free(s->index_to_slot);
    free(s->slot_gen); free(s->slot_free);
    free(s->ccount); free(s->cstart); free(s->corder);
    free(s);
}

SimPParams *simp_params(SimP *s) { return &s->params; }

/* ------------------------------------------------------------- terrain */

void simp_set_wall(SimP *s, int cx, int cy, bool solid) {
    if (cx < 0 || cy < 0 || cx >= s->gw || cy >= s->gh) return;
    s->solid[cy * s->gw + cx] = solid ? 1 : 0;
    s->nav_dirty = true;
}
void simp_set_goal(SimP *s, int cx, int cy, bool goal) {
    if (cx < 0 || cy < 0 || cx >= s->gw || cy >= s->gh) return;
    s->goal[cy * s->gw + cx] = goal ? 1 : 0;
    s->nav_dirty = true;
}
bool simp_is_wall(const SimP *s, int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= s->gw || cy >= s->gh) return true;
    return s->solid[cy * s->gw + cx] != 0;
}
void simp_terrain_commit(SimP *s) { nav_commit(s); }

void simp_add_cost(SimP *s, int cx, int cy, float w) {
    if (cx < 0 || cy < 0 || cx >= s->gw || cy >= s->gh) return;
    int i = cy * s->gw + cx;
    s->cost_user[i] = clampf(s->cost_user[i] + w, COST_MIN, COST_MAX);
    s->cost_dirty = true;
}

void simp_clear_cost(SimP *s) {
    memset(s->cost_user, 0, (size_t)s->gw * s->gh * sizeof(float));
    s->cost_dirty = true;
}

const float *simp_user_cost(const SimP *s)   { return s->cost_user; }
const float *simp_density_arr(const SimP *s) { return s->rho_s; }

/* ------------------------------------------------------------- agents */

/* shared bookkeeping: position, seed, flags, slot map. The caller fills
 * rad/vpref/invm (drawing any jitter from seed[i] AFTER this returns). */
static int spawn_common(SimP *s, float x, float y) {
    if (s->count >= s->cap) return -1;
    int cx = (int)(x * s->inv_cell), cy = (int)(y * s->inv_cell);
    if (simp_is_wall(s, cx, cy)) return -1;
    int i = s->count++;
    s->px[i] = x; s->py[i] = y;
    s->qx[i] = x; s->qy[i] = y;
    s->vx[i] = 0.0f; s->vy[i] = 0.0f;
    uint32_t st = s->rng; rng_next(&s->rng);
    s->seed[i] = st ^ 0xA511E9B3u ^ (uint32_t)i * 2654435761u;
    if (s->seed[i] == 0) s->seed[i] = 1;
    s->aflags[i] = 0;
    s->z[i] = 0.0f; s->vz[i] = 0.0f;
    /* count < cap guarantees the free stack is non-empty */
    int slot = s->slot_free[--s->slot_free_top];
    s->slot_to_index[slot] = i;
    s->index_to_slot[i] = slot;
    return i;
}

int simp_spawn(SimP *s, float x, float y) {
    int i = spawn_common(s, x, y);
    if (i < 0) return -1;
    float rj = 1.0f + s->params.r_jitter * rng_fsym(&s->seed[i]);
    float vj = 1.0f + s->params.v_jitter * rng_fsym(&s->seed[i]);
    s->rad[i]   = s->params.radius * rj;
    s->vpref[i] = s->params.v_max * vj;
    s->invm[i]  = 1.0f / (s->rad[i] * s->rad[i]);   /* mass ~ r^2 */
    return i;
}

int simp_spawn_dormant(SimP *s, float x, float y) {
    int i = simp_spawn(s, x, y);
    if (i >= 0) s->aflags[i] |= SIMP_DORMANT;
    return i;
}

int simp_spawn_desc(SimP *s, float x, float y, const SimPAgentDesc *d) {
    int i = spawn_common(s, x, y);
    if (i < 0) return -1;
    float vj = 1.0f + s->params.v_jitter * rng_fsym(&s->seed[i]);
    s->rad[i]   = d->radius;
    s->vpref[i] = d->v_pref * vj;
    /* mass in walker units: 1.0 = a default-radius agent. Default spawns
     * store invm = 1/r^2, i.e. mass = r^2; dividing by R0^2 puts both on
     * the same scale (PBD only ever uses invm ratios). */
    float r0 = s->params.radius;
    float m = d->mass > 1e-3f ? d->mass : 1e-3f;
    s->invm[i] = 1.0f / (m * r0 * r0);
    /* an agent larger than the grid was sized for would slip through the
     * 5-cell PBD pair search: coarsen the grid (fewer cells than allocated,
     * so no realloc; brute-force fallbacks cover until the next rebuild) */
    if (d->radius > s->grid_rmax) {
        s->grid_rmax = d->radius;
        s->ccell = 2.0f * s->grid_rmax * 1.05f;
        s->inv_ccell = 1.0f / s->ccell;
        s->cgw = (int)ceilf(s->world_w * s->inv_ccell) + 1;
        s->cgh = (int)ceilf(s->world_h * s->inv_ccell) + 1;
        s->grid_stale = true;
    }
    return i;
}

void simp_sleep(SimP *s, int i) {
    if (i < 0 || i >= s->count) return;
    if (s->aflags[i] & SIMP_FLYING) return;
    s->aflags[i] |= SIMP_DORMANT;
}

void simp_kill(SimP *s, int i) {
    int slot = s->index_to_slot[i];
    s->slot_gen[slot]++;                       /* wrap is fine (12 bits used) */
    s->slot_to_index[slot] = -1;
    s->slot_free[s->slot_free_top++] = slot;
    s->grid_stale = true;
    int last = --s->count;
    if (i == last) return;
    s->px[i] = s->px[last]; s->py[i] = s->py[last];
    s->qx[i] = s->qx[last]; s->qy[i] = s->qy[last];
    s->vx[i] = s->vx[last]; s->vy[i] = s->vy[last];
    s->rad[i] = s->rad[last]; s->vpref[i] = s->vpref[last];
    s->invm[i] = s->invm[last]; s->seed[i] = s->seed[last];
    s->aflags[i] = s->aflags[last];
    s->z[i] = s->z[last]; s->vz[i] = s->vz[last];
    int ls = s->index_to_slot[last];
    s->slot_to_index[ls] = i;
    s->index_to_slot[i] = ls;
}

int simp_count(const SimP *s) { return s->count; }

bool simp_free_at(const SimP *s, float x, float y, float r) {
    if (x < r || y < r || x > s->world_w - r || y > s->world_h - r) return false;
    if (simp_sample_sdf(s, x, y) < r) return false;
    /* The grid covers grounded agents [0, grid_total) + corpse ghosts, with
     * positions as of the last rebuild (end of step: exact). Agents spawned
     * since are checked linearly; spawns also recycle ghost slots, which
     * degrades corpse blocking for the rest of the frame — harmless. After
     * kills the grid references dead slots: full brute force. Airborne
     * agents never block a spawn point. */
    if (!s->grid_stale && s->grid_total <= s->count) {
        float rmax = s->grid_rmax;
        if (s->corpse_rmax > rmax) rmax = s->corpse_rmax;
        float reach = r + rmax;
        int x0 = clampi((int)((x - reach) * s->inv_ccell), 0, s->cgw - 1);
        int x1 = clampi((int)((x + reach) * s->inv_ccell), 0, s->cgw - 1);
        int y0 = clampi((int)((y - reach) * s->inv_ccell), 0, s->cgh - 1);
        int y1 = clampi((int)((y + reach) * s->inv_ccell), 0, s->cgh - 1);
        for (int cy = y0; cy <= y1; cy++) for (int cx = x0; cx <= x1; cx++) {
            int c = cy * s->cgw + cx;
            for (int k = s->cstart[c]; k < s->cstart[c + 1]; k++) {
                int i = s->corder[k];      /* agent or ghost: same arrays */
                float dx = s->px[i] - x, dy = s->py[i] - y;
                float rs = r + s->rad[i];
                if (dx * dx + dy * dy < rs * rs) return false;
            }
        }
        for (int i = s->grid_total; i < s->count; i++) {
            float dx = s->px[i] - x, dy = s->py[i] - y;
            float rs = r + s->rad[i];
            if (dx * dx + dy * dy < rs * rs) return false;
        }
        return true;
    }
    for (int i = 0; i < s->count; i++) {
        float dx = s->px[i] - x, dy = s->py[i] - y;
        float rs = r + s->rad[i];
        if (dx * dx + dy * dy < rs * rs) return false;
    }
    for (int j = 0; j < s->corpse_count; j++) {
        float dx = s->cpx[j] - x, dy = s->cpy[j] - y;
        float rs = r + s->crad[j];
        if (dx * dx + dy * dy < rs * rs) return false;
    }
    return true;
}

void simp_wake_radius(SimP *s, float x, float y, float radius) {
    const float r2 = radius * radius;
    for (int i = 0; i < s->count; i++) {
        if (!(s->aflags[i] & SIMP_DORMANT)) continue;
        float dx = s->px[i] - x, dy = s->py[i] - y;
        if (dx * dx + dy * dy <= r2) s->aflags[i] &= (uint8_t)~SIMP_DORMANT;
    }
}

void simp_wake_all(SimP *s) {
    for (int i = 0; i < s->count; i++)
        s->aflags[i] &= (uint8_t)~SIMP_DORMANT;
}

/* ------------------------------------------------------------- handles */

SimPHandle simp_handle_of(const SimP *s, int index) {
    if (index < 0 || index >= s->count) return SIMP_HANDLE_INVALID;
    int slot = s->index_to_slot[index];
    return ((uint32_t)(s->slot_gen[slot] & 0xFFFu) << 20) | (uint32_t)(slot + 1);
}

int simp_slot_of(const SimP *s, int index) {
    if (index < 0 || index >= s->count) return -1;
    return s->index_to_slot[index];
}

int simp_index_of(const SimP *s, SimPHandle h) {
    if (h == SIMP_HANDLE_INVALID) return -1;
    int slot = (int)(h & 0xFFFFFu) - 1;
    if (slot < 0 || slot >= s->cap) return -1;
    if ((h >> 20) != (uint32_t)(s->slot_gen[slot] & 0xFFFu)) return -1;
    return s->slot_to_index[slot];
}

/* ------------------------------------------------------- spatial queries */

/* The collision grid is coherent with current positions only between the
 * end of a step (final rebuild) and the next spawn/kill/corpse_add. The
 * grid bins grounded agents + corpse ghosts; queries skip the ghosts and,
 * by construction, never see airborne agents. */
static inline bool grid_current(const SimP *s) {
    return !s->grid_stale && s->grid_total == s->count;
}

int simp_query_circle(const SimP *s, float x, float y, float r,
                      int *out, int max_out, uint32_t flags) {
    const float r2 = r * r;
    int n = 0;
    if (grid_current(s) && !(flags & SIMP_QUERY_FLYING)) {
        int x0 = clampi((int)((x - r) * s->inv_ccell), 0, s->cgw - 1);
        int x1 = clampi((int)((x + r) * s->inv_ccell), 0, s->cgw - 1);
        int y0 = clampi((int)((y - r) * s->inv_ccell), 0, s->cgh - 1);
        int y1 = clampi((int)((y + r) * s->inv_ccell), 0, s->cgh - 1);
        for (int cy = y0; cy <= y1; cy++) for (int cx = x0; cx <= x1; cx++) {
            int c = cy * s->cgw + cx;
            for (int k = s->cstart[c]; k < s->cstart[c + 1]; k++) {
                int i = s->corder[k];
                if (i >= s->count) continue;               /* corpse ghost */
                float dx = s->px[i] - x, dy = s->py[i] - y;
                if (dx * dx + dy * dy > r2) continue;
                if (n == max_out) return n;
                out[n++] = i;
            }
        }
    } else {
        for (int i = 0; i < s->count; i++) {
            if (!(flags & SIMP_QUERY_FLYING) && (s->aflags[i] & SIMP_FLYING))
                continue;
            float dx = s->px[i] - x, dy = s->py[i] - y;
            if (dx * dx + dy * dy > r2) continue;
            if (n == max_out) return n;
            out[n++] = i;
        }
    }
    return n;
}

int simp_query_nearest(const SimP *s, float x, float y, float r_max) {
    int best = -1;
    float best_d2 = r_max * r_max;
    if (!grid_current(s)) {
        for (int i = 0; i < s->count; i++) {
            if (s->aflags[i] & SIMP_FLYING) continue;
            float dx = s->px[i] - x, dy = s->py[i] - y;
            float d2 = dx * dx + dy * dy;
            if (d2 <= best_d2) { best_d2 = d2; best = i; }
        }
        return best;
    }
    /* ring scan: cells at Chebyshev distance `ring` from the center cell.
     * An agent in ring m is at least (m-1)*ccell away from (x,y), so we can
     * stop as soon as that lower bound exceeds the best hit. */
    int ccx = clampi((int)(x * s->inv_ccell), 0, s->cgw - 1);
    int ccy = clampi((int)(y * s->inv_ccell), 0, s->cgh - 1);
    int max_ring = (int)(r_max * s->inv_ccell) + 1;
    for (int ring = 0; ring <= max_ring; ring++) {
        if (best >= 0) {
            float lb = (float)(ring - 1) * s->ccell;
            if (lb > 0.0f && lb * lb > best_d2) break;
        }
        int x0 = ccx - ring, x1 = ccx + ring;
        int y0 = ccy - ring, y1 = ccy + ring;
        for (int cy = y0; cy <= y1; cy++) {
            if (cy < 0 || cy >= s->cgh) continue;
            /* full rows on top/bottom of the ring, only the two side
             * columns in between (visit each ring cell exactly once) */
            int step = (cy == y0 || cy == y1) ? 1 : (x1 - x0 > 0 ? x1 - x0 : 1);
            for (int cx = x0; cx <= x1; cx += step) {
                if (cx < 0 || cx >= s->cgw) continue;
                int c = cy * s->cgw + cx;
                for (int k = s->cstart[c]; k < s->cstart[c + 1]; k++) {
                    int i = s->corder[k];
                    if (i >= s->count) continue;           /* corpse ghost */
                    float dx = s->px[i] - x, dy = s->py[i] - y;
                    float d2 = dx * dx + dy * dy;
                    if (d2 <= best_d2) { best_d2 = d2; best = i; }
                }
            }
        }
    }
    return best;
}

static inline void impulse_one(SimP *s, int i, float x, float y,
                               float radius, float strength, float up_ratio) {
    float dx = s->px[i] - x, dy = s->py[i] - y;
    float d2 = dx * dx + dy * dy;
    if (d2 > radius * radius) return;
    float d = sqrtf(d2);
    float fall = 1.0f - d / radius;
    float nx, ny;
    if (d > 1e-5f) { nx = dx / d; ny = dy / d; }
    else { nx = rng_fsym(&s->rng); ny = rng_fsym(&s->rng);
           float l = sqrtf(nx*nx + ny*ny) + 1e-6f; nx /= l; ny /= l; }
    s->vx[i] += strength * fall * nx;
    s->vy[i] += strength * fall * ny;
    if (up_ratio > 0.0f) {
        s->vz[i] += strength * fall * up_ratio;
        if (s->vz[i] > TAKEOFF_VZ) s->aflags[i] |= SIMP_FLYING;
    }
}

void simp_apply_impulse_ex(SimP *s, float x, float y, float radius,
                           float strength, float up_ratio) {
    /* gridded path skips corpse ghosts and (being unbinned) the already-
     * airborne; the brute-force fallback boosts the airborne too */
    if (!s->grid_stale && s->grid_total == s->count) {
        int x0 = clampi((int)((x - radius) * s->inv_ccell), 0, s->cgw - 1);
        int x1 = clampi((int)((x + radius) * s->inv_ccell), 0, s->cgw - 1);
        int y0 = clampi((int)((y - radius) * s->inv_ccell), 0, s->cgh - 1);
        int y1 = clampi((int)((y + radius) * s->inv_ccell), 0, s->cgh - 1);
        for (int cy = y0; cy <= y1; cy++) for (int cx = x0; cx <= x1; cx++) {
            int c = cy * s->cgw + cx;
            for (int k = s->cstart[c]; k < s->cstart[c + 1]; k++) {
                int i = s->corder[k];
                if (i >= s->count) continue;               /* corpse ghost */
                impulse_one(s, i, x, y, radius, strength, up_ratio);
            }
        }
    } else {
        for (int i = 0; i < s->count; i++)
            impulse_one(s, i, x, y, radius, strength, up_ratio);
    }
}

void simp_apply_impulse(SimP *s, float x, float y, float radius, float strength) {
    simp_apply_impulse_ex(s, x, y, radius, strength, 0.0f);
}

/* ------------------------------------------------------------- corpses */

int simp_corpse_add(SimP *s, float x, float y, float radius, float ttl) {
    int i;
    if (s->corpse_count < CORPSE_CAP) i = s->corpse_count++;
    else {
        /* full: replace the closest-to-expiry corpse (cost guarantee) */
        i = 0;
        for (int j = 1; j < CORPSE_CAP; j++)
            if (s->cttl[j] < s->cttl[i]) i = j;
    }
    s->cpx[i] = x; s->cpy[i] = y;
    s->crad[i] = radius; s->cttl[i] = ttl;
    if (radius > s->corpse_rmax) s->corpse_rmax = radius;
    /* same constraint as simp_spawn_desc: the PBD 5-cell pair search only
     * sees discs whose radius fits the grid cell */
    if (radius > s->grid_rmax) {
        s->grid_rmax = radius;
        s->ccell = 2.0f * s->grid_rmax * 1.05f;
        s->inv_ccell = 1.0f / s->ccell;
        s->cgw = (int)ceilf(s->world_w * s->inv_ccell) + 1;
        s->cgh = (int)ceilf(s->world_h * s->inv_ccell) + 1;
    }
    s->grid_stale = true;        /* binned as a ghost at the next rebuild */
    return i;
}

int simp_corpse_count(const SimP *s) { return s->corpse_count; }
const float *simp_corpse_px(const SimP *s)  { return s->cpx; }
const float *simp_corpse_py(const SimP *s)  { return s->cpy; }
const float *simp_corpse_rad(const SimP *s) { return s->crad; }

/* --------------------------------------------------------------- stepping */

static void rebuild_grid(SimP *s) {
    const int nc = s->cgw * s->cgh;
    /* corpse ghosts: copy the pool past the live agents as invm=0 discs so
     * the PBD kernel treats them like any other (immovable) body */
    const int ng = s->corpse_count;
    for (int j = 0; j < ng; j++) {
        int g = s->count + j;
        s->px[g] = s->cpx[j]; s->py[g] = s->cpy[j];
        s->rad[g] = s->crad[j];
        s->invm[g] = 0.0f;
        s->seed[g] = 0xC0FF1234u + (uint32_t)j;   /* determinism (d==0 path) */
    }
    const int total = s->count + ng;
    const uint8_t *fl = s->aflags;
    memset(s->ccount, 0, (size_t)(nc + 1) * sizeof(int));
    for (int i = 0; i < total; i++) {
        if (i < s->count && (fl[i] & SIMP_FLYING)) continue;  /* airborne */
        int cx = clampi((int)(s->px[i] * s->inv_ccell), 0, s->cgw - 1);
        int cy = clampi((int)(s->py[i] * s->inv_ccell), 0, s->cgh - 1);
        s->ccount[cy * s->cgw + cx]++;
    }
    int acc = 0;
    for (int c = 0; c < nc; c++) { s->cstart[c] = acc; acc += s->ccount[c]; }
    s->cstart[nc] = acc;
    memcpy(s->ccount, s->cstart, (size_t)(nc + 1) * sizeof(int)); /* reuse as cursor */
    for (int i = 0; i < total; i++) {
        if (i < s->count && (fl[i] & SIMP_FLYING)) continue;
        int cx = clampi((int)(s->px[i] * s->inv_ccell), 0, s->cgw - 1);
        int cy = clampi((int)(s->py[i] * s->inv_ccell), 0, s->cgh - 1);
        s->corder[s->ccount[cy * s->cgw + cx]++] = i;
    }
    s->grid_total = s->count;
    s->grid_ghosts = ng;
    s->grid_stale = false;
}

static void pbd_iteration(SimP *s) {
    const int cgw = s->cgw, cgh = s->cgh;
    /* Gauss-Seidel over cells; for each agent, look at its own cell and the
     * 4 forward neighbours (E, SW, S, SE) so every pair is visited once. */
    static const int NX[5] = { 0, 1, -1, 0, 1 };
    static const int NY[5] = { 0, 0,  1, 1, 1 };
    for (int cy = 0; cy < cgh; cy++) {
        for (int cx = 0; cx < cgw; cx++) {
            int c = cy * cgw + cx;
            int beg = s->cstart[c], end = s->cstart[c + 1];
            for (int a = beg; a < end; a++) {
                int i = s->corder[a];
                float pix = s->px[i], piy = s->py[i];
                float ri = s->rad[i], wi = s->invm[i];
                for (int k = 0; k < 5; k++) {
                    int nx = cx + NX[k], ny = cy + NY[k];
                    if (nx < 0 || ny < 0 || nx >= cgw || ny >= cgh) continue;
                    int nc2 = ny * cgw + nx;
                    int b0 = (k == 0) ? a + 1 : s->cstart[nc2];
                    int b1 = s->cstart[nc2 + 1];
                    for (int b = b0; b < b1; b++) {
                        int j = s->corder[b];
                        float dx = s->px[j] - pix, dy = s->py[j] - piy;
                        float rsum = ri + s->rad[j];
                        float d2 = dx * dx + dy * dy;
                        if (d2 >= rsum * rsum) continue;
                        float wj = s->invm[j];
                        if (wi + wj <= 0.0f) continue;  /* corpse vs corpse */
                        float d = sqrtf(d2);
                        float nxv, nyv;
                        if (d > 1e-5f) { nxv = dx / d; nyv = dy / d; }
                        else {
                            uint32_t *st = &s->seed[i];
                            nxv = rng_fsym(st); nyv = rng_fsym(st);
                            float l = sqrtf(nxv*nxv + nyv*nyv) + 1e-6f;
                            nxv /= l; nyv /= l; d = 0.0f;
                        }
                        float overlap = rsum - d;
                        /* robustness: never resolve more than 30% of the
                         * combined radius per pair per iteration; deep
                         * overlaps relax over a few steps instead of
                         * exploding into huge recovered velocities */
                        float capd = 0.30f * rsum;
                        if (overlap > capd) overlap = capd;
                        float inv = 1.0f / (wi + wj);
                        float ci = overlap * wi * inv;
                        float cj = overlap * wj * inv;
                        pix -= nxv * ci; piy -= nyv * ci;
                        s->px[j] += nxv * cj; s->py[j] += nyv * cj;
                        s->diag_overlap_sum += overlap;
                        s->diag_overlap_n++;
                    }
                }
                s->px[i] = pix; s->py[i] = piy;
            }
        }
    }
}

static void wall_projection(SimP *s) {
    const float wall_h = s->params.wall_h;
    for (int i = 0; i < s->count; i++) {
        float r = s->rad[i];
        /* above wall_h walls are overflown; low flyers slam into them */
        if (!((s->aflags[i] & SIMP_FLYING) && s->z[i] > wall_h)) {
            float d = simp_sample_sdf(s, s->px[i], s->py[i]);
            if (d < r) {
                float gx, gy;
                sdf_grad(s, s->px[i], s->py[i], &gx, &gy);
                float push = r - d;
                s->px[i] += gx * push;
                s->py[i] += gy * push;
            }
        }
        /* hard world bounds (flyers included: they slide along the edge) */
        s->px[i] = clampf(s->px[i], r, s->world_w - r);
        s->py[i] = clampf(s->py[i], r, s->world_h - r);
    }
}

int simp_step(SimP *s, float dt) {
    const SimPParams *P = &s->params;
    /* navigation: terrain edits force a full commit (phi+flow+SDF, reads the
     * current rho_s/cost_user); otherwise phi+flow alone are refreshed on a
     * throttle whenever density matters or user costs changed (M3.6) */
    if (s->nav_dirty) {
        nav_commit(s);
        s->flow_timer = 0.0f;
        s->cost_dirty = false;
    } else if (P->flow_period > 0.0f) {
        s->flow_timer += dt;
        if (s->flow_timer >= P->flow_period) {
            s->flow_timer = 0.0f;
            bool density_on = P->k_density > 0.0f &&
                (s->count > 0 || s->corpse_count > 0 || s->rho_active);
            if (density_on || s->cost_dirty) {
                density_update(s);
                recompute_phi(s);
                recompute_flow(s);
                s->cost_dirty = false;
            }
        }
    }
    const float amax_dt = P->a_max * dt;
    const float damp = clampf(1.0f - P->damping * dt, 0.0f, 1.0f);

    s->diag_overlap_sum = 0.0f;
    s->diag_overlap_n = 0;

    s->landed_n = 0;

    /* 1) steering toward flow field + noise, bounded acceleration.
     * Dormant agents steer toward zero velocity: they stand still, but a
     * push (PBD, impulse) still moves them and they brake back to rest.
     * Flying agents are ballistic: no steering, no damping. */
    for (int i = 0; i < s->count; i++) {
        uint8_t fl = s->aflags[i];
        if (fl & SIMP_FLYING) continue;
        float fx = 0.0f, fy = 0.0f;
        if (!(fl & SIMP_DORMANT)) {
            simp_sample_flow(s, s->px[i], s->py[i], &fx, &fy);
            if (P->noise_ang > 0.0f && (fx != 0.0f || fy != 0.0f)) {
                float a = P->noise_ang * rng_fsym(&s->seed[i]);
                float ca = cosf(a), sa = sinf(a);
                float rx = fx * ca - fy * sa, ry = fx * sa + fy * ca;
                fx = rx; fy = ry;
            }
        }
        float dvx = fx * s->vpref[i] - s->vx[i];
        float dvy = fy * s->vpref[i] - s->vy[i];
        float dl = sqrtf(dvx * dvx + dvy * dvy);
        if (dl > amax_dt) { float k = amax_dt / dl; dvx *= k; dvy *= k; }
        s->vx[i] = (s->vx[i] + dvx) * damp;
        s->vy[i] = (s->vy[i] + dvy) * damp;
    }

    /* 2) integrate (save pre-projection position); fly the fake z axis */
    for (int i = 0; i < s->count; i++) {
        s->qx[i] = s->px[i]; s->qy[i] = s->py[i];
        s->px[i] += s->vx[i] * dt;
        s->py[i] += s->vy[i] * dt;
        if (s->aflags[i] & SIMP_FLYING) {
            s->vz[i] -= GRAV * dt;
            s->z[i]  += s->vz[i] * dt;
            if (s->z[i] <= 0.0f) {                       /* touchdown */
                s->z[i] = 0.0f; s->vz[i] = 0.0f;
                s->aflags[i] &= (uint8_t)~SIMP_FLYING;
                s->vx[i] *= P->landing_damp;
                s->vy[i] *= P->landing_damp;
                /* recovered velocity is (px-qx)/dt: bend qx so the recovery
                 * yields the damped speed, or the damp would be overwritten */
                s->qx[i] = s->px[i] - s->vx[i] * dt;
                s->qy[i] = s->py[i] - s->vy[i] * dt;
                s->landed[s->landed_n++] = simp_handle_of(s, i);
            }
        }
    }

    /* 3) constraints: agent-agent (PBD) + walls, iterated */
    rebuild_grid(s);
    for (int it = 0; it < P->pbd_iters; it++) {
        pbd_iteration(s);
        wall_projection(s);
    }

    /* 4) recover effective velocity from positional change. Flyers keep
     * their integrated velocity (they take no projections; clamping would
     * cut hard launches short). */
    const float inv_dt = 1.0f / dt;
    const float vc = P->v_clamp;
    for (int i = 0; i < s->count; i++) {
        if (s->aflags[i] & SIMP_FLYING) continue;
        float nvx = (s->px[i] - s->qx[i]) * inv_dt;
        float nvy = (s->py[i] - s->qy[i]) * inv_dt;
        float sp2 = nvx * nvx + nvy * nvy;
        if (sp2 > vc * vc) {
            float k = vc / sqrtf(sp2);
            nvx *= k; nvy *= k;
        }
        s->vx[i] = nvx;
        s->vy[i] = nvy;
    }

    /* 5) drain agents standing on goal cells (iterate backward: swap-and-pop).
     * Dormant agents are exempt: a sleeper shoved across a goal cell is not
     * "arriving" anywhere. */
    int drained = 0;
    for (int i = s->count - 1; i >= 0; i--) {
        if (s->aflags[i] & SIMP_DORMANT) continue;
        int cx = clampi((int)(s->px[i] * s->inv_cell), 0, s->gw - 1);
        int cy = clampi((int)(s->py[i] * s->inv_cell), 0, s->gh - 1);
        if (s->goal[cy * s->gw + cx]) { simp_kill(s, i); drained++; }
    }

    /* 5b) corpse decay: swap-and-pop on expiry (no handles point here) */
    for (int j = s->corpse_count - 1; j >= 0; j--) {
        s->cttl[j] -= dt;
        if (s->cttl[j] <= 0.0f) {
            int last = --s->corpse_count;
            s->cpx[j] = s->cpx[last]; s->cpy[j] = s->cpy[last];
            s->crad[j] = s->crad[last]; s->cttl[j] = s->cttl[last];
        }
    }
    if (s->corpse_count == 0) s->corpse_rmax = 0.0f;

    /* 6) rebuild the grid on final positions: the mid-step grid is stale by
     * the PBD/wall corrections (and by the drain), so spatial queries and
     * impulses between steps would miss agents that drifted across a cell
     * boundary. A counting sort is two passes over the agents: cheap. */
    rebuild_grid(s);
    return drained;
}

/* ------------------------------------------------------------ read access */

const float *simp_px(const SimP *s) { return s->px; }
const float *simp_py(const SimP *s) { return s->py; }
const float *simp_vx(const SimP *s) { return s->vx; }
const float *simp_vy(const SimP *s) { return s->vy; }
const float *simp_radius_arr(const SimP *s) { return s->rad; }
const uint8_t *simp_flags_arr(const SimP *s) { return s->aflags; }
const float *simp_z_arr(const SimP *s) { return s->z; }
int simp_landed_count(const SimP *s) { return s->landed_n; }
const SimPHandle *simp_landed(const SimP *s) { return s->landed; }

float simp_mean_overlap(const SimP *s) {
    return s->diag_overlap_n ? s->diag_overlap_sum / (float)s->diag_overlap_n : 0.0f;
}

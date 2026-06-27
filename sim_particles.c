/* sim_particles.c — implementation. See sim_particles.h for the model. */

#include "sim_particles.h"
#include "sim_threads.h"        /* M4: pthreads pool (header-only) */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <limits.h>

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
#define DRAG_CAP   1024       /* fixed draggable pool (DRAG_DESIGN.md) */
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
    float   *wall_cost;    /* gw*gh per-cell Dijkstra entry toll for wall cells;
                              default WALL_ENTER. Higher = the horde routes AWAY
                              and won't press here (palazzo/rock, indestructible);
                              lower-but-still >> open routes = the preferred siege
                              target (barricade). Only consulted when solid[i].
                              Collision (SDF) is independent of this — both tiers
                              are solid; this picks only WHERE the horde presses. */
    uint8_t *goal;         /* gw*gh, 1 = goal/drain cell */
    float   *phi;          /* cost-to-goal */
    float   *flow_x;       /* normalized direction field (0,0 in walls) */
    float   *flow_y;
    float   *sdf;          /* signed distance to walls (m), >0 = free */
    /* M4 wall-skip: per nav cell, 1 = a wall is close enough that an agent in
     * this cell COULD overlap it (min SDF over the 3x3 block <= grid_rmax+cell).
     * job_wall skips the bilinear SDF sample entirely where this is 0 (open
     * field): conservative, the bilinear value is a convex combo of 4 cell
     * centres all inside the 3x3, so the skip never misses a real contact.
     * Rebuilt on SDF change and when grid_rmax grows. */
    uint8_t *wall_near;
    bool     nav_dirty;

    /* navigation costs (M3.5 + M3.6) */
    float   *cost_user;    /* gw*gh additive user cost, clamped on write */
    float   *rho_raw;      /* gw*gh scratch histogram (agents per cell) */
    float   *rho_s;        /* gw*gh smoothed density (blur + EMA) */
    float   *jam_raw;      /* gw*gh scratch histogram (stillness-weighted) */
    float   *jam_s;        /* gw*gh smoothed stalled density (M3.7) */
    float   *cost_mult;    /* gw*gh edge multiplier, filled per recompute */
    bool     cost_dirty;   /* cost_user changed since last flow recompute */
    bool     rho_active;   /* rho_s still holds non-negligible mass */
    float    flow_timer;   /* seconds since last throttled recompute */

    /* preallocated nav scratch (recomputes run inside simp_step: no malloc) */
    void    *heap_nodes;   /* HNode[gw*gh*8] for the Dijkstra */
    float   *flow_tx, *flow_ty;

    /* M4: incremental Dijkstra. The throttled phi recompute is the worst frame
     * (serial, doesn't scale with threads), so it is drained in fixed-size
     * budgets across consecutive steps instead of all at once. phi is built into
     * its own buffer while agents keep reading the COMMITTED flow; when the heap
     * empties, flow is recomputed and committed. The schedule (fixed budget,
     * serial heap) is independent of thread count, so determinism holds. */
    int      nav_phase;    /* 0 = idle, 1 = a phi build is draining */
    int      nav_heap_n;   /* persistent heap size between budget chunks */
    int      nav_budget;   /* heap pops drained per step (gw*gh / NAV_BUDGET_DIV) */

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
    float *stun;           /* per-agent stun timer (s): >0 brakes steering to rest
                            * (hit flinch); decays per step, no goal pull meanwhile */
    float *z, *vz;         /* fake third axis for ballistic flight */

    /* agents landed during the last step (handles: drain-safe) */
    SimPHandle *landed;
    int landed_n;

    /* siege sensor: per-agent wall contact of the last step (SIEGE_DESIGN.md).
     * Filled on the final wall_projection pass; indexed by dense index. */
    float *wall_pressure;  /* cap : push * max(into_wall,0), 0 = no siege */
    int   *wall_cell;      /* cap : besieged nav cell, -1 = none          */

    /* corpse pool: static obstacle discs, TTL-managed */
    int   corpse_count;
    float *cpx, *cpy, *crad, *cttl;
    float corpse_rmax;     /* largest radius ever in pool (search reach) */

    /* corpse pile -> cost (CORPSE_DESIGN.md §7-bis). Per nav cell, accumulated
     * VOLUME (mass, += pi*r^2 per corpse, slow decay) and packing (pack, += per
     * trampling agent, faster decay). Derived height = k_h*mass/(1+k_pack*pack)
     * feeds the k_corpse nav-cost term. Independent of the TTL pool: a corpse
     * adds volume that then rots on its own clock. */
    float *corpse_mass;    /* gw*gh */
    float *corpse_pack;    /* gw*gh */
    float *corpse_height;  /* gw*gh, derived, refreshed end of step */
    bool   corpse_active;  /* any pile field still non-negligible */

    /* draggable pool (DRAG_DESIGN.md): finite-mass discs with momentum + friction,
     * shoved by the crowd and colliding with walls / each other. Ghosts like
     * corpses but carrying their own velocity; persistent (no TTL). dq = position
     * before this step's integration, for velocity recovery from the PBD shove. */
    int   drag_count;
    float *dpx, *dpy, *dvx, *dvy, *drad, *dinvm, *dqx, *dqy;
    float drag_rmax;       /* largest draggable radius (collision-grid reach) */

    /* blood-fear "danger" field (CORPSE_DESIGN.md, 2026-06-25): the anti-choke
     * pivot. Deposited at death alongside the blood decal, decays with
     * danger_hl; the k_danger nav-cost term routes the horde AWAY from cells
     * soaked in other zombies' blood (animal instinct). Same machinery shape as
     * the corpse fields, but a single decaying scalar. */
    float *danger;         /* gw*gh */
    bool   danger_active;  /* danger still non-negligible somewhere */

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
    int   grid_drag0;      /* first draggable-ghost index (= count+corpse_count);
                              ghosts in [count, grid_drag0) are corpses, the PBD
                              skips only corpse-corpse pairs */
    bool  grid_stale;      /* set by kill/corpse_add: indices unreliable */

    /* M4: periodic cache-locality reorder. The collision grid is sparse and the
     * sim is memory-bound (scattered corder writes, neighbour px/py reads in the
     * PBD). Every REORDER_PERIOD steps the SoA is permuted into grid-cell order
     * so agents close in space are close in memory. Serial + deterministic; it
     * changes the within-cell PBD order (checksum shifts, like the tile change)
     * but stays identical across thread counts. */
    int  *reorder_perm;    /* cap : perm[new index] = old index */
    void *reorder_tmp;     /* cap * 4B : scratch for one array permute */
    int   reorder_ctr;     /* steps since last reorder */
    int   reorder_period;  /* steps between reorders; <=0 disables (SIMP_REORDER) */

    SimPParams params;
    uint32_t rng;          /* sim-level RNG (spawn jitter) */
    float diag_overlap_sum;
    int   diag_overlap_n;

    /* M4: thread pool + per-worker scratch (reduced deterministically) */
    SimPool *pool;
    int      nthreads;
    double  *diag_sum_w;   /* nthreads : overlap sum partials */
    long    *diag_n_w;     /* nthreads : overlap count partials */
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

/* M4: a throttled phi recompute is drained in ~this many per-step budgets to
 * cap the worst-frame cost. Bigger = finishes sooner but taller spike. */
#define NAV_BUDGET_DIV 6

/* M4: steps between cache-locality reorders of the agent SoA (see reorder_agents).
 * Agents drift ~2 collision cells over this window, so locality stays decent
 * between reorders while the amortized cost is negligible. */
#define REORDER_PERIOD 60

/* Seed a fresh phi build: reset phi, rebuild the per-destination edge multiplier
 * (M3.5 user cost + M3.6 density), push the goals onto the heap. The heap is left
 * persistent in s->heap_nodes (size in s->nav_heap_n) for nav_phi_drain. */
static void nav_phi_begin(SimP *s) {
    const int n = s->gw * s->gh;
    for (int i = 0; i < n; i++) s->phi[i] = PHI_INF;

    /* per-destination edge multiplier (M3.5 user cost + M3.6 density).
     * rho_max = packing density of a nav cell (agents whose center fits) */
    const float kd = s->params.k_density;
    const float r0 = s->params.radius;
    const float inv_rho_max =
        (3.14159265f * r0 * r0) / (s->cell * s->cell * 0.7f);
    const float kj = s->params.k_jam;
    /* §7-bis: a dense corpse pile climbs toward the k_corpse ceiling, saturating
     * at wall_h (a "wall of corpses"). Stays below WALL_ENTER so palazzi/sealed
     * sieges are untouched, but can top a barricade's per-cell breakthrough toll
     * -> the Dijkstra reroutes the horde to break the barricade. */
    const float kc = s->params.k_corpse;
    const float inv_wall_h = (s->params.wall_h > 0.0f) ? 1.0f / s->params.wall_h : 0.0f;
    /* blood-fear (2026-06-25): GRADUATED term k_danger*min(danger/danger_ref,1).
     * Light blood -> soft reroute between open paths (the PBD still shoves the
     * front in, so a big horde floods through); a sustained killbox saturates to
     * the wall-scale k_danger ceiling, so the horde breaks the player's barricades
     * instead of feeding the box (anti-killbox). Stays < WALL_ENTER per cell. */
    const float kdg = s->params.k_danger;
    const float inv_dref = (s->params.danger_ref > 0.0f) ? 1.0f / s->params.danger_ref : 0.0f;
    for (int i = 0; i < n; i++) {
        float m = 1.0f + s->cost_user[i];
        if (kd > 0.0f) {
            float t = s->rho_s[i] * inv_rho_max;
            m += kd * (t > 1.0f ? 1.0f : t);
        }
        if (kj > 0.0f) {
            float t = s->jam_s[i] * inv_rho_max;
            m += kj * (t > 1.0f ? 1.0f : t);
        }
        if (kc > 0.0f) {
            float t = s->corpse_height[i] * inv_wall_h;
            m += kc * (t > 1.0f ? 1.0f : t);
        }
        if (kdg > 0.0f) {
            float t = s->danger[i] * inv_dref;
            m += kdg * (t > 1.0f ? 1.0f : t);
        }
        s->cost_mult[i] = m < 0.2f ? 0.2f : m;
    }

    /* heap scratch is preallocated: pushes are bounded by successful phi
     * improvements, n*8 has ample slack over the n*4 seen in practice */
    Heap h; h.a = (HNode *)s->heap_nodes; h.n = 0;
    for (int i = 0; i < n; i++)
        if (s->goal[i] && !s->solid[i]) { s->phi[i] = 0.0f; heap_push(&h, 0.0f, i); }
    s->nav_heap_n = h.n;
}

/* Drain up to `budget` heap pops of the in-progress phi build (Dijkstra). Stops
 * early if the heap empties (build complete). Resumes from the persistent heap on
 * the next call: the result is bit-identical to draining it all at once, only
 * spread across steps. budget = INT_MAX runs it to completion in one call. */
static void nav_phi_drain(SimP *s, int budget) {
    static const int   DX[8] = { 1,-1, 0, 0, 1, 1,-1,-1 };
    static const int   DY[8] = { 0, 0, 1,-1, 1,-1, 1,-1 };
    static const float DC[8] = { 1,1,1,1, 1.41421356f,1.41421356f,1.41421356f,1.41421356f };

    Heap h; h.a = (HNode *)s->heap_nodes; h.n = s->nav_heap_n;
    int popped = 0;
    while (h.n > 0 && popped < budget) {
        HNode nd = heap_pop(&h); popped++;
        if (nd.c > s->phi[nd.i]) continue;          /* stale entry */
        int cx = nd.i % s->gw, cy = nd.i / s->gw;
        for (int k = 0; k < 8; k++) {
            int nx = cx + DX[k], ny = cy + DY[k];
            if (nx < 0 || ny < 0 || nx >= s->gw || ny >= s->gh) continue;
            int j = ny * s->gw + nx;
            float nc = nd.c + DC[k] * s->cost_mult[j];
            if (s->solid[j]) nc += s->wall_cost[j];   /* per-cell breakthrough toll */
            /* diagonal corner-cutting through walls pays the dearer corner's toll */
            else if (k >= 4) {
                int ca = cy * s->gw + nx, cb = ny * s->gw + cx;
                float toll = 0.0f;
                if (s->solid[ca]) toll = s->wall_cost[ca];
                if (s->solid[cb] && s->wall_cost[cb] > toll) toll = s->wall_cost[cb];
                nc += toll;
            }
            if (nc < s->phi[j]) { s->phi[j] = nc; heap_push(&h, nc, j); }
        }
    }
    s->nav_heap_n = h.n;
}

/* full synchronous phi (terrain edits must take effect immediately) */
static void recompute_phi(SimP *s) {
    nav_phi_begin(s);
    nav_phi_drain(s, INT_MAX);
}

/* direction = toward the lowest-phi reachable neighbour (M4: per-cell parallel) */
static void job_flow_dir(void *arg, int worker, int begin, int end) {
    (void)worker;
    SimP *s = arg; const int gw = s->gw, gh = s->gh;
    for (int i = begin; i < end; i++) {
        int cx = i % gw, cy = i / gw;
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

/* one smoothing pass (skip walls), then renormalize, into flow_tx/flow_ty */
static void job_flow_smooth(void *arg, int worker, int begin, int end) {
    (void)worker;
    SimP *s = arg; const int gw = s->gw, gh = s->gh;
    float *tx = s->flow_tx, *ty = s->flow_ty;
    for (int i = begin; i < end; i++) {
        int cx = i % gw, cy = i / gw;
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

static void recompute_flow(SimP *s) {
    const int n = s->gw * s->gh;
    /* two passes (smooth reads what dir wrote): the parallel_for return is the
     * barrier between them. Both per-cell, deterministic for any thread count. */
    simpool_parallel_for(s->pool, n, 4096, job_flow_dir, s);
    simpool_parallel_for(s->pool, n, 4096, job_flow_smooth, s);
    memcpy(s->flow_x, s->flow_tx, sizeof(float) * (size_t)n);
    memcpy(s->flow_y, s->flow_ty, sizeof(float) * (size_t)n);
}

/* M3.6: refresh the smoothed density field from current agent positions.
 * Fresh histogram (grounded agents + corpse weight), 3x3 blur folded into a
 * temporal EMA: rho_s tracks the crowd with ~2 recompute periods of lag,
 * which is exactly what stops the flow from flickering between routes. */
/* per-cell 3x3 blur of rho_raw/jam_raw folded into the rho_s/jam_s EMA (M4) */
static void job_density_blur(void *arg, int worker, int begin, int end) {
    (void)worker;
    SimP *s = arg; const int gw = s->gw, gh = s->gh;
    for (int i = begin; i < end; i++) {
        int cx = i % gw, cy = i / gw;
        float acc = 0.0f, jacc = 0.0f; int cnt = 0;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int nx = cx + dx, ny = cy + dy;
            if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
            acc  += s->rho_raw[ny * gw + nx];
            jacc += s->jam_raw[ny * gw + nx]; cnt++;
        }
        s->rho_s[i] = s->rho_s[i] * (1.0f - RHO_EMA) + (acc / (float)cnt) * RHO_EMA;
        s->jam_s[i] = s->jam_s[i] * (1.0f - RHO_EMA) + (jacc / (float)cnt) * RHO_EMA;
    }
}

static void density_update(SimP *s) {
    const int gw = s->gw, gh = s->gh, n = gw * gh;
    memset(s->rho_raw, 0, (size_t)n * sizeof(float));
    memset(s->jam_raw, 0, (size_t)n * sizeof(float));
    for (int i = 0; i < s->count; i++) {
        if (s->aflags[i] & SIMP_FLYING) continue;
        int cx = clampi((int)(s->px[i] * s->inv_cell), 0, gw - 1);
        int cy = clampi((int)(s->py[i] * s->inv_cell), 0, gh - 1);
        s->rho_raw[cy * gw + cx] += 1.0f;
        /* M3.7: stillness = how far below preferred speed the agent actually
         * moves (recovered velocity, so crowd pressure counts). Squared, so
         * slow-but-flowing crowd barely registers while a stopped queue gets
         * full weight. Dormant packs read as fully stalled: intended, the
         * flow routes around sleeping obstacles. */
        float sp = sqrtf(s->vx[i] * s->vx[i] + s->vy[i] * s->vy[i]);
        float still = 1.0f - sp / s->vpref[i];
        if (still > 0.0f) s->jam_raw[cy * gw + cx] += still * still;
        /* §7-bis: a grounded agent over a piled cell tramples it (packing rises
         * -> pile flattens). Cheap, we already have the cell here. Per recompute
         * (flow_period cadence), absorbed by pack_inc; decay is per-step in 5b.
         * NOTE: with the corpses still at INFINITE mass (this slice), an agent's
         * center can't share a 0.5 m cell with a corpse disc — PBD pushes it out
         * — so packing rarely fires; it comes alive with the finite-mass §3 slice
         * (agents cresting the pile). The path is wired and correct now. */
        if (s->corpse_mass[cy * gw + cx] > 1e-4f)
            s->corpse_pack[cy * gw + cx] += s->params.pack_inc;
    }
    for (int j = 0; j < s->corpse_count; j++) {
        int cx = clampi((int)(s->cpx[j] * s->inv_cell), 0, gw - 1);
        int cy = clampi((int)(s->cpy[j] * s->inv_cell), 0, gh - 1);
        s->rho_raw[cy * gw + cx] += CORPSE_RHO;
        s->jam_raw[cy * gw + cx] += CORPSE_RHO;
    }
    /* 3x3 blur folded into the EMA, per cell (M4: parallel). The histogram
     * scatter above stays serial (cheap, and a parallel scatter would need
     * atomics/per-thread bins to stay deterministic). */
    simpool_parallel_for(s->pool, n, 4096, job_density_blur, s);

    /* peak feeds rho_active only; a plain serial reduction keeps it deterministic
     * (40k reads, negligible next to the blur it follows) */
    float peak = 0.0f;
    for (int i = 0; i < n; i++) if (s->rho_s[i] > peak) peak = s->rho_s[i];
    /* keeps the throttled recompute alive until an emptied map decays out
     * (jam_raw <= rho_raw cell-wise, so the rho peak covers both fields) */
    s->rho_active = peak > 0.01f;
}

static void build_wall_near(SimP *s);

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
    build_wall_near(s);
}

/* M4 wall-skip mask: flag a nav cell when a wall is close enough that an agent
 * inside it could overlap (min SDF over the 3x3 block <= grid_rmax + cell). The
 * 3x3 min covers the 2x2 footprint of the bilinear sample for any point in the
 * cell, so a cleared cell guarantees SDF(agent) > r_max everywhere in it. */
static void build_wall_near(SimP *s) {
    const int gw = s->gw, gh = s->gh;
    const float *d = s->sdf;
    const float thr = s->grid_rmax + s->cell;   /* one extra cell of slack */
    for (int cy = 0; cy < gh; cy++) for (int cx = 0; cx < gw; cx++) {
        float mn = d[cy * gw + cx];
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int nx = cx + dx, ny = cy + dy;
            if (nx < 0 || ny < 0 || nx >= gw || ny >= gh) continue;
            float v = d[ny * gw + nx];
            if (v < mn) mn = v;
        }
        s->wall_near[cy * gw + cx] = (mn <= thr) ? 1 : 0;
    }
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
    s->params.k_jam     = 8.0f;
    s->params.flow_period = 0.5f;
    /* §7-bis corpse->nav cost RETIRED (2026-06-25): the anti-choke pivot moved to
     * blood-fear (k_danger below). Machinery kept but OFF by default; callers that
     * still want it set k_corpse explicitly (e.g. test_corpse_pile). */
    s->params.k_corpse  = 0.0f;
    s->params.k_h       = 1.0f;
    s->params.k_pack    = 0.5f;
    s->params.corpse_mass_hl = 30.0f;
    s->params.corpse_pack_hl = 10.0f;
    s->params.pack_inc  = 1.0f;
    s->params.corpse_weight = 40.0f;
    /* blood-fear: GRADUATED to wall-scale (anti-killbox). Light blood = soft
     * reroute, a sustained killbox saturates to k_danger (out-costs a barricade).
     * half-life matched to the blood decal lifetime (~30 s). */
    s->params.k_danger  = 400.0f;
    s->params.danger_ref = 8.0f;
    s->params.danger_hl = 30.0f;
    s->params.drag_damp = 4.0f;       /* draggable friction (DRAG_DESIGN.md) */

    const size_t n = (size_t)gw * gh;
    s->solid  = (uint8_t *)calloc(n, 1);
    s->wall_cost = (float *)malloc(n * sizeof(float));
    for (size_t i = 0; i < n; i++) s->wall_cost[i] = WALL_ENTER;
    s->goal   = (uint8_t *)calloc(n, 1);
    s->phi    = (float *)malloc(n * sizeof(float));
    s->flow_x = (float *)calloc(n, sizeof(float));
    s->flow_y = (float *)calloc(n, sizeof(float));
    s->sdf    = (float *)malloc(n * sizeof(float));
    s->wall_near = (uint8_t *)malloc(n);
    s->cost_user = (float *)calloc(n, sizeof(float));
    s->rho_raw   = (float *)calloc(n, sizeof(float));
    s->rho_s     = (float *)calloc(n, sizeof(float));
    s->jam_raw   = (float *)calloc(n, sizeof(float));
    s->jam_s     = (float *)calloc(n, sizeof(float));
    s->cost_mult = (float *)malloc(n * sizeof(float));
    s->heap_nodes = malloc(n * 8 * sizeof(HNode));
    s->flow_tx = (float *)malloc(n * sizeof(float));
    s->flow_ty = (float *)malloc(n * sizeof(float));
    s->cost_dirty = false;
    s->rho_active = false;
    s->flow_timer = 0.0f;
    s->nav_phase = 0;
    /* spread the throttled phi build over ~NAV_BUDGET_DIV steps so no single
     * frame eats the whole Dijkstra; well under flow_period (~30 steps) so a
     * build always finishes before the next throttle tick */
    s->nav_budget = (int)(n / NAV_BUDGET_DIV); if (s->nav_budget < 1) s->nav_budget = 1;
    s->reorder_ctr = 0;
    s->reorder_period = REORDER_PERIOD;
    { const char *e = getenv("SIMP_REORDER"); if (e) s->reorder_period = atoi(e); }

    s->cap = max_agents;
    /* px/py/rad/invm/seed carry CORPSE_CAP + DRAG_CAP extra slots: corpse and
     * draggable ghosts are appended there before binning so the PBD kernel sees
     * them as plain discs (no special cases in the hot loop) */
    const size_t capg = (size_t)max_agents + CORPSE_CAP + DRAG_CAP;
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
    s->stun = (float *)calloc((size_t)max_agents, sizeof(float));
    s->z  = (float *)calloc((size_t)max_agents, sizeof(float));
    s->vz = (float *)calloc((size_t)max_agents, sizeof(float));
    s->landed = (SimPHandle *)malloc((size_t)max_agents * sizeof(SimPHandle));
    s->landed_n = 0;

    s->wall_pressure = (float *)calloc((size_t)max_agents, sizeof(float));
    s->wall_cell     = (int *)malloc((size_t)max_agents * sizeof(int));

    s->cpx  = (float *)malloc(CORPSE_CAP * sizeof(float));
    s->cpy  = (float *)malloc(CORPSE_CAP * sizeof(float));
    s->crad = (float *)malloc(CORPSE_CAP * sizeof(float));
    s->cttl = (float *)malloc(CORPSE_CAP * sizeof(float));
    s->corpse_count = 0;
    s->corpse_rmax = 0.0f;
    s->dpx   = (float *)malloc(DRAG_CAP * sizeof(float));
    s->dpy   = (float *)malloc(DRAG_CAP * sizeof(float));
    s->dvx   = (float *)calloc(DRAG_CAP, sizeof(float));
    s->dvy   = (float *)calloc(DRAG_CAP, sizeof(float));
    s->drad  = (float *)malloc(DRAG_CAP * sizeof(float));
    s->dinvm = (float *)malloc(DRAG_CAP * sizeof(float));
    s->dqx   = (float *)malloc(DRAG_CAP * sizeof(float));
    s->dqy   = (float *)malloc(DRAG_CAP * sizeof(float));
    s->drag_count = 0;
    s->drag_rmax = 0.0f;
    s->corpse_mass   = (float *)calloc(n, sizeof(float));
    s->corpse_pack   = (float *)calloc(n, sizeof(float));
    s->corpse_height = (float *)calloc(n, sizeof(float));
    s->corpse_active = false;
    s->danger        = (float *)calloc(n, sizeof(float));
    s->danger_active = false;

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
    s->reorder_perm = (int *)malloc((size_t)max_agents * sizeof(int));
    s->reorder_tmp  = malloc((size_t)max_agents * sizeof(float)); /* 4B covers every per-agent array */
    s->grid_total = 0;
    s->grid_ghosts = 0;
    s->grid_drag0 = 0;
    s->grid_stale = false;

    s->rng = 0x9E3779B9u;
    s->nav_dirty = true;

    /* M4: thread pool. Default = online CPUs (env SIMP_THREADS overrides);
     * results are identical for any thread count (colored tiles = disjoint
     * writes), so this is purely a speed knob. */
    s->nthreads = simpool_default_threads();
    s->pool = simpool_create(s->nthreads);
    s->nthreads = simpool_nthreads(s->pool);
    s->diag_sum_w = (double *)calloc((size_t)s->nthreads, sizeof(double));
    s->diag_n_w   = (long *)calloc((size_t)s->nthreads, sizeof(long));
    return s;
}

void simp_destroy(SimP *s) {
    if (!s) return;
    free(s->solid); free(s->wall_cost); free(s->goal); free(s->phi);
    free(s->flow_x); free(s->flow_y); free(s->sdf); free(s->wall_near);
    free(s->cost_user); free(s->rho_raw); free(s->rho_s); free(s->cost_mult);
    free(s->jam_raw); free(s->jam_s);
    free(s->heap_nodes); free(s->flow_tx); free(s->flow_ty);
    free(s->px); free(s->py); free(s->qx); free(s->qy);
    free(s->vx); free(s->vy);
    free(s->rad); free(s->vpref); free(s->invm); free(s->seed);
    free(s->aflags); free(s->stun); free(s->z); free(s->vz); free(s->landed);
    free(s->wall_pressure); free(s->wall_cell);
    free(s->cpx); free(s->cpy); free(s->crad); free(s->cttl);
    free(s->dpx); free(s->dpy); free(s->dvx); free(s->dvy);
    free(s->drad); free(s->dinvm); free(s->dqx); free(s->dqy);
    free(s->corpse_mass); free(s->corpse_pack); free(s->corpse_height); free(s->danger);
    free(s->slot_to_index); free(s->index_to_slot);
    free(s->slot_gen); free(s->slot_free);
    free(s->ccount); free(s->cstart); free(s->corder);
    free(s->reorder_perm); free(s->reorder_tmp);
    simpool_destroy(s->pool);
    free(s->diag_sum_w); free(s->diag_n_w);
    free(s);
}

/* Override the worker-thread count (>=1). Rebuilds the pool; results are
 * unchanged (deterministic across thread counts), only the speed differs. */
void simp_set_threads(SimP *s, int nthreads) {
    if (nthreads < 1) nthreads = 1;
    if (nthreads == s->nthreads) return;
    simpool_destroy(s->pool);
    s->nthreads = nthreads;
    s->pool = simpool_create(nthreads);
    s->nthreads = simpool_nthreads(s->pool);
    s->diag_sum_w = (double *)realloc(s->diag_sum_w, (size_t)s->nthreads * sizeof(double));
    s->diag_n_w   = (long *)realloc(s->diag_n_w, (size_t)s->nthreads * sizeof(long));
}
int simp_threads(const SimP *s) { return s->nthreads; }

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

/* Per-cell wall breakthrough toll (palazzo >> barricata). Only takes effect on
 * cells that are also solid (simp_set_wall). Clamped to >= 0; the caller keeps
 * it >> open path lengths (use simp_wall_base_cost as the reference tier).
 * Marks nav dirty: phi is rebuilt with the new toll. */
void simp_set_wall_cost(SimP *s, int cx, int cy, float cost) {
    if (cx < 0 || cy < 0 || cx >= s->gw || cy >= s->gh) return;
    s->wall_cost[cy * s->gw + cx] = cost < 0.0f ? 0.0f : cost;
    s->nav_dirty = true;
}
/* Default/uniform wall toll: the game builds its palazzo/barricata tiers
 * relative to this (e.g. palazzo = 10x, barricata = 1x). */
float        simp_wall_base_cost(void)         { return WALL_ENTER; }
const float *simp_wall_cost_arr(const SimP *s) { return s->wall_cost; }

const float *simp_user_cost(const SimP *s)   { return s->cost_user; }
const float *simp_density_arr(const SimP *s) { return s->rho_s; }
const float *simp_jam_arr(const SimP *s)     { return s->jam_s; }

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
    s->stun[i] = 0.0f;
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
        build_wall_near(s);   /* threshold grew: re-flag near-wall cells */
    }
    return i;
}

void simp_sleep(SimP *s, int i) {
    if (i < 0 || i >= s->count) return;
    if (s->aflags[i] & SIMP_FLYING) return;
    s->aflags[i] |= SIMP_DORMANT;
}

void simp_set_vpref(SimP *s, int i, float v_pref) {
    if (i < 0 || i >= s->count) return;
    s->vpref[i] = v_pref < 0.0f ? 0.0f : v_pref;
}

void simp_stun(SimP *s, int i, float duration) {
    if (i < 0 || i >= s->count) return;
    if (s->aflags[i] & SIMP_FLYING) return;        /* airborne: nothing to plant */
    if (duration <= 0.0f) { s->stun[i] = 0.0f; return; }
    s->stun[i] = duration;
    s->vx[i] = 0.0f; s->vy[i] = 0.0f;              /* drop self-momentum at once */
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
    s->stun[i] = s->stun[last];
    s->z[i] = s->z[last]; s->vz[i] = s->vz[last];
    s->wall_pressure[i] = s->wall_pressure[last];   /* keep siege sensor in sync */
    s->wall_cell[i] = s->wall_cell[last];
    int ls = s->index_to_slot[last];
    s->slot_to_index[ls] = i;
    s->index_to_slot[i] = ls;
}

int simp_count(const SimP *s) { return s->count; }
int simp_grid_w(const SimP *s) { return s->gw; }
int simp_grid_h(const SimP *s) { return s->gh; }
float simp_cell_size(const SimP *s) { return s->cell; }

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
        if (s->drag_rmax > rmax) rmax = s->drag_rmax;
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
    for (int j = 0; j < s->drag_count; j++) {
        float dx = s->dpx[j] - x, dy = s->dpy[j] - y;
        float rs = r + s->drad[j];
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

/* Distance along a normalized ray to the first solid nav cell, or maxd if the
 * ray reaches maxd / leaves the grid without hitting a wall. Amanatides-Woo
 * DDA over the nav grid (cell, solid[]). Line-of-sight occlusion for hitscan. */
static float wall_ray_t(const SimP *s, float ox, float oy,
                        float dx, float dy, float maxd) {
    int cx = (int)(ox * s->inv_cell), cy = (int)(oy * s->inv_cell);
    if (cx < 0 || cx >= s->gw || cy < 0 || cy >= s->gh) return maxd;
    if (s->solid[cy * s->gw + cx]) return 0.0f;             /* muzzle in a wall */
    int stepx = dx >= 0.0f ? 1 : -1, stepy = dy >= 0.0f ? 1 : -1;
    float tDX = dx != 0.0f ? s->cell / fabsf(dx) : 1e30f;
    float tDY = dy != 0.0f ? s->cell / fabsf(dy) : 1e30f;
    float bx = (dx >= 0.0f ? (cx + 1) : cx) * s->cell;
    float by = (dy >= 0.0f ? (cy + 1) : cy) * s->cell;
    float tMX = dx != 0.0f ? (bx - ox) / dx : 1e30f;
    float tMY = dy != 0.0f ? (by - oy) / dy : 1e30f;
    for (;;) {
        float t;
        if (tMX < tMY) { cx += stepx; t = tMX; tMX += tDX; }
        else           { cy += stepy; t = tMY; tMY += tDY; }
        if (t > maxd) return maxd;
        if (cx < 0 || cx >= s->gw || cy < 0 || cy >= s->gh) return maxd;
        if (s->solid[cy * s->gw + cx]) return t;
    }
}

/* Segment-vs-disc: t of the ray's entry into agent i's disc, clamped to [0,eff];
 * -1 if the ray (0..eff) misses the disc or it lies entirely behind the origin. */
static inline float ray_disc_t(const SimP *s, int i, float ox, float oy,
                               float dx, float dy, float eff) {
    float ocx = s->px[i] - ox, ocy = s->py[i] - oy;
    float tca = ocx * dx + ocy * dy;
    float r = s->rad[i];
    float d2 = ocx * ocx + ocy * ocy - tca * tca;       /* perp distance^2 */
    float r2 = r * r;
    if (d2 > r2) return -1.0f;
    float thc = sqrtf(r2 - d2);
    if (tca + thc < 0.0f) return -1.0f;                 /* disc fully behind */
    float t0 = tca - thc;
    float thit = t0 > 0.0f ? t0 : 0.0f;                 /* origin inside -> 0 */
    return thit <= eff ? thit : -1.0f;
}

/* Insert (i,thit) into the nearest-max_out set kept in out[]/out_t[] (*n live).
 * Dedups by index; when full, replaces the farthest kept hit if thit is closer.
 * (Re-presented evicted agents are rejected: their t is >= the current worst.) */
static inline void ray_consider(int i, float thit, int *out, float *out_t,
                                int max_out, int *n) {
    for (int q = 0; q < *n; q++) if (out[q] == i) return;   /* already kept */
    if (*n < max_out) { out[*n] = i; out_t[*n] = thit; (*n)++; return; }
    int w = 0;
    for (int q = 1; q < max_out; q++) if (out_t[q] > out_t[w]) w = q;
    if (thit < out_t[w]) { out[w] = i; out_t[w] = thit; }
}

int simp_query_ray(const SimP *s, float ox, float oy, float dx, float dy,
                   float maxdist, int *out, float *out_t, int max_out,
                   uint32_t flags) {
    if (max_out <= 0) return 0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-8f) return 0;
    dx /= len; dy /= len;

    float eff = maxdist;
    if (!(flags & SIMP_RAY_NOWALL)) {
        float wt = wall_ray_t(s, ox, oy, dx, dy, maxdist);
        if (wt < eff) eff = wt;
    }
    if (eff <= 0.0f) return 0;

    int n = 0;
    if (grid_current(s) && !(flags & SIMP_QUERY_FLYING)) {
        /* Amanatides-Woo over the collision grid; each stepped cell is tested
         * with its 3x3 halo, because an agent is binned by its CENTER cell and
         * a disc (radius <= grid_rmax <= ccell/2) can cross the ray from a
         * neighbouring cell. ray_consider dedups the halo overlap. We process
         * every cell with entry distance <= eff: a hit at t* <= eff has its
         * centre within one cell of the cell the ray occupies at t* (entry
         * <= t* <= eff), so its halo covers it. */
        int cx = clampi((int)(ox * s->inv_ccell), 0, s->cgw - 1);
        int cy = clampi((int)(oy * s->inv_ccell), 0, s->cgh - 1);
        int stepx = dx >= 0.0f ? 1 : -1, stepy = dy >= 0.0f ? 1 : -1;
        float tDX = dx != 0.0f ? s->ccell / fabsf(dx) : 1e30f;
        float tDY = dy != 0.0f ? s->ccell / fabsf(dy) : 1e30f;
        float bx = (dx >= 0.0f ? (cx + 1) : cx) * s->ccell;
        float by = (dy >= 0.0f ? (cy + 1) : cy) * s->ccell;
        float tMX = dx != 0.0f ? (bx - ox) / dx : 1e30f;
        float tMY = dy != 0.0f ? (by - oy) / dy : 1e30f;
        float tcell = 0.0f;
        for (;;) {
            if (tcell > eff) break;
            int hx0 = cx > 0 ? cx - 1 : 0, hx1 = cx < s->cgw - 1 ? cx + 1 : s->cgw - 1;
            int hy0 = cy > 0 ? cy - 1 : 0, hy1 = cy < s->cgh - 1 ? cy + 1 : s->cgh - 1;
            for (int hy = hy0; hy <= hy1; hy++) for (int hx = hx0; hx <= hx1; hx++) {
                int c = hy * s->cgw + hx;
                for (int k = s->cstart[c]; k < s->cstart[c + 1]; k++) {
                    int i = s->corder[k];
                    if (i >= s->count) continue;            /* corpse ghost */
                    float t = ray_disc_t(s, i, ox, oy, dx, dy, eff);
                    if (t >= 0.0f) ray_consider(i, t, out, out_t, max_out, &n);
                }
            }
            if (tMX < tMY) { cx += stepx; tcell = tMX; tMX += tDX; }
            else           { cy += stepy; tcell = tMY; tMY += tDY; }
            if (cx < 0 || cx >= s->cgw || cy < 0 || cy >= s->cgh) break;
        }
    } else {
        for (int i = 0; i < s->count; i++) {
            if (!(flags & SIMP_QUERY_FLYING) && (s->aflags[i] & SIMP_FLYING))
                continue;
            float t = ray_disc_t(s, i, ox, oy, dx, dy, eff);
            if (t >= 0.0f) ray_consider(i, t, out, out_t, max_out, &n);
        }
    }

    /* order the kept hits by t ascending (insertion sort; n <= max_out small) */
    for (int a = 1; a < n; a++) {
        int bi = out[a]; float bt = out_t[a];
        int q = a - 1;
        while (q >= 0 && out_t[q] > bt) { out[q+1] = out[q]; out_t[q+1] = out_t[q]; q--; }
        out[q+1] = bi; out_t[q+1] = bt;
    }
    return n;
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
    /* §7-bis: accumulate the corpse VOLUME into its nav cell (height of the
     * pile). Independent of the TTL pool: this rots on its own clock. */
    {
        int cx = clampi((int)(x * s->inv_cell), 0, s->gw - 1);
        int cy = clampi((int)(y * s->inv_cell), 0, s->gh - 1);
        s->corpse_mass[cy * s->gw + cx] += 3.14159265f * radius * radius;
        s->corpse_active = true;
    }
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

void simp_corpse_splat(SimP *s, float x, float y, float radius, float mass_scale) {
    if (mass_scale <= 0.0f || radius <= 0.0f) return;
    /* §7-bis nav cost only: bump the cell's corpse VOLUME, no disc, no TTL. */
    int cx = clampi((int)(x * s->inv_cell), 0, s->gw - 1);
    int cy = clampi((int)(y * s->inv_cell), 0, s->gh - 1);
    s->corpse_mass[cy * s->gw + cx] += mass_scale * 3.14159265f * radius * radius;
    s->corpse_active = true;
}

void simp_add_danger(SimP *s, float x, float y, float radius, float w) {
    if (w == 0.0f || radius < 0.0f) return;
    int hcx = clampi((int)(x * s->inv_cell), 0, s->gw - 1);
    int hcy = clampi((int)(y * s->inv_cell), 0, s->gh - 1);
    int cx0 = clampi((int)((x - radius) * s->inv_cell), 0, s->gw - 1);
    int cx1 = clampi((int)((x + radius) * s->inv_cell), 0, s->gw - 1);
    int cy0 = clampi((int)((y - radius) * s->inv_cell), 0, s->gh - 1);
    int cy1 = clampi((int)((y + radius) * s->inv_cell), 0, s->gh - 1);
    float r2 = radius * radius;
    for (int cy = cy0; cy <= cy1; cy++) {
        float dy = (cy + 0.5f) * s->cell - y;
        for (int cx = cx0; cx <= cx1; cx++) {
            float dx = (cx + 0.5f) * s->cell - x;
            /* disc footprint, but the home cell always gets the deposit (so a
             * radius below one cell still marks something) */
            if (dx * dx + dy * dy > r2 && !(cx == hcx && cy == hcy)) continue;
            s->danger[cy * s->gw + cx] += w;
        }
    }
    s->danger_active = true;
    s->cost_dirty = true;     /* fresh blood reroutes at the next throttle tick */
}

const float *simp_danger_arr(const SimP *s) { return s->danger; }

int simp_corpse_count(const SimP *s) { return s->corpse_count; }
const float *simp_corpse_px(const SimP *s)  { return s->cpx; }
const float *simp_corpse_py(const SimP *s)  { return s->cpy; }
const float *simp_corpse_rad(const SimP *s) { return s->crad; }
const float *simp_corpse_height(const SimP *s) { return s->corpse_height; }

void simp_corpse_clear(SimP *s, float x, float y, float radius) {
    float r2 = radius * radius;
    /* remove physical corpses whose center is in range (swap-and-pop, like the
     * TTL decay path; no handles point here) */
    for (int j = s->corpse_count - 1; j >= 0; j--) {
        float dx = s->cpx[j] - x, dy = s->cpy[j] - y;
        if (dx * dx + dy * dy <= r2) {
            int last = --s->corpse_count;
            s->cpx[j] = s->cpx[last]; s->cpy[j] = s->cpy[last];
            s->crad[j] = s->crad[last]; s->cttl[j] = s->cttl[last];
        }
    }
    if (s->corpse_count == 0) s->corpse_rmax = 0.0f;
    s->grid_stale = true;        /* ghosts changed: rebind next step */
    /* zero the pile fields on the cells overlapping the disc */
    int cx0 = clampi((int)((x - radius) * s->inv_cell), 0, s->gw - 1);
    int cx1 = clampi((int)((x + radius) * s->inv_cell), 0, s->gw - 1);
    int cy0 = clampi((int)((y - radius) * s->inv_cell), 0, s->gh - 1);
    int cy1 = clampi((int)((y + radius) * s->inv_cell), 0, s->gh - 1);
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++) {
            float ccx = (cx + 0.5f) * s->cell, ccy = (cy + 0.5f) * s->cell;
            float dx = ccx - x, dy = ccy - y;
            if (dx * dx + dy * dy > r2) continue;
            int c = cy * s->gw + cx;
            s->corpse_mass[c] = s->corpse_pack[c] = s->corpse_height[c] = 0.0f;
        }
    s->cost_dirty = true;        /* force a flow recompute (the pile is gone) */
}

/* ------------------------------------------------------------- draggables */

int simp_drag_add(SimP *s, float x, float y, float radius, float mass) {
    if (s->drag_count >= DRAG_CAP) return -1;
    int i = s->drag_count++;
    s->dpx[i] = x; s->dpy[i] = y;
    s->dvx[i] = 0.0f; s->dvy[i] = 0.0f;
    s->drad[i] = radius;
    /* mass in walker units, like simp_spawn_desc: invm = 1/(mass*r0^2) */
    float r0 = s->params.radius;
    float m = mass > 1e-3f ? mass : 1e-3f;
    s->dinvm[i] = 1.0f / (m * r0 * r0);
    if (radius > s->drag_rmax) s->drag_rmax = radius;
    /* a draggable larger than the grid was sized for would slip through the
     * 5-cell PBD pair search: coarsen the grid (same as simp_spawn_desc). */
    if (radius > s->grid_rmax) {
        s->grid_rmax = radius;
        s->ccell = 2.0f * s->grid_rmax * 1.05f;
        s->inv_ccell = 1.0f / s->ccell;
        s->cgw = (int)ceilf(s->world_w * s->inv_ccell) + 1;
        s->cgh = (int)ceilf(s->world_h * s->inv_ccell) + 1;
        build_wall_near(s);
    }
    s->grid_stale = true;        /* binned as a ghost at the next rebuild */
    return i;
}

void simp_drag_remove(SimP *s, int i) {
    if (i < 0 || i >= s->drag_count) return;
    int last = --s->drag_count;
    s->dpx[i] = s->dpx[last]; s->dpy[i] = s->dpy[last];
    s->dvx[i] = s->dvx[last]; s->dvy[i] = s->dvy[last];
    s->drad[i] = s->drad[last]; s->dinvm[i] = s->dinvm[last];
    if (s->drag_count == 0) s->drag_rmax = 0.0f;
    s->grid_stale = true;
}

void simp_drag_clear(SimP *s) {
    s->drag_count = 0;
    s->drag_rmax = 0.0f;
    s->grid_stale = true;
}

int simp_drag_count(const SimP *s) { return s->drag_count; }
const float *simp_drag_px(const SimP *s)  { return s->dpx; }
const float *simp_drag_py(const SimP *s)  { return s->dpy; }
const float *simp_drag_vx(const SimP *s)  { return s->dvx; }
const float *simp_drag_vy(const SimP *s)  { return s->dvy; }
const float *simp_drag_rad(const SimP *s) { return s->drad; }

/* --------------------------------------------------------------- stepping */

/* Permute the count-byte-per-element array `arr` by perm (newpos -> oldindex)
 * via the shared scratch, then copy back. Only the first `n` slots move. */
static void permute_arr(void *arr, size_t esz, const int *perm, int n, void *tmp) {
    char *a = (char *)arr, *t = (char *)tmp;
    for (int i = 0; i < n; i++) memcpy(t + (size_t)i * esz, a + (size_t)perm[i] * esz, esz);
    memcpy(a, t, (size_t)n * esz);
}

/* M4: reorder the live agents into collision-cell order for cache locality.
 * A plain serial counting sort over all `count` agents (flyers included, by
 * their xy cell) builds perm; every per-agent SoA array is permuted to match and
 * the handle map is rebuilt. The grid is left invalid on purpose: the caller
 * rebuilds it right after. Deterministic (serial), independent of thread count. */
static void reorder_agents(SimP *s) {
    const int n = s->count;
    if (n < 2) return;
    const int nc = s->cgw * s->cgh;
    int *cnt = s->ccount;          /* reused; rebuild_grid refills it next */
    memset(cnt, 0, (size_t)(nc + 1) * sizeof(int));
    for (int i = 0; i < n; i++) {
        int cx = clampi((int)(s->px[i] * s->inv_ccell), 0, s->cgw - 1);
        int cy = clampi((int)(s->py[i] * s->inv_ccell), 0, s->cgh - 1);
        cnt[cy * s->cgw + cx]++;
    }
    int acc = 0;
    for (int c = 0; c < nc; c++) { s->cstart[c] = acc; acc += cnt[c]; }
    memcpy(cnt, s->cstart, (size_t)nc * sizeof(int));   /* cursor */
    int *perm = s->reorder_perm;
    for (int i = 0; i < n; i++) {                        /* stable by i */
        int cx = clampi((int)(s->px[i] * s->inv_ccell), 0, s->cgw - 1);
        int cy = clampi((int)(s->py[i] * s->inv_ccell), 0, s->cgh - 1);
        perm[cnt[cy * s->cgw + cx]++] = i;
    }
    void *tmp = s->reorder_tmp;
    permute_arr(s->px, sizeof(float), perm, n, tmp);
    permute_arr(s->py, sizeof(float), perm, n, tmp);
    permute_arr(s->qx, sizeof(float), perm, n, tmp);
    permute_arr(s->qy, sizeof(float), perm, n, tmp);
    permute_arr(s->vx, sizeof(float), perm, n, tmp);
    permute_arr(s->vy, sizeof(float), perm, n, tmp);
    permute_arr(s->rad, sizeof(float), perm, n, tmp);
    permute_arr(s->vpref, sizeof(float), perm, n, tmp);
    permute_arr(s->invm, sizeof(float), perm, n, tmp);
    permute_arr(s->z, sizeof(float), perm, n, tmp);
    permute_arr(s->vz, sizeof(float), perm, n, tmp);
    permute_arr(s->seed, sizeof(uint32_t), perm, n, tmp);
    permute_arr(s->aflags, sizeof(uint8_t), perm, n, tmp);
    permute_arr(s->stun, sizeof(float), perm, n, tmp);
    permute_arr(s->wall_pressure, sizeof(float), perm, n, tmp);
    permute_arr(s->wall_cell, sizeof(int), perm, n, tmp);
    /* handle map: permute index->slot, then rebuild slot->index from it
     * (slot_gen / slot_free are slot-indexed, untouched) */
    permute_arr(s->index_to_slot, sizeof(int), perm, n, tmp);
    for (int i = 0; i < n; i++) s->slot_to_index[s->index_to_slot[i]] = i;
    s->grid_stale = true;
}

static void rebuild_grid(SimP *s) {
    const int nc = s->cgw * s->cgh;
    /* corpse ghosts: copy the pool past the live agents as discs the PBD treats
     * like any other body. §3: a large-but-FINITE mass (corpse_weight walker
     * units, invm = 1/(weight*r0^2) as in simp_spawn_desc) so a pushing crowd
     * shoves the pile; corpse_weight <= 0 = the legacy infinite seal (invm 0). */
    const int ng = s->corpse_count;
    const float cw = s->params.corpse_weight;
    const float r0 = s->params.radius;
    const float cinvm = (cw > 0.0f) ? 1.0f / (cw * r0 * r0) : 0.0f;
    for (int j = 0; j < ng; j++) {
        int g = s->count + j;
        s->px[g] = s->cpx[j]; s->py[g] = s->cpy[j];
        s->rad[g] = s->crad[j];
        s->invm[g] = cinvm;
        s->seed[g] = 0xC0FF1234u + (uint32_t)j;   /* determinism (d==0 path) */
    }
    /* draggable ghosts (DRAG_DESIGN.md): appended past the corpses, each with its
     * own finite invm. grid_drag0 marks the boundary so the PBD skips only
     * corpse-corpse pairs (draggable-draggable / draggable-corpse collide). */
    const int nd = s->drag_count;
    s->grid_drag0 = s->count + ng;
    for (int j = 0; j < nd; j++) {
        int g = s->grid_drag0 + j;
        s->px[g] = s->dpx[j]; s->py[g] = s->dpy[j];
        s->rad[g] = s->drad[j];
        s->invm[g] = s->dinvm[j];
        s->seed[g] = 0xDA661234u + (uint32_t)j;   /* determinism (d==0 path) */
    }
    const int total = s->count + ng + nd;
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
    s->grid_ghosts = ng + nd;
    s->grid_stale = false;
}

/* Per-step constants shared with the parallel jobs (arg = &StepCtx). */
typedef struct {
    SimP *s;
    float dt, amax_dt, damp, inv_dt, vc, wall_h;
    int   wall_record;          /* wall job: sample the siege sensor */
    /* PBD colored-tile dispatch (set per color before each parallel_for) */
    int   tile, color_ncx, color_ox, color_oy;
} StepCtx;

#define PBD_TILE 4              /* collision-cell tile side; >=2 keeps same-color
                                  tiles' write footprints disjoint (lock-free) */

/* Gauss-Seidel resolution for one collision cell: agent vs own-cell tail +
 * 4 forward neighbours (E, SW, S, SE), every pair once. Writes px/py of the
 * home agent and of the neighbour agents it pushes. Diagnostics into locals. */
static inline void pbd_cell(SimP *s, int cx, int cy, double *dsum, long *dn) {
    const int cgw = s->cgw, cgh = s->cgh;
    static const int NX[5] = { 0, 1, -1, 0, 1 };
    static const int NY[5] = { 0, 0,  1, 1, 1 };
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
                /* corpse-corpse (both in [count, grid_drag0)): skip, or a pile
                 * built from overlapping discs would blow itself apart. Also
                 * covers both-immovable (invm 0) under the legacy infinite mass.
                 * Draggable ghosts (>= grid_drag0) are NOT skipped: draggable-
                 * draggable / draggable-corpse collide so an accosted row holds
                 * (DRAG_DESIGN.md). With no draggables grid_drag0 = count+ng, so
                 * this is bit-identical to the old "both ghosts" test. */
                int ic = (i >= s->count && i < s->grid_drag0);
                int jc = (j >= s->count && j < s->grid_drag0);
                if (ic && jc) continue;
                if (wi + wj <= 0.0f) continue;  /* both immovable (infinite) */
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
                /* robustness: never resolve more than 30% of the combined
                 * radius per pair per iteration; deep overlaps relax over a
                 * few steps instead of exploding into recovered velocities */
                float capd = 0.30f * rsum;
                if (overlap > capd) overlap = capd;
                float inv = 1.0f / (wi + wj);
                float ci = overlap * wi * inv;
                float cj = overlap * wj * inv;
                pix -= nxv * ci; piy -= nyv * ci;
                s->px[j] += nxv * cj; s->py[j] += nyv * cj;
                *dsum += overlap; (*dn)++;
            }
        }
        s->px[i] = pix; s->py[i] = piy;
    }
}

/* One color pass: process the same-color tiles in [begin,end) (their write
 * footprints are disjoint, so this is lock-free). idx -> (tile_x,tile_y). */
static void job_pbd_color(void *arg, int worker, int begin, int end) {
    StepCtx *ctx = arg; SimP *s = ctx->s;
    const int T = ctx->tile, ncx = ctx->color_ncx, ox = ctx->color_ox, oy = ctx->color_oy;
    double dsum = 0; long dn = 0;
    for (int idx = begin; idx < end; idx++) {
        int txi = ox + 2 * (idx % ncx);
        int tyi = oy + 2 * (idx / ncx);
        int hx0 = txi * T, hy0 = tyi * T;
        int hx1 = hx0 + T; if (hx1 > s->cgw) hx1 = s->cgw;
        int hy1 = hy0 + T; if (hy1 > s->cgh) hy1 = s->cgh;
        for (int cy = hy0; cy < hy1; cy++)
            for (int cx = hx0; cx < hx1; cx++)
                pbd_cell(s, cx, cy, &dsum, &dn);
    }
    s->diag_sum_w[worker] += dsum; s->diag_n_w[worker] += dn;
}

/* One PBD sweep: 4 color passes (2x2 tile checkerboard), barrier between them
 * (parallel_for completes = barrier). Same result for any thread count. */
static void pbd_iteration(SimP *s, StepCtx *ctx) {
    const int T = PBD_TILE;
    int tiles_x = (s->cgw + T - 1) / T;
    int tiles_y = (s->cgh + T - 1) / T;
    ctx->tile = T;
    for (int color = 0; color < 4; color++) {
        int ox = color & 1, oy = (color >> 1) & 1;
        if (ox >= tiles_x || oy >= tiles_y) continue;
        int ncx = (tiles_x - ox + 1) / 2;
        int ncy = (tiles_y - oy + 1) / 2;
        if (ncx <= 0 || ncy <= 0) continue;
        ctx->color_ncx = ncx; ctx->color_ox = ox; ctx->color_oy = oy;
        simpool_parallel_for(s->pool, ncx * ncy, 1, job_pbd_color, ctx);
    }
}

/* When record is set (the final iteration of the step) the siege sensor is
 * sampled from the PRE-correction penetration: an agent pressing into a wall
 * to reach its goal has push > 0 and steers into the wall (into_wall > 0);
 * one grazing it tangentially has into_wall <= 0 and is left at 0. Measuring
 * here, before the projection cancels the overlap, is the only place push is
 * still meaningful (after the push d >= r). See SIEGE_DESIGN.md. */
static void job_wall(void *arg, int worker, int begin, int end) {
    (void)worker;
    StepCtx *ctx = arg; SimP *s = ctx->s;
    const float wall_h = ctx->wall_h;
    const int record = ctx->wall_record;
    for (int i = begin; i < end; i++) {
        float r = s->rad[i];
        /* above wall_h walls are overflown; low flyers slam into them */
        int wcx = clampi((int)(s->px[i] * s->inv_cell), 0, s->gw - 1);
        int wcy = clampi((int)(s->py[i] * s->inv_cell), 0, s->gh - 1);
        /* M4: skip the bilinear SDF sample where no wall is within reach */
        if (!((s->aflags[i] & SIMP_FLYING) && s->z[i] > wall_h) &&
            s->wall_near[wcy * s->gw + wcx]) {
            float d = simp_sample_sdf(s, s->px[i], s->py[i]);
            if (d < r) {
                float gx, gy;
                sdf_grad(s, s->px[i], s->py[i], &gx, &gy);
                float push = r - d;
                /* siege sensor (skip flyers: ballistic, not besieging) */
                if (record && !(s->aflags[i] & SIMP_FLYING)) {
                    float fx, fy;
                    simp_sample_flow(s, s->px[i], s->py[i], &fx, &fy);
                    float into_wall = -(fx * gx + fy * gy);  /* >0 = into wall */
                    if (into_wall > 0.0f) {
                        /* besieged cell = the SOLID cell near the agent best
                         * aligned with -gradient (toward the wall). Scanning a
                         * small window (the wall is within r of the center, so
                         * within ~1 cell) keeps the cell exactly solid, where a
                         * single blind step can miss at corners / thin walls. */
                        int acx = clampi((int)(s->px[i] * s->inv_cell), 0, s->gw - 1);
                        int acy = clampi((int)(s->py[i] * s->inv_cell), 0, s->gh - 1);
                        int w = (int)ceilf((r + 0.5f * s->cell) * s->inv_cell);
                        float best = 0.0f; int bestc = -1;
                        for (int dy = -w; dy <= w; dy++) for (int dx = -w; dx <= w; dx++) {
                            int nx = acx + dx, ny = acy + dy;
                            if (nx < 0 || ny < 0 || nx >= s->gw || ny >= s->gh) continue;
                            int c = ny * s->gw + nx;
                            if (!s->solid[c]) continue;
                            float ccx = ((float)nx + 0.5f) * s->cell - s->px[i];
                            float ccy = ((float)ny + 0.5f) * s->cell - s->py[i];
                            float dot = ccx * (-gx) + ccy * (-gy);   /* toward wall */
                            if (dot > best) { best = dot; bestc = c; }
                        }
                        if (bestc >= 0) {
                            s->wall_pressure[i] = push * into_wall;
                            s->wall_cell[i] = bestc;
                        }
                    }
                }
                s->px[i] += gx * push;
                s->py[i] += gy * push;
            }
        }
        /* hard world bounds (flyers included: they slide along the edge) */
        s->px[i] = clampf(s->px[i], r, s->world_w - r);
        s->py[i] = clampf(s->py[i], r, s->world_h - r);
    }
}

/* steering toward the flow field + noise, bounded acceleration. Per-agent,
 * own RNG seed: embarrassingly parallel. Dormant agents brake to rest, flyers
 * are ballistic (skipped). */
static void job_steering(void *arg, int worker, int begin, int end) {
    (void)worker;
    StepCtx *ctx = arg; SimP *s = ctx->s; const SimPParams *P = &s->params;
    const float amax_dt = ctx->amax_dt, damp = ctx->damp, dt = ctx->dt;
    for (int i = begin; i < end; i++) {
        uint8_t fl = s->aflags[i];
        if (fl & SIMP_FLYING) continue;
        /* hit stun: decay the timer and brake to rest (no goal pull) while it
         * runs — keeps the agent planted under its flinch animation instead of
         * sliding forward. Still collides and absorbs crowd pushes (PBD). */
        bool stunned = false;
        if (s->stun[i] > 0.0f) { s->stun[i] -= dt; stunned = true; }
        float fx = 0.0f, fy = 0.0f;
        if (!(fl & SIMP_DORMANT) && !stunned) {
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
}

/* recover effective velocity from positional change (the crowd push becomes
 * real momentum). Per-agent: embarrassingly parallel. Flyers keep theirs. */
static void job_recover(void *arg, int worker, int begin, int end) {
    (void)worker;
    StepCtx *ctx = arg; SimP *s = ctx->s;
    const float inv_dt = ctx->inv_dt, vc = ctx->vc;
    for (int i = begin; i < end; i++) {
        if (s->aflags[i] & SIMP_FLYING) continue;
        float nvx = (s->px[i] - s->qx[i]) * inv_dt;
        float nvy = (s->py[i] - s->qy[i]) * inv_dt;
        float sp2 = nvx * nvx + nvy * nvy;
        if (sp2 > vc * vc) { float k = vc / sqrtf(sp2); nvx *= k; nvy *= k; }
        s->vx[i] = nvx; s->vy[i] = nvy;
    }
}

int simp_step(SimP *s, float dt) {
    const SimPParams *P = &s->params;
    /* navigation: terrain edits force a full commit (phi+flow+SDF, reads the
     * current rho_s/cost_user); otherwise phi+flow alone are refreshed on a
     * throttle whenever density matters or user costs changed (M3.6).
     * M4: the throttled phi is drained incrementally over several steps (a fixed
     * budget per step) so it never spikes a single frame; flow is committed only
     * when the heap empties, agents read the previous flow until then. */
    if (s->nav_dirty) {
        s->nav_phase = 0;                /* drop any in-progress incremental build */
        nav_commit(s);
        s->flow_timer = 0.0f;
        s->cost_dirty = false;
    } else if (s->nav_phase == 1) {      /* a phi build is in progress: keep draining */
        nav_phi_drain(s, s->nav_budget);
        if (s->nav_heap_n == 0) {        /* heap empty: build done, commit the flow */
            recompute_flow(s);
            s->nav_phase = 0;
        }
    } else if (P->flow_period > 0.0f) {
        s->flow_timer += dt;
        if (s->flow_timer >= P->flow_period) {
            s->flow_timer = 0.0f;
            bool density_on = (P->k_density > 0.0f || P->k_jam > 0.0f) &&
                (s->count > 0 || s->corpse_count > 0 || s->rho_active);
            bool corpse_on = P->k_corpse > 0.0f && s->corpse_active;
            bool danger_on = P->k_danger > 0.0f && s->danger_active;
            if (density_on || corpse_on || danger_on || s->cost_dirty) {
                density_update(s);
                nav_phi_begin(s);          /* seed the heap (frozen inputs) */
                nav_phi_drain(s, s->nav_budget);
                s->cost_dirty = false;
                if (s->nav_heap_n == 0) recompute_flow(s);  /* drained in one go */
                else                    s->nav_phase = 1;    /* finish over next steps */
            }
        }
    }
    const float amax_dt = P->a_max * dt;
    const float damp = clampf(1.0f - P->damping * dt, 0.0f, 1.0f);

    StepCtx ctx = { .s = s, .dt = dt, .amax_dt = amax_dt, .damp = damp,
                    .inv_dt = 1.0f / dt, .vc = P->v_clamp, .wall_h = P->wall_h,
                    .wall_record = 0, .tile = 0, .color_ncx = 0, .color_ox = 0, .color_oy = 0 };

    s->diag_overlap_sum = 0.0f;
    s->diag_overlap_n = 0;

    s->landed_n = 0;

    /* 1) steering toward flow field + noise, bounded acceleration (parallel:
     * per-agent, own RNG seed). Dormant agents brake to rest; flyers ballistic. */
    simpool_parallel_for(s->pool, s->count, 2048, job_steering, &ctx);

    /* 2) integrate (save pre-projection position); fly the fake z axis.
     * Serial: cheap, and touchdown appends to landed[] (shared cursor). */
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

    /* 2b) integrate draggables in the pool (DRAG_DESIGN.md): they carry their own
     * momentum, so advance the pool positions before rebuild_grid copies them
     * into ghost slots. dq = pre-integration position, for velocity recovery from
     * the PBD shove (3b'). No gravity (planar). */
    for (int j = 0; j < s->drag_count; j++) {
        s->dqx[j] = s->dpx[j]; s->dqy[j] = s->dpy[j];
        s->dpx[j] += s->dvx[j] * dt;
        s->dpy[j] += s->dvy[j] * dt;
    }

    /* 3) constraints: agent-agent (PBD, parallel colored tiles) + walls
     * (parallel per-agent), iterated. The siege sensor (SIEGE_DESIGN.md) is
     * reset, then filled on the FINAL wall projection. */
    for (int i = 0; i < s->count; i++) {
        s->wall_pressure[i] = 0.0f;
        s->wall_cell[i] = -1;
    }
    rebuild_grid(s);
    for (int w = 0; w < s->nthreads; w++) { s->diag_sum_w[w] = 0.0; s->diag_n_w[w] = 0; }
    for (int it = 0; it < P->pbd_iters; it++) {
        pbd_iteration(s, &ctx);
        ctx.wall_record = (it == P->pbd_iters - 1);
        simpool_parallel_for(s->pool, s->count, 2048, job_wall, &ctx);
    }
    /* reduce per-worker overlap diagnostics in fixed order (deterministic) */
    for (int w = 0; w < s->nthreads; w++) {
        s->diag_overlap_sum += (float)s->diag_sum_w[w];
        s->diag_overlap_n   += (int)s->diag_n_w[w];
    }

    /* 3b) §3: persist the shoved corpse positions back to the pool. Finite-mass
     * ghosts were moved by the crowd during the PBD; the next rebuild reloads
     * ghost positions from cpx/cpy, so write them now — BEFORE the drain changes
     * `count` (ghosts live at index count+j). No-op under infinite mass. */
    if (s->params.corpse_weight > 0.0f)
        for (int j = 0; j < s->corpse_count; j++) {
            s->cpx[j] = s->px[s->count + j];
            s->cpy[j] = s->py[s->count + j];
        }

    /* 3b') draggables (DRAG_DESIGN.md): push the ghosts out of walls (they MOVE,
     * unlike corpses — serial, they are few), then recover velocity from the PBD
     * shove (v = (pos - dq)/dt, clamped), brake it by friction, and persist
     * position + velocity to the pool. Ghosts live at grid_drag0+j; do it before
     * the drain changes `count`. */
    if (s->drag_count > 0) {
        const float inv_dt = 1.0f / dt, vc = P->v_clamp;
        const float dragf = clampf(1.0f - P->drag_damp * dt, 0.0f, 1.0f);
        for (int j = 0; j < s->drag_count; j++) {
            int g = s->grid_drag0 + j;
            float r = s->drad[j];
            /* wall projection (same SDF push as job_wall) */
            float d = simp_sample_sdf(s, s->px[g], s->py[g]);
            if (d < r) {
                float gx, gy;
                sdf_grad(s, s->px[g], s->py[g], &gx, &gy);
                float push = r - d;
                s->px[g] += gx * push; s->py[g] += gy * push;
            }
            s->px[g] = clampf(s->px[g], r, s->world_w - r);
            s->py[g] = clampf(s->py[g], r, s->world_h - r);
            /* recover + friction */
            float nvx = (s->px[g] - s->dqx[j]) * inv_dt;
            float nvy = (s->py[g] - s->dqy[j]) * inv_dt;
            float sp2 = nvx * nvx + nvy * nvy;
            if (sp2 > vc * vc) { float k = vc / sqrtf(sp2); nvx *= k; nvy *= k; }
            s->dvx[j] = nvx * dragf; s->dvy[j] = nvy * dragf;
            s->dpx[j] = s->px[g]; s->dpy[j] = s->py[g];
        }
    }

    /* 4) recover effective velocity from positional change (parallel). Flyers
     * keep their integrated velocity (no projections; clamping would cut hard
     * launches short — job_recover skips them). */
    simpool_parallel_for(s->pool, s->count, 2048, job_recover, &ctx);

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

    /* 5b') corpse-pile decay + derived height (§7-bis). Pointwise over the nav
     * grid (cheap vs PBD; gated so it idles when no pile exists). mass rots
     * slowly, pack faster (untrampled piles re-inflate); height = k_h * mass /
     * (1 + k_pack * pack) drops as the pile is packed flat. */
    if (s->corpse_active) {
        const int n = s->gw * s->gh;
        const float lm = expf(-0.69314718f * dt / s->params.corpse_mass_hl);
        const float lp = expf(-0.69314718f * dt / s->params.corpse_pack_hl);
        const float kh = s->params.k_h, kpk = s->params.k_pack;
        float maxm = 0.0f;
        for (int i = 0; i < n; i++) {
            float mass = s->corpse_mass[i] * lm;
            float pack = s->corpse_pack[i] * lp;
            if (mass < 1e-4f) { mass = 0.0f; pack = 0.0f; }   /* settle to clean 0 */
            s->corpse_mass[i] = mass;
            s->corpse_pack[i] = pack;
            s->corpse_height[i] = kh * mass / (1.0f + kpk * pack);
            if (mass > maxm) maxm = mass;
        }
        if (maxm <= 0.0f) s->corpse_active = false;
    }

    /* 5b'') blood-fear decay (2026-06-25): single scalar field, exponential
     * decay with danger_hl (matched to the blood decal lifetime). Pointwise,
     * gated like the corpse pile so it idles when no blood is fresh. The
     * throttled flow recompute (danger_on) reroutes the horde back as it fades. */
    if (s->danger_active) {
        const int n = s->gw * s->gh;
        const float ld = expf(-0.69314718f * dt / s->params.danger_hl);
        float maxd = 0.0f;
        for (int i = 0; i < n; i++) {
            float d = s->danger[i] * ld;
            if (d < 1e-4f) d = 0.0f;            /* settle to clean 0 */
            s->danger[i] = d;
            if (d > maxd) maxd = d;
        }
        if (maxd <= 0.0f) s->danger_active = false;
    }

    /* 5c) periodic cache-locality reorder (M4): permute the SoA into cell order
     * so the rebuild below and the next steps' PBD touch memory sequentially.
     * Done just before the rebuild so it commits the new index layout. */
    if (s->reorder_period > 0 && ++s->reorder_ctr >= s->reorder_period) {
        s->reorder_ctr = 0; reorder_agents(s);
    }

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
const float *simp_wall_pressure(const SimP *s) { return s->wall_pressure; }
const int   *simp_wall_cell(const SimP *s) { return s->wall_cell; }
const uint8_t *simp_flags_arr(const SimP *s) { return s->aflags; }
const float *simp_z_arr(const SimP *s) { return s->z; }
int simp_landed_count(const SimP *s) { return s->landed_n; }
const SimPHandle *simp_landed(const SimP *s) { return s->landed; }

float simp_mean_overlap(const SimP *s) {
    return s->diag_overlap_n ? s->diag_overlap_sum / (float)s->diag_overlap_n : 0.0f;
}

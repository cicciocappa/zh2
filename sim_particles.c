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

    /* agents (SoA) */
    int   count, cap;
    float *px, *py;        /* positions */
    float *qx, *qy;        /* previous positions (pre-projection) */
    float *vx, *vy;
    float *rad;            /* per-agent radius */
    float *vpref;          /* per-agent preferred speed */
    float *invm;           /* inverse mass (~ 1/r^2) */
    uint32_t *seed;        /* per-agent RNG state */

    /* collision grid (uniform, rebuilt each step via counting sort) */
    int   cgw, cgh;
    float ccell, inv_ccell;
    int  *ccount;          /* cgw*cgh + 1 : counts then prefix sums */
    int  *cstart;          /* prefix sums (cgw*cgh + 1) */
    int  *corder;          /* cap : agent indices sorted by cell */

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

static void recompute_phi(SimP *s) {
    const int n = s->gw * s->gh;
    for (int i = 0; i < n; i++) s->phi[i] = PHI_INF;

    Heap h; h.a = (HNode *)malloc(sizeof(HNode) * (size_t)n * 4); h.n = 0;
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
            if (s->solid[j]) continue;
            /* forbid diagonal corner-cutting through walls */
            if (k >= 4 && (s->solid[cy * s->gw + nx] || s->solid[ny * s->gw + cx])) continue;
            float nc = nd.c + DC[k];
            if (nc < s->phi[j]) { s->phi[j] = nc; heap_push(&h, nc, j); }
        }
    }
    free(h.a);
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
                if (s->solid[j]) continue;
                if (dx && dy && (s->solid[cy * gw + nx] || s->solid[ny * gw + cx])) continue;
                if (s->phi[j] < best) { best = s->phi[j]; bx = (float)dx; by = (float)dy; }
            }
            float len = sqrtf(bx * bx + by * by);
            if (len > 0.0f) { s->flow_x[i] = bx / len; s->flow_y[i] = by / len; }
        }
    }
    /* one smoothing pass (skip walls), then renormalize */
    float *tx = (float *)malloc(sizeof(float) * (size_t)gw * gh);
    float *ty = (float *)malloc(sizeof(float) * (size_t)gw * gh);
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
    free(tx); free(ty);
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

    const size_t n = (size_t)gw * gh;
    s->solid  = (uint8_t *)calloc(n, 1);
    s->goal   = (uint8_t *)calloc(n, 1);
    s->phi    = (float *)malloc(n * sizeof(float));
    s->flow_x = (float *)calloc(n, sizeof(float));
    s->flow_y = (float *)calloc(n, sizeof(float));
    s->sdf    = (float *)malloc(n * sizeof(float));

    s->cap = max_agents;
    s->px = (float *)malloc((size_t)max_agents * sizeof(float));
    s->py = (float *)malloc((size_t)max_agents * sizeof(float));
    s->qx = (float *)malloc((size_t)max_agents * sizeof(float));
    s->qy = (float *)malloc((size_t)max_agents * sizeof(float));
    s->vx = (float *)calloc((size_t)max_agents, sizeof(float));
    s->vy = (float *)calloc((size_t)max_agents, sizeof(float));
    s->rad   = (float *)malloc((size_t)max_agents * sizeof(float));
    s->vpref = (float *)malloc((size_t)max_agents * sizeof(float));
    s->invm  = (float *)malloc((size_t)max_agents * sizeof(float));
    s->seed  = (uint32_t *)malloc((size_t)max_agents * sizeof(uint32_t));

    /* collision grid: cell side = 2 * max plausible radius */
    float rmax = s->params.radius * (1.0f + s->params.r_jitter);
    s->ccell = 2.0f * rmax * 1.05f;
    s->inv_ccell = 1.0f / s->ccell;
    s->cgw = (int)ceilf(s->world_w * s->inv_ccell) + 1;
    s->cgh = (int)ceilf(s->world_h * s->inv_ccell) + 1;
    s->ccount = (int *)malloc(((size_t)s->cgw * s->cgh + 1) * sizeof(int));
    s->cstart = (int *)malloc(((size_t)s->cgw * s->cgh + 1) * sizeof(int));
    s->corder = (int *)malloc((size_t)max_agents * sizeof(int));

    s->rng = 0x9E3779B9u;
    s->nav_dirty = true;
    return s;
}

void simp_destroy(SimP *s) {
    if (!s) return;
    free(s->solid); free(s->goal); free(s->phi);
    free(s->flow_x); free(s->flow_y); free(s->sdf);
    free(s->px); free(s->py); free(s->qx); free(s->qy);
    free(s->vx); free(s->vy);
    free(s->rad); free(s->vpref); free(s->invm); free(s->seed);
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

/* ------------------------------------------------------------- agents */

int simp_spawn(SimP *s, float x, float y) {
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
    float rj = 1.0f + s->params.r_jitter * rng_fsym(&s->seed[i]);
    float vj = 1.0f + s->params.v_jitter * rng_fsym(&s->seed[i]);
    s->rad[i]   = s->params.radius * rj;
    s->vpref[i] = s->params.v_max * vj;
    s->invm[i]  = 1.0f / (s->rad[i] * s->rad[i]);   /* mass ~ r^2 */
    return i;
}

void simp_kill(SimP *s, int i) {
    int last = --s->count;
    if (i == last) return;
    s->px[i] = s->px[last]; s->py[i] = s->py[last];
    s->qx[i] = s->qx[last]; s->qy[i] = s->qy[last];
    s->vx[i] = s->vx[last]; s->vy[i] = s->vy[last];
    s->rad[i] = s->rad[last]; s->vpref[i] = s->vpref[last];
    s->invm[i] = s->invm[last]; s->seed[i] = s->seed[last];
}

int simp_count(const SimP *s) { return s->count; }

void simp_apply_impulse(SimP *s, float x, float y, float radius, float strength) {
    /* coarse cull through the collision grid */
    int x0 = clampi((int)((x - radius) * s->inv_ccell), 0, s->cgw - 1);
    int x1 = clampi((int)((x + radius) * s->inv_ccell), 0, s->cgw - 1);
    int y0 = clampi((int)((y - radius) * s->inv_ccell), 0, s->cgh - 1);
    int y1 = clampi((int)((y + radius) * s->inv_ccell), 0, s->cgh - 1);
    const float r2 = radius * radius;
    /* grid may be stale (impulses usually arrive between steps): fall back
     * to brute force if it has not been built for the current count */
    bool grid_ok = (s->cstart[s->cgw * s->cgh] == s->count);
    if (grid_ok) {
        for (int cy = y0; cy <= y1; cy++) for (int cx = x0; cx <= x1; cx++) {
            int c = cy * s->cgw + cx;
            for (int k = s->cstart[c]; k < s->cstart[c + 1]; k++) {
                int i = s->corder[k];
                float dx = s->px[i] - x, dy = s->py[i] - y;
                float d2 = dx * dx + dy * dy;
                if (d2 > r2) continue;
                float d = sqrtf(d2);
                float fall = 1.0f - d / radius;
                float nx, ny;
                if (d > 1e-5f) { nx = dx / d; ny = dy / d; }
                else { nx = rng_fsym(&s->rng); ny = rng_fsym(&s->rng);
                       float l = sqrtf(nx*nx + ny*ny) + 1e-6f; nx /= l; ny /= l; }
                s->vx[i] += strength * fall * nx;
                s->vy[i] += strength * fall * ny;
            }
        }
    } else {
        for (int i = 0; i < s->count; i++) {
            float dx = s->px[i] - x, dy = s->py[i] - y;
            float d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            float d = sqrtf(d2);
            float fall = 1.0f - d / radius;
            if (d > 1e-5f) { s->vx[i] += strength * fall * dx / d;
                             s->vy[i] += strength * fall * dy / d; }
        }
    }
}

/* --------------------------------------------------------------- stepping */

static void rebuild_grid(SimP *s) {
    const int nc = s->cgw * s->cgh;
    memset(s->ccount, 0, (size_t)(nc + 1) * sizeof(int));
    for (int i = 0; i < s->count; i++) {
        int cx = clampi((int)(s->px[i] * s->inv_ccell), 0, s->cgw - 1);
        int cy = clampi((int)(s->py[i] * s->inv_ccell), 0, s->cgh - 1);
        s->ccount[cy * s->cgw + cx]++;
    }
    int acc = 0;
    for (int c = 0; c < nc; c++) { s->cstart[c] = acc; acc += s->ccount[c]; }
    s->cstart[nc] = acc;
    memcpy(s->ccount, s->cstart, (size_t)(nc + 1) * sizeof(int)); /* reuse as cursor */
    for (int i = 0; i < s->count; i++) {
        int cx = clampi((int)(s->px[i] * s->inv_ccell), 0, s->cgw - 1);
        int cy = clampi((int)(s->py[i] * s->inv_ccell), 0, s->cgh - 1);
        s->corder[s->ccount[cy * s->cgw + cx]++] = i;
    }
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
                        float wj = s->invm[j];
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
    for (int i = 0; i < s->count; i++) {
        float d = simp_sample_sdf(s, s->px[i], s->py[i]);
        float r = s->rad[i];
        if (d < r) {
            float gx, gy;
            sdf_grad(s, s->px[i], s->py[i], &gx, &gy);
            float push = r - d;
            s->px[i] += gx * push;
            s->py[i] += gy * push;
        }
        /* hard world bounds */
        s->px[i] = clampf(s->px[i], r, s->world_w - r);
        s->py[i] = clampf(s->py[i], r, s->world_h - r);
    }
}

int simp_step(SimP *s, float dt) {
    if (s->nav_dirty) nav_commit(s);
    const SimPParams *P = &s->params;
    const float amax_dt = P->a_max * dt;
    const float damp = clampf(1.0f - P->damping * dt, 0.0f, 1.0f);

    s->diag_overlap_sum = 0.0f;
    s->diag_overlap_n = 0;

    /* 1) steering toward flow field + noise, bounded acceleration */
    for (int i = 0; i < s->count; i++) {
        float fx, fy;
        simp_sample_flow(s, s->px[i], s->py[i], &fx, &fy);
        if (P->noise_ang > 0.0f && (fx != 0.0f || fy != 0.0f)) {
            float a = P->noise_ang * rng_fsym(&s->seed[i]);
            float ca = cosf(a), sa = sinf(a);
            float rx = fx * ca - fy * sa, ry = fx * sa + fy * ca;
            fx = rx; fy = ry;
        }
        float dvx = fx * s->vpref[i] - s->vx[i];
        float dvy = fy * s->vpref[i] - s->vy[i];
        float dl = sqrtf(dvx * dvx + dvy * dvy);
        if (dl > amax_dt) { float k = amax_dt / dl; dvx *= k; dvy *= k; }
        s->vx[i] = (s->vx[i] + dvx) * damp;
        s->vy[i] = (s->vy[i] + dvy) * damp;
    }

    /* 2) integrate (save pre-projection position) */
    for (int i = 0; i < s->count; i++) {
        s->qx[i] = s->px[i]; s->qy[i] = s->py[i];
        s->px[i] += s->vx[i] * dt;
        s->py[i] += s->vy[i] * dt;
    }

    /* 3) constraints: agent-agent (PBD) + walls, iterated */
    rebuild_grid(s);
    for (int it = 0; it < P->pbd_iters; it++) {
        pbd_iteration(s);
        wall_projection(s);
    }

    /* 4) recover effective velocity from positional change */
    const float inv_dt = 1.0f / dt;
    const float vc = P->v_clamp;
    for (int i = 0; i < s->count; i++) {
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

    /* 5) drain agents standing on goal cells (iterate backward: swap-and-pop) */
    int drained = 0;
    for (int i = s->count - 1; i >= 0; i--) {
        int cx = clampi((int)(s->px[i] * s->inv_cell), 0, s->gw - 1);
        int cy = clampi((int)(s->py[i] * s->inv_cell), 0, s->gh - 1);
        if (s->goal[cy * s->gw + cx]) { simp_kill(s, i); drained++; }
    }
    return drained;
}

/* ------------------------------------------------------------ read access */

const float *simp_px(const SimP *s) { return s->px; }
const float *simp_py(const SimP *s) { return s->py; }
const float *simp_vx(const SimP *s) { return s->vx; }
const float *simp_vy(const SimP *s) { return s->vy; }
const float *simp_radius_arr(const SimP *s) { return s->rad; }

float simp_mean_overlap(const SimP *s) {
    return s->diag_overlap_n ? s->diag_overlap_sum / (float)s->diag_overlap_n : 0.0f;
}

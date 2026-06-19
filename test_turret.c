/* test_turret.c — M5 part 1: hitscan (simp_query_ray) + simp_set_vpref.
 *
 * The turret/wound/economy gameplay lives in game code (M5_DESIGN.md); this
 * test exercises only the two new CORE primitives:
 *
 *   A) simp_query_ray vs an O(N) brute-force oracle (set equality + t order),
 *      over the gridded fast path AND the stale-grid brute-force fallback.
 *   B) piercing vs non-piercing (max_out): all colinear hits in t order, or
 *      just the nearest.
 *   C) wall occlusion (line-of-sight): a wall hides the agent beyond it unless
 *      SIMP_RAY_NOWALL is passed.
 *   D) simp_set_vpref: a crawl-speed agent advances far less than a normal one.
 */
#include "sim_particles.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define N 8000
#define DT (1.0f / 60.0f)

static uint32_t rs = 1234567u;
static float frnd(void) {
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return (float)(rs >> 8) / 16777216.0f;
}

static int   ibuf[N], rbuf[N], cmpa[N], cmpb[N];
static float tbuf[N], rtbuf[N];

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

/* Brute-force oracle: mirrors ray_disc_t (incl. the impl's renormalization of
 * (dx,dy)) so the float math is identical; no wall occlusion modelled here. */
static int ray_oracle(SimP *s, float ox, float oy, float dx, float dy,
                      float maxd, int *out, float *out_t, int max_out) {
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-8f) return 0;
    dx /= len; dy /= len;
    const float *px = simp_px(s), *py = simp_py(s), *rad = simp_radius_arr(s);
    int n = 0;
    for (int i = 0; i < simp_count(s); i++) {
        float ocx = px[i] - ox, ocy = py[i] - oy;
        float tca = ocx * dx + ocy * dy;
        float r = rad[i];
        float d2 = ocx * ocx + ocy * ocy - tca * tca;
        float r2 = r * r;
        if (d2 > r2) continue;
        float thc = sqrtf(r2 - d2);
        if (tca + thc < 0.0f) continue;
        float t0 = tca - thc;
        float thit = t0 > 0.0f ? t0 : 0.0f;
        if (thit > maxd) continue;
        if (n < max_out) { out[n] = i; out_t[n] = thit; n++; }
        else {
            int w = 0;
            for (int q = 1; q < max_out; q++) if (out_t[q] > out_t[w]) w = q;
            if (thit < out_t[w]) { out[w] = i; out_t[w] = thit; }
        }
    }
    return n;
}

/* compare two index sets (order-independent) */
static int set_equal(const int *a, int na, const int *b, int nb) {
    if (na != nb) return 0;
    for (int i = 0; i < na; i++) cmpa[i] = a[i];
    for (int i = 0; i < nb; i++) cmpb[i] = b[i];
    qsort(cmpa, (size_t)na, sizeof(int), cmp_int);
    qsort(cmpb, (size_t)nb, sizeof(int), cmp_int);
    for (int i = 0; i < na; i++) if (cmpa[i] != cmpb[i]) return 0;
    return 1;
}

int main(void) {
    float ww = GW * CELL, wh = GH * CELL;

    /* ---- A) random rays vs brute force (no walls) ---------------------- */
    SimP *s = simp_create(GW, GH, CELL, N);
    simp_terrain_commit(s);
    for (int k = 0; k < N; k++)
        simp_spawn(s, 0.5f + frnd() * (ww - 1.0f), 0.5f + frnd() * (wh - 1.0f));
    simp_step(s, DT);                          /* build the collision grid */

    int bad_set = 0, bad_order = 0;
    for (int q = 0; q < 400; q++) {
        float ox = frnd() * ww, oy = frnd() * wh;
        float ang = frnd() * 6.2831853f;
        float dx = cosf(ang), dy = sinf(ang);
        float maxd = 5.0f + frnd() * 30.0f;
        int n  = simp_query_ray(s, ox, oy, dx, dy, maxd, ibuf, tbuf, N,
                                SIMP_RAY_NOWALL);
        int m  = ray_oracle(s, ox, oy, dx, dy, maxd, rbuf, rtbuf, N);
        if (!set_equal(ibuf, n, rbuf, m)) bad_set++;
        for (int k = 1; k < n; k++) if (tbuf[k] < tbuf[k - 1]) { bad_order++; break; }
    }
    printf("A) random rays: 400 run, %d set mismatches, %d order violations\n",
           bad_set, bad_order);

    /* stale-grid fallback: kill one agent (grid goes stale), keep comparing */
    int bad_stale = 0;
    simp_kill(s, simp_count(s) / 2);
    for (int q = 0; q < 100; q++) {
        float ox = frnd() * ww, oy = frnd() * wh;
        float ang = frnd() * 6.2831853f;
        float dx = cosf(ang), dy = sinf(ang);
        float maxd = 5.0f + frnd() * 30.0f;
        int n = simp_query_ray(s, ox, oy, dx, dy, maxd, ibuf, tbuf, N,
                               SIMP_RAY_NOWALL);
        int m = ray_oracle(s, ox, oy, dx, dy, maxd, rbuf, rtbuf, N);
        if (!set_equal(ibuf, n, rbuf, m)) bad_stale++;
    }
    printf("A') stale-grid fallback: 100 run, %d mismatches\n", bad_stale);
    simp_destroy(s);

    /* ---- B) piercing vs non-piercing (colinear) ----------------------- */
    s = simp_create(GW, GH, CELL, 64);
    simp_terrain_commit(s);
    float ty = wh * 0.5f, tx = 5.0f;
    int line[5];
    for (int k = 0; k < 5; k++) line[k] = simp_spawn(s, tx + 1.0f + (float)k, ty);
    simp_step(s, DT);
    int np = simp_query_ray(s, tx, ty, 1.0f, 0.0f, 10.0f, ibuf, tbuf, 16,
                            SIMP_RAY_NOWALL);
    int bad_pierce = (np != 5) || (ibuf[0] != line[0]) || (ibuf[4] != line[4]);
    for (int k = 1; k < np; k++) if (tbuf[k] <= tbuf[k - 1]) bad_pierce = 1;
    /* non-piercing = max_out 1 → only the nearest */
    int n1 = simp_query_ray(s, tx, ty, 1.0f, 0.0f, 10.0f, ibuf, tbuf, 1,
                            SIMP_RAY_NOWALL);
    int bad_nonpierce = (n1 != 1) || (ibuf[0] != line[0]);
    printf("B) piercing: %d hits (want 5), order ok=%d; non-pierce nearest ok=%d\n",
           np, !bad_pierce, !bad_nonpierce);
    simp_destroy(s);

    /* ---- C) wall occlusion (line-of-sight) ---------------------------- */
    s = simp_create(GW, GH, CELL, 64);
    int wcx = (int)((tx + 5.0f) / CELL);
    int cyt = (int)(ty / CELL);
    for (int cy = cyt - 3; cy <= cyt + 3; cy++) simp_set_wall(s, wcx, cy, true);
    simp_terrain_commit(s);
    int aNear = simp_spawn(s, tx + 2.0f, ty);   /* before the wall */
    int aFar  = simp_spawn(s, tx + 8.0f, ty);   /* beyond the wall */
    simp_step(s, DT);
    /* default: occluded → only the near agent */
    int no = simp_query_ray(s, tx, ty, 1.0f, 0.0f, 20.0f, ibuf, tbuf, 16, 0);
    int occ_near = 0, occ_far = 0;
    for (int k = 0; k < no; k++) { if (ibuf[k] == aNear) occ_near = 1;
                                   if (ibuf[k] == aFar)  occ_far = 1; }
    /* NOWALL: both */
    int nn = simp_query_ray(s, tx, ty, 1.0f, 0.0f, 20.0f, ibuf, tbuf, 16,
                            SIMP_RAY_NOWALL);
    int los_near = 0, los_far = 0;
    for (int k = 0; k < nn; k++) { if (ibuf[k] == aNear) los_near = 1;
                                   if (ibuf[k] == aFar)  los_far = 1; }
    int bad_los = !(occ_near && !occ_far) || !(los_near && los_far);
    printf("C) LoS: occluded {near=%d far=%d}, no-wall {near=%d far=%d}, ok=%d\n",
           occ_near, occ_far, los_near, los_far, !bad_los);
    simp_destroy(s);

    /* ---- D) simp_set_vpref: crawl advances far less ------------------- */
    s = simp_create(GW, GH, CELL, 64);
    for (int cy = 0; cy < GH; cy++) simp_set_goal(s, GW - 1, cy, true);  /* right edge */
    simp_terrain_commit(s);
    int iN = simp_spawn(s, 5.0f, wh * 0.4f);
    int iC = simp_spawn(s, 5.0f, wh * 0.6f);
    simp_set_vpref(s, iC, 0.2f);                /* crawl */
    SimPHandle hN = simp_handle_of(s, iN), hC = simp_handle_of(s, iC);
    for (int k = 0; k < 120; k++) simp_step(s, DT);   /* 2 s */
    float xN = simp_px(s)[simp_index_of(s, hN)];
    float xC = simp_px(s)[simp_index_of(s, hC)];
    float dN = xN - 5.0f, dC = xC - 5.0f;
    int bad_vpref = !(dC > 0.0f && dC < dN * 0.5f);
    printf("D) set_vpref: normal advanced %.2f m, crawl %.2f m, ok=%d\n",
           dN, dC, !bad_vpref);
    simp_destroy(s);

    int ok = !bad_set && !bad_order && !bad_stale && !bad_pierce &&
             !bad_nonpierce && !bad_los && !bad_vpref;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

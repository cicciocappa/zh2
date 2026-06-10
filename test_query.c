/* test_query.c — spatial queries (M3.4) against brute force.
 *
 * 10k agents at random positions, one step to build the collision grid,
 * then: circle queries vs O(N) reference (set equality), nearest vs O(N)
 * argmin (distance equality), buffer saturation, and a turret scenario
 * (nearest + kill via handles, which also exercises the stale-grid brute
 * force fallback and the kill-in-descending-order pattern).
 */
#include "sim_particles.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define N 10000
#define DT (1.0f / 60.0f)

static uint32_t rs = 7u;
static float frnd(void) {
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return (float)(rs >> 8) / 16777216.0f;
}

static int idx_buf[N], ref_buf[N];

static int cmp_int(const void *a, const void *b) {
    return *(const int *)a - *(const int *)b;
}

int main(void) {
    SimP *s = simp_create(GW, GH, CELL, N);
    simp_terrain_commit(s);
    float ww = GW * CELL, wh = GH * CELL;
    for (int k = 0; k < N; k++)
        simp_spawn(s, 0.5f + frnd() * (ww - 1.0f), 0.5f + frnd() * (wh - 1.0f));
    simp_step(s, DT);                       /* builds the collision grid */

    const float *px = simp_px(s), *py = simp_py(s);
    int bad_circle = 0, bad_nearest = 0;

    /* circle queries vs brute force */
    for (int q = 0; q < 200; q++) {
        float x = frnd() * ww, y = frnd() * wh, r = 0.5f + frnd() * 7.5f;
        int n = simp_query_circle(s, x, y, r, idx_buf, N, 0);
        int m = 0;
        for (int i = 0; i < simp_count(s); i++) {
            float dx = px[i] - x, dy = py[i] - y;
            if (dx * dx + dy * dy <= r * r) ref_buf[m++] = i;
        }
        qsort(idx_buf, (size_t)n, sizeof(int), cmp_int);
        if (n != m) { bad_circle++; continue; }
        for (int k = 0; k < n; k++) if (idx_buf[k] != ref_buf[k]) { bad_circle++; break; }
    }
    printf("circle queries: 200 run, %d mismatches\n", bad_circle);

    /* saturation: a small buffer must cap the result */
    int n_sat = simp_query_circle(s, ww * 0.5f, wh * 0.5f, 10.0f, idx_buf, 5, 0);
    printf("saturation: asked max 5, got %d\n", n_sat);
    int bad_sat = (n_sat != 5);

    /* nearest vs brute force (compare distances: ties may pick either) */
    for (int q = 0; q < 200; q++) {
        float x = frnd() * ww, y = frnd() * wh, rmx = 0.3f + frnd() * 9.7f;
        int n1 = simp_query_nearest(s, x, y, rmx);
        int n2 = -1; float bd2 = rmx * rmx;
        for (int i = 0; i < simp_count(s); i++) {
            float dx = px[i] - x, dy = py[i] - y;
            float d2 = dx * dx + dy * dy;
            if (d2 <= bd2) { bd2 = d2; n2 = i; }
        }
        if ((n1 < 0) != (n2 < 0)) { bad_nearest++; continue; }
        if (n1 >= 0) {
            float dx = px[n1] - x, dy = py[n1] - y;
            if (dx * dx + dy * dy != bd2) bad_nearest++;
        }
    }
    printf("nearest queries: 200 run, %d mismatches\n", bad_nearest);

    /* turret: nearest -> handle -> kill, until the 10 m circle is empty.
     * After the first kill the grid is stale: brute-force fallback path. */
    float tx = ww * 0.5f, ty = wh * 0.5f;
    int kills = 0, bad_turret = 0;
    int before = simp_count(s);
    for (;;) {
        int i = simp_query_nearest(s, tx, ty, 10.0f);
        if (i < 0) break;
        SimPHandle h = simp_handle_of(s, i);
        int j = simp_index_of(s, h);
        if (j != i) { bad_turret++; break; }
        simp_kill(s, j);
        if (simp_index_of(s, h) != -1) bad_turret++;   /* must be dead now */
        kills++;
        if (kills > before) { bad_turret++; break; }   /* runaway guard */
    }
    int bad_count = (simp_count(s) != before - kills);
    printf("turret: %d kills, count %d -> %d, bad=%d\n",
           before - simp_count(s), before, simp_count(s), bad_turret + bad_count);

    /* AoE pattern: query then kill in DECREASING index order (no handles) */
    int n_aoe = simp_query_circle(s, ww * 0.25f, wh * 0.25f, 6.0f, idx_buf, N, 0);
    qsort(idx_buf, (size_t)n_aoe, sizeof(int), cmp_int);
    int before_aoe = simp_count(s);
    for (int k = n_aoe - 1; k >= 0; k--) simp_kill(s, idx_buf[k]);
    int bad_aoe = (simp_count(s) != before_aoe - n_aoe);
    printf("aoe: killed %d by decreasing index, count ok=%d\n", n_aoe, !bad_aoe);

    simp_destroy(s);
    int ok = !bad_circle && !bad_sat && !bad_nearest && !bad_turret &&
             !bad_count && !bad_aoe;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

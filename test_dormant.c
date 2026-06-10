/* test_dormant.c — headless check of the behaviour-state additions.
 *
 * Verifies:
 *   1) simp_free_at admission: a pack placed by rejection sampling has zero
 *      overlapping pairs (no PBD ejection transient at spawn).
 *   2) Dormant agents hold position exactly (no drift, no drain) while a
 *      goal is present and attracting.
 *   3) simp_wake_radius wakes only the targeted subset; the woken agents
 *      march to the goal and drain, the sleepers stay.
 *   4) simp_wake_all empties the map. No NaN anywhere.
 */
#include "sim_particles.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define GW   80
#define GH   60
#define CELL 0.5f                 /* 40 m x 30 m world */
#define DT   (1.0f / 60.0f)

static uint32_t rs = 12345u;
static float frnd(void) {        /* xorshift, [0,1) — keep the test deterministic */
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return (float)(rs >> 8) / 16777216.0f;
}

int main(void) {
    SimP *s = simp_create(GW, GH, CELL, 8000);
    for (int cy = 1; cy < GH - 1; cy++) simp_set_goal(s, GW - 2, cy, true);
    simp_terrain_commit(s);

    /* 1) pack placement: rejection sampling into a 10x10 m square */
    const float x0 = 5.0f, y0 = 10.0f, side = 10.0f;
    const float rchk = 0.30f * 1.15f;          /* radius * (1 + r_jitter) */
    for (int k = 0; k < 20000; k++) {
        float x = x0 + frnd() * side, y = y0 + frnd() * side;
        if (simp_free_at(s, x, y, rchk)) simp_spawn_dormant(s, x, y);
    }
    int n0 = simp_count(s);
    const float *px = simp_px(s), *py = simp_py(s), *rad = simp_radius_arr(s);
    int overlaps = 0;
    for (int i = 0; i < n0; i++) for (int j = i + 1; j < n0; j++) {
        float dx = px[i] - px[j], dy = py[i] - py[j];
        float rsum = rad[i] + rad[j];
        if (dx * dx + dy * dy < rsum * rsum * 0.9999f) overlaps++;
    }
    printf("pack placed: %d agents, overlapping pairs: %d\n", n0, overlaps);
    int ok_place = (n0 >= 100) && (overlaps == 0);

    /* 2) dormant pack must hold position (goal attracts, sleepers ignore it) */
    float *sx = (float *)malloc((size_t)n0 * sizeof(float));
    float *sy = (float *)malloc((size_t)n0 * sizeof(float));
    for (int i = 0; i < n0; i++) { sx[i] = px[i]; sy[i] = py[i]; }
    int drained = 0;
    for (int t = 0; t < 300; t++) drained += simp_step(s, DT);
    float maxd = 0.0f;
    for (int i = 0; i < simp_count(s); i++) {
        float dx = px[i] - sx[i], dy = py[i] - sy[i];
        float d = sqrtf(dx * dx + dy * dy);
        if (d > maxd) maxd = d;
    }
    free(sx); free(sy);
    printf("300 dormant steps: drained=%d, max displacement=%.6f m\n", drained, maxd);
    int ok_hold = (drained == 0) && (simp_count(s) == n0) && (maxd < 1e-3f);

    /* 3) wake a circle over the goal-side part of the pack: the subset
     * marches off unobstructed and drains. (Waking the far side also works
     * but is slow: the marchers have to shove through the sleeping pack.) */
    simp_wake_radius(s, x0 + side - 2.0f, y0 + 0.5f * side, 4.0f);
    const uint8_t *dor = simp_dormant_arr(s);
    int sleepers = 0;
    for (int i = 0; i < simp_count(s); i++) sleepers += dor[i];
    int woken = n0 - sleepers;
    printf("wake_radius: %d woken, %d still dormant\n", woken, sleepers);
    int ok_subset = (woken > 0) && (sleepers > 0);

    for (int t = 0; t < 1800; t++) drained += simp_step(s, DT);
    int sleepers2 = 0;
    for (int i = 0; i < simp_count(s); i++) sleepers2 += dor[i];
    printf("1800 steps later: drained=%d, dormant=%d, alive=%d\n",
           drained, sleepers2, simp_count(s));
    int ok_march = (drained > woken / 2) && (sleepers2 == sleepers);

    /* 4) sunrise: wake everyone, the map must (almost) fully drain */
    simp_wake_all(s);
    for (int t = 0; t < 3600; t++) drained += simp_step(s, DT);
    int nan = 0;
    for (int i = 0; i < simp_count(s); i++)
        if (px[i] != px[i] || py[i] != py[i]) nan++;
    printf("wake_all + 3600 steps: alive=%d, drained=%d, NaN=%d\n",
           simp_count(s), drained, nan);
    int ok_drain = (simp_count(s) < n0 / 20) && (nan == 0);

    simp_destroy(s);
    int ok = ok_place && ok_hold && ok_subset && ok_march && ok_drain;
    if (ok) printf("PASS\n");
    else    printf("FAIL (place=%d hold=%d subset=%d march=%d drain=%d)\n",
                   ok_place, ok_hold, ok_subset, ok_march, ok_drain);
    return ok ? 0 : 1;
}

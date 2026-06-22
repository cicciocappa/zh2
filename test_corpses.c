/* test_corpses.c — corpses as passive obstacles (M3.3) + finite mass (§3).
 *
 * A 4 m corridor with a marching column of ~200 agents.
 *   A) baseline: column drains at the far end;
 *   B) INFINITE-mass seal (corpse_weight <= 0, legacy idealized plug): a line of
 *      corpses seals the corridor at x=40, the column piles up — near-zero drain,
 *      corpses bit-identical (PBD never moves an infinite-mass ghost), bounded
 *      residual penetration even under full crowd pressure;
 *   C) FINITE-mass pile (§3, the default): the same line is large-but-finite, so
 *      the pushing crowd SHOVES it downstream and breaks through — the drain is
 *      clearly > 0 (it leaks) yet still well below the open corridor (it slows a
 *      lot), and the corpses have MOVED. This is the intended replacement of the
 *      total seal: "rallentano ma non fermano".
 *   D) TTL: a short-lived pile vanishes on schedule.
 * Zero NaN everywhere.
 */
#include "sim_particles.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define GW 120
#define GH 40
#define CELL 0.5f                 /* 60 m x 20 m world */
#define DT (1.0f / 60.0f)
#define STEPS 2700                /* 45 s: enough for the rear to drain */
#define NB 9

static SimP *corridor_create(void) {
    SimP *s = simp_create(GW, GH, CELL, 4000);
    /* solid except a 4 m band: corridor y in [8,12) m = rows 16..23 */
    for (int cy = 0; cy < GH; cy++) for (int cx = 0; cx < GW; cx++)
        if (cy < 16 || cy > 23) simp_set_wall(s, cx, cy, true);
    for (int cy = 16; cy <= 23; cy++) simp_set_goal(s, GW - 2, cy, true);
    simp_terrain_commit(s);
    for (int r = 0; r < 5; r++) for (int c = 0; c < 40; c++)
        simp_spawn(s, 2.0f + c * 0.7f + (r & 1) * 0.35f, 8.5f + r * 0.62f);
    return s;
}

static int check_nan(const SimP *s) {
    const float *px = simp_px(s), *py = simp_py(s);
    for (int i = 0; i < simp_count(s); i++)
        if (!isfinite(px[i]) || !isfinite(py[i])) return 1;
    return 0;
}

/* run a barrier scenario at a given corpse_weight; report drain, how many
 * corpses moved, and the worst residual penetration. */
static long barrier_run(float weight, int *moved_out, float *worst_out, int *nan_out) {
    SimP *s = corridor_create();
    simp_params(s)->corpse_weight = weight;
    float bx[NB], by[NB];
    for (int j = 0; j < NB; j++) {
        bx[j] = 40.0f;
        by[j] = 8.0f + 0.5f * (float)j;          /* overlaps both walls */
        simp_corpse_add(s, bx[j], by[j], 0.35f, 99999.0f);
    }
    long dr = 0;
    for (int t = 0; t < STEPS; t++) dr += simp_step(s, DT);
    *nan_out += check_nan(s);

    int moved = 0;
    for (int j = 0; j < NB; j++)
        if (simp_corpse_px(s)[j] != bx[j] || simp_corpse_py(s)[j] != by[j]) moved++;

    float worst = 0.0f;
    const float *px = simp_px(s), *py = simp_py(s), *rad = simp_radius_arr(s);
    const float *cpx = simp_corpse_px(s), *cpy = simp_corpse_py(s);
    for (int i = 0; i < simp_count(s); i++) for (int j = 0; j < simp_corpse_count(s); j++) {
        float dx = px[i] - cpx[j], dy = py[i] - cpy[j];
        float pen = (rad[i] + 0.35f) - sqrtf(dx * dx + dy * dy);
        if (pen > worst) worst = pen;
    }
    *moved_out = moved; *worst_out = worst;
    simp_destroy(s);
    return dr;
}

int main(void) {
    int nan = 0;

    /* A) baseline (open) */
    SimP *s = corridor_create();
    int n0 = simp_count(s);
    long drA = 0;
    for (int t = 0; t < STEPS; t++) drA += simp_step(s, DT);
    nan += check_nan(s);
    simp_destroy(s);

    /* B) infinite-mass seal (legacy) */
    int movedB; float worstB;
    long drB = barrier_run(0.0f, &movedB, &worstB, &nan);

    /* C) finite-mass pile (§3 default) */
    int movedC; float worstC;
    long drC = barrier_run(40.0f, &movedC, &worstC, &nan);

    printf("spawned %d | drained A=%ld (open)\n", n0, drA);
    printf("  B infinite: drained=%ld moved=%d worst_pen=%.3f m  (must SEAL)\n",
           drB, movedB, (double)worstB);
    printf("  C finite §3: drained=%ld moved=%d worst_pen=%.3f m  (slow but LEAKS)\n",
           drC, movedC, (double)worstC);

    /* B seals: near-zero drain, corpses immovable, bounded penetration. */
    int ok_seal = (drA > n0 / 2) && (drB < drA / 4) && (movedB == 0) && (worstB < 0.20f);
    /* C is the §3 replacement: it must LEAK (drain clearly > the sealed run) and
     * the corpses must have been SHOVED, yet it still slows the column well below
     * the open corridor (a real obstacle, not a wall). */
    int ok_finite = (drC > drB + n0 / 10) && (movedC > 0) && (drC < (3 * drA) / 4)
                    && isfinite((float)drC);

    /* D) TTL: a short-lived pile must vanish on schedule */
    s = simp_create(40, 40, CELL, 100);
    simp_terrain_commit(s);
    for (int j = 0; j < 10; j++)
        simp_corpse_add(s, 5.0f + j * 0.5f, 5.0f, 0.3f, 1.0f);
    for (int t = 0; t < 90; t++) simp_step(s, DT);
    printf("ttl: pool after 1.5 s = %d (expected 0)\n", simp_corpse_count(s));
    int ok_ttl = (simp_corpse_count(s) == 0);
    simp_destroy(s);

    int ok = ok_seal && ok_finite && ok_ttl && (nan == 0);
    printf("seal ok=%d  finite-leak ok=%d  ttl ok=%d  nan=%d\n",
           ok_seal, ok_finite, ok_ttl, nan);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

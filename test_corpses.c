/* test_corpses.c — corpses as passive obstacles (M3.3).
 *
 * A 4 m corridor with a marching column of ~200 agents.
 *   A) baseline: column drains at the far end;
 *   B) a line of corpses seals the corridor at x=40: the column must pile
 *      up against it — near-zero drain, corpses bit-identical (PBD never
 *      moves an infinite-mass ghost), bounded residual penetration even
 *      under full crowd pressure;
 *   C) TTL: a short-lived pile vanishes on schedule.
 * Zero NaN everywhere. (The emergent partial barricade from the KILL brush
 * is verified visually in the sandbox.)
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

int main(void) {
    /* A) baseline */
    SimP *s = corridor_create();
    int n0 = simp_count(s);
    long drA = 0;
    for (int t = 0; t < STEPS; t++) drA += simp_step(s, DT);
    int nan = check_nan(s);
    simp_destroy(s);

    /* B) corpse barrier sealing the corridor at x=40 */
    s = corridor_create();
    enum { NB = 9 };
    float bx[NB], by[NB];
    for (int j = 0; j < NB; j++) {
        bx[j] = 40.0f;
        by[j] = 8.0f + 0.5f * (float)j;          /* overlaps both walls */
        simp_corpse_add(s, bx[j], by[j], 0.35f, 99999.0f);
    }
    long drB = 0;
    for (int t = 0; t < STEPS; t++) drB += simp_step(s, DT);
    nan += check_nan(s);

    int moved = 0;
    for (int j = 0; j < NB; j++)
        if (simp_corpse_px(s)[j] != bx[j] || simp_corpse_py(s)[j] != by[j])
            moved++;

    /* residual agent-corpse penetration under full crowd pressure */
    float worst_pen = 0.0f;
    const float *px = simp_px(s), *py = simp_py(s), *rad = simp_radius_arr(s);
    for (int i = 0; i < simp_count(s); i++) for (int j = 0; j < NB; j++) {
        float dx = px[i] - bx[j], dy = py[i] - by[j];
        float pen = (rad[i] + 0.35f) - sqrtf(dx * dx + dy * dy);
        if (pen > worst_pen) worst_pen = pen;
    }
    simp_destroy(s);

    printf("spawned %d | drained A=%ld (open) B=%ld (sealed) | "
           "corpses moved=%d | worst penetration=%.3f m | nan=%d\n",
           n0, drA, drB, moved, (double)worst_pen, nan);
    int ok_block = (drA > n0 / 2) && (drB < drA / 4) &&
                   (moved == 0) && (worst_pen < 0.20f);

    /* C) TTL: a short-lived pile must vanish on schedule */
    s = simp_create(40, 40, CELL, 100);
    simp_terrain_commit(s);
    for (int j = 0; j < 10; j++)
        simp_corpse_add(s, 5.0f + j * 0.5f, 5.0f, 0.3f, 1.0f);
    for (int t = 0; t < 90; t++) simp_step(s, DT);
    printf("ttl: pool after 1.5 s = %d (expected 0)\n", simp_corpse_count(s));
    int ok_ttl = (simp_corpse_count(s) == 0);
    simp_destroy(s);

    int ok = ok_block && ok_ttl && (nan == 0);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

/* test_drag.c — draggable objects + breakable barricades (DRAG_DESIGN.md).
 *
 * A draggable is the third PBD body category: a finite-mass disc that the crowd
 * SHOVES, that carries its own MOMENTUM (coasts after the push stops) and is
 * braked by FRICTION (drag_damp), and that COLLIDES with walls and with other
 * draggables. A row of accosted draggables is a barricade the horde overruns.
 *
 * Verifies, causally:
 *   1) momentum + friction — a shoved draggable keeps gliding after the crowd is
 *      gone (displacement > 0) and its speed decays to ~0 (geometric, drag_damp);
 *   2) mass ratio — same crowd shove moves a light object far more than a heavy;
 *   3) breakable barricade — a corridor sealed by a draggable row drains the
 *      horde (the crowd squeezes the discs aside), while the same corridor sealed
 *      by a SOLID wall drains nothing; a light barricade falls faster than a heavy;
 *   4) wall collision — a draggable shoved against a wall never penetrates it;
 *   5) determinism (bit-identical re-run) + no NaN / out-of-bounds.
 */
#include "sim_particles.h"
#include <stdio.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)

static int any_bad = 0;

static void check_finite(SimP *s) {
    const float *px = simp_px(s), *py = simp_py(s);
    for (int i = 0; i < simp_count(s); i++)
        if (!isfinite(px[i]) || !isfinite(py[i])) any_bad++;
    const float *dx = simp_drag_px(s), *dy = simp_drag_py(s);
    const float *dvx = simp_drag_vx(s), *dvy = simp_drag_vy(s);
    for (int j = 0; j < simp_drag_count(s); j++)
        if (!isfinite(dx[j]) || !isfinite(dy[j]) ||
            !isfinite(dvx[j]) || !isfinite(dvy[j])) any_bad++;
}

/* hex-staggered crowd block in [x0,x1]x[y0,y1]; all agents share the live goal. */
static int spawn_block(SimP *s, float x0, float x1, float y0, float y1, float sp) {
    int n = 0;
    for (int row = 0; ; row++) {
        float y = y0 + row * sp * 0.866f;
        if (y > y1) break;
        float xoff = (row & 1) ? sp * 0.5f : 0.0f;
        for (int col = 0; ; col++) {
            float x = x0 + xoff + col * sp;
            if (x > x1) break;
            if (simp_spawn(s, x, y) >= 0) n++;
        }
    }
    return n;
}

/* ---- 1) momentum + friction -------------------------------------------- */
/* Shove one draggable with a crowd flowing rightward, then KILL the crowd and
 * watch the object coast and brake to rest. */
static int test_momentum(void) {
    SimP *s = simp_create(GW, GH, CELL, 4000);
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
    for (int y = 26; y < 34; y++) simp_set_goal(s, GW - 6, y, true);   /* far right */
    simp_terrain_commit(s);

    int di = simp_drag_add(s, 30.0f, 15.0f, 0.5f, 5.0f);              /* mass 5 */
    (void)di;
    /* dense crowd just left of the object, flowing right into it */
    spawn_block(s, 22.0f, 28.0f, 12.0f, 18.0f, 0.66f);

    float peak = 0.0f;
    for (int step = 0; step < 90; step++) {                          /* push phase */
        simp_step(s, DT);
        check_finite(s);
        float v = hypotf(simp_drag_vx(s)[0], simp_drag_vy(s)[0]);
        if (v > peak) peak = v;
    }
    float x_release = simp_drag_px(s)[0];
    for (int i = simp_count(s) - 1; i >= 0; i--) simp_kill(s, i);    /* crowd gone */

    float vmax_after = 0.0f, x_final = x_release;
    for (int step = 0; step < 150; step++) {                         /* coast phase */
        simp_step(s, DT);
        check_finite(s);
        x_final = simp_drag_px(s)[0];
        float v = hypotf(simp_drag_vx(s)[0], simp_drag_vy(s)[0]);
        if (step > 100 && v > vmax_after) vmax_after = v;            /* late speed */
    }
    float coast = x_final - x_release;
    simp_destroy(s);

    int ok_push  = peak > 0.15f;                  /* the crowd really moved it    */
    int ok_coast = coast > 0.10f;                 /* it kept gliding after release */
    int ok_stop  = vmax_after < 0.05f * peak;     /* friction brought it to rest  */
    printf("1) momentum: peak=%.3f m/s coast=%.3f m late_v=%.4f m/s  push=%d coast=%d stop=%d\n",
           peak, coast, vmax_after, ok_push, ok_coast, ok_stop);
    return ok_push && ok_coast && ok_stop;
}

/* ---- 2) mass ratio ----------------------------------------------------- */
static float shove_displacement(float mass) {
    SimP *s = simp_create(GW, GH, CELL, 4000);
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
    for (int y = 26; y < 34; y++) simp_set_goal(s, GW - 6, y, true);
    simp_terrain_commit(s);
    simp_drag_add(s, 30.0f, 15.0f, 0.5f, mass);
    spawn_block(s, 22.0f, 28.0f, 12.0f, 18.0f, 0.66f);
    float x0 = simp_drag_px(s)[0];
    for (int step = 0; step < 240; step++) { simp_step(s, DT); check_finite(s); }
    float d = simp_drag_px(s)[0] - x0;
    simp_destroy(s);
    return d;
}

/* ---- 3) breakable barricade -------------------------------------------- */
/* Horizontal corridor; the only path runs straight through x = BARR. Three runs:
 * a SOLID wall there (sealed -> 0 drain), a HEAVY draggable row, a LIGHT row.   */
enum { BARRIER_SOLID, BARRIER_DRAG };
#define LANE_Y0 52          /* lane spans cells [52,68) = y in [26,34) m */
#define LANE_Y1 68
#define BARR_CX 80          /* barricade column (x = 40 m)              */

static int run_barricade(int kind, float mass, float *drag_checksum) {
    SimP *s = simp_create(GW, GH, CELL, 9000);
    /* corridor: solid everywhere outside the lane (+ borders implicit) */
    for (int cy = 0; cy < GH; cy++)
        for (int cx = 0; cx < GW; cx++)
            if (cy < LANE_Y0 || cy >= LANE_Y1 || cx == 0 || cx == GW - 1)
                simp_set_wall(s, cx, cy, true);
    for (int cy = LANE_Y0; cy < LANE_Y1; cy++) simp_set_goal(s, GW - 4, cy, true);

    if (kind == BARRIER_SOLID)
        for (int cy = LANE_Y0; cy < LANE_Y1; cy++) simp_set_wall(s, BARR_CX, cy, true);
    simp_terrain_commit(s);

    if (kind == BARRIER_DRAG) {
        /* a vertical row of accosted discs sealing the lane (y in [26,34) m) */
        float bx = (BARR_CX + 0.5f) * CELL;
        for (float y = 26.6f; y < 34.0f; y += 1.18f)
            simp_drag_add(s, bx, y, 0.6f, mass);
    }

    /* horde filling the left half of the lane, flowing right. A moderate crowd
     * (not a saturating flood) keeps the mass effect on breakthrough visible:
     * the design note warns a huge horde washes the difference out. */
    spawn_block(s, 4.0f, 34.0f, 26.6f, 33.4f, 0.70f);

    int drained = 0;
    for (int step = 0; step < 2600; step++) {                       /* ~43 s */
        drained += simp_step(s, DT);
        check_finite(s);
    }
    float sum = 0.0f;
    for (int j = 0; j < simp_drag_count(s); j++)
        sum += simp_drag_px(s)[j] * 1.0f + simp_drag_py(s)[j] * 3.0f;
    if (drag_checksum) *drag_checksum = sum;
    simp_destroy(s);
    return drained;
}

/* ---- 4) wall collision ------------------------------------------------- */
/* Shove a draggable straight into a wall; its center must never enter it. */
static int test_wall_collision(void) {
    SimP *s = simp_create(GW, GH, CELL, 4000);
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
    /* a solid block on the right the object will be pushed into */
    for (int cx = 100; cx < GW; cx++)
        for (int cy = 40; cy < 80; cy++) simp_set_wall(s, cx, cy, true);
    for (int cy = 50; cy < 70; cy++) simp_set_goal(s, 99, cy, true); /* lure crowd rightward */
    simp_terrain_commit(s);

    float R = 0.6f;
    simp_drag_add(s, 47.0f, 29.0f, R, 4.0f);       /* near the x=50 m wall face */
    spawn_block(s, 30.0f, 45.0f, 26.0f, 32.0f, 0.66f);

    float min_clear = 1e9f;
    for (int step = 0; step < 600; step++) {
        simp_step(s, DT);
        check_finite(s);
        float d = simp_sample_sdf(s, simp_drag_px(s)[0], simp_drag_py(s)[0]);
        if (d < min_clear) min_clear = d;
    }
    simp_destroy(s);
    int ok = min_clear > -0.05f;                   /* center never inside a wall */
    printf("4) wall: min center-to-wall distance = %.3f m (radius %.2f)  ok=%d\n",
           min_clear, R, ok);
    return ok;
}

int main(void) {
    int ok1 = test_momentum();

    float d_light = shove_displacement(5.0f);
    float d_heavy = shove_displacement(40.0f);
    int ok2 = d_light > 0.20f && d_light > 2.0f * d_heavy;
    printf("2) mass ratio: light(m5)=%.3f m  heavy(m40)=%.3f m  ok=%d\n",
           d_light, d_heavy, ok2);

    float cs_a = 0.0f, cs_b = 0.0f;
    int dr_solid = run_barricade(BARRIER_SOLID, 0.0f, NULL);
    int dr_heavy = run_barricade(BARRIER_DRAG, 30.0f, &cs_a);
    int dr_light = run_barricade(BARRIER_DRAG, 6.0f,  NULL);
    int dr_heavy2 = run_barricade(BARRIER_DRAG, 30.0f, &cs_b);
    printf("3) barricade: solid-wall drain=%d  heavy-row=%d  light-row=%d\n",
           dr_solid, dr_heavy, dr_light);
    /* solid wall seals (0 drain); the draggable row is overrun (>>0); and the
     * lighter row falls clearly faster than the heavy one (mass matters). */
    int ok3 = (dr_solid == 0) && (dr_heavy > 30) && (dr_light > dr_heavy + dr_heavy / 4);

    int ok4 = test_wall_collision();

    int ok_det = (cs_a == cs_b) && (dr_heavy == dr_heavy2);
    int ok_nan = (any_bad == 0);
    printf("5) determinism=%d (checksum %.3f vs %.3f, drain %d vs %d)  no-nan=%d\n",
           ok_det, cs_a, cs_b, dr_heavy, dr_heavy2, ok_nan);

    int ok = ok1 && ok2 && ok3 && ok4 && ok_det && ok_nan;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

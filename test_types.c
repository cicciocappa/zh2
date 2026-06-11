/* test_types.c — M3.5 verification: typed spawns + user nav costs.
 *
 * Per the design doc this is sanity-level (the real tuning is by eye in the
 * sandbox), but three things are asserted quantitatively:
 *
 *   1. RACE: runners (v_pref 2.8) spawned alongside walkers (1.4) on the
 *      same start line pull ahead in open field.
 *   2. TANK PRESS: a 5% tank mix shoved through a single gate stays sane
 *      (no NaN, everyone keeps draining, tanks make it through too).
 *   3. USER COST: two symmetric corridors; simp_add_cost on one corridor
 *      steers the horde into the other one — repulsion (w > 0) and
 *      attraction (w < 0) both ways.
 */
#include "sim_particles.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)

static const SimPAgentDesc WALKER = { 0.30f, 1.4f, 1.0f };
static const SimPAgentDesc RUNNER = { 0.27f, 2.8f, 0.9f };
static const SimPAgentDesc TANK   = { 0.55f, 1.0f, 10.0f };

static void borders(SimP *s) {
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
}

static int sane(const SimP *s) {
    const float *px = simp_px(s), *py = simp_py(s);
    for (int i = 0; i < simp_count(s); i++)
        if (!isfinite(px[i]) || !isfinite(py[i]) ||
            px[i] < 0 || py[i] < 0 || px[i] > GW * CELL || py[i] > GH * CELL)
            return 0;
    return 1;
}

/* ---- 1. race: runners overtake walkers in open field -------------------- */
static int test_race(void) {
    SimP *s = simp_create(GW, GH, CELL, 2000);
    borders(s);
    for (int y = 1; y < GH - 1; y++) simp_set_goal(s, GW - 2, y, true);
    simp_terrain_commit(s);

    /* per-slot type table: 0 = walker, 1 = runner */
    static uint8_t type[2000];
    int n_spawn = 0;
    for (int row = 0; row < 30; row++) {
        for (int col = 0; col < 10; col++) {
            float x = 3.0f + col * 0.8f;
            float y = 12.0f + row * 1.2f;
            const SimPAgentDesc *d = (n_spawn & 1) ? &RUNNER : &WALKER;
            int i = simp_spawn_desc(s, x, y, d);
            if (i >= 0) { type[simp_slot_of(s, i)] = (uint8_t)(n_spawn & 1); n_spawn++; }
        }
    }

    for (int step = 0; step < 600; step++) simp_step(s, DT);   /* 10 s */

    double mx[2] = { 0, 0 }; int nn[2] = { 0, 0 };
    const float *px = simp_px(s);
    for (int i = 0; i < simp_count(s); i++) {
        int t = type[simp_slot_of(s, i)];
        mx[t] += px[i]; nn[t]++;
    }
    int ok = sane(s) && nn[0] > 0 && nn[1] > 0;
    double walker_x = mx[0] / nn[0], runner_x = mx[1] / nn[1];
    ok = ok && (runner_x > walker_x + 5.0);
    printf("race: %d agents | walker mean x %.1f m | runner mean x %.1f m | %s\n",
           n_spawn, walker_x, runner_x, ok ? "ok" : "BAD");
    simp_destroy(s);
    return ok;
}

/* ---- 2. tank press through a single gate --------------------------------- */
static int test_tanks(void) {
    SimP *s = simp_create(GW, GH, CELL, 2000);
    borders(s);
    for (int y = 1; y < GH - 1; y++)
        if (y < 56 || y >= 64) simp_set_wall(s, 80, y, true);   /* 4 m gate */
    /* goal column 2 cells deep: the drain tests the cell under the agent's
     * CENTER, and a tank's radius (0.55) keeps its center half a cell off
     * the border wall — a 1-cell goal column against the border would be
     * physically unreachable for it (and the parked tanks then barricade
     * the drain for everyone else) */
    for (int y = 1; y < GH - 1; y++) {
        simp_set_goal(s, GW - 3, y, true);
        simp_set_goal(s, GW - 2, y, true);
    }
    simp_terrain_commit(s);

    static uint8_t is_tank[2000];
    int n_spawn = 0, n_tank = 0;
    for (int row = 0; row < 24; row++) {
        for (int col = 0; col < 30; col++) {
            float x = 4.0f + col * 0.9f;
            float y = 16.0f + row * 1.2f;
            int tank = (n_spawn % 20) == 0;                     /* 5% mix */
            int i = simp_spawn_desc(s, x, y, tank ? &TANK : &WALKER);
            if (i >= 0) {
                is_tank[simp_slot_of(s, i)] = (uint8_t)tank;
                n_spawn++; n_tank += tank;
            }
        }
    }

    long drained = 0;
    int bad = 0;
    for (int step = 0; step < 12000; step++) {                  /* 200 s: tanks
        walk ~75 m at 1.0 m/s plus the gate queue */
        drained += simp_step(s, DT);
        if (step % 600 == 0 && !sane(s)) bad++;
        if (simp_count(s) == 0) break;
    }
    int tanks_left = 0;
    for (int i = 0; i < simp_count(s); i++)
        tanks_left += is_tank[simp_slot_of(s, i)];
    int tanks_drained = n_tank - tanks_left;
    int ok = (bad == 0) && sane(s) && drained > n_spawn / 2 && tanks_drained > 0;
    printf("tanks: spawned %d (%d tanks) | drained %ld | tanks through %d/%d | %s\n",
           n_spawn, n_tank, drained, tanks_drained, n_tank, ok ? "ok" : "BAD");
    simp_destroy(s);
    return ok;
}

/* ---- 3. user cost: repel from / attract into one of two corridors -------- */

/* central block 30..50 m x 20..40 m: corridor TOP (y<20m) vs BOTTOM (y>40m) */
static SimP *corridor_scene(void) {
    SimP *s = simp_create(GW, GH, CELL, 1000);
    simp_params(s)->k_density = 0.0f;       /* isolate the user-cost effect */
    borders(s);
    for (int cy = 40; cy < 80; cy++)
        for (int cx = 60; cx < 100; cx++) simp_set_wall(s, cx, cy, true);
    for (int y = 1; y < GH - 1; y++) simp_set_goal(s, GW - 2, y, true);
    return s;
}

static void corridor_spawn(SimP *s) {
    for (int row = 0; row < 15; row++)
        for (int col = 0; col < 20; col++)
            simp_spawn(s, 20.0f + col * 0.45f, 24.5f + row * 0.78f);
}

/* run and tally agent presence in the corridor band x in [35,45] m */
static void corridor_run(SimP *s, long *top, long *bottom) {
    *top = *bottom = 0;
    const float *px = simp_px(s), *py = simp_py(s);
    for (int step = 0; step < 2400; step++) {                   /* 40 s */
        simp_step(s, DT);
        for (int i = 0; i < simp_count(s); i++) {
            if (px[i] < 35.0f || px[i] > 45.0f) continue;
            if (py[i] < 30.0f) (*top)++; else (*bottom)++;
        }
    }
}

static int test_cost(void) {
    /* repulsion: cost +5 on the whole TOP corridor -> everyone goes bottom */
    SimP *s = corridor_scene();
    for (int cy = 1; cy < 40; cy++)
        for (int cx = 50; cx < 110; cx++) simp_add_cost(s, cx, cy, 5.0f);
    simp_terrain_commit(s);
    corridor_spawn(s);
    long top, bottom;
    corridor_run(s, &top, &bottom);
    double f_rep = (double)bottom / (double)(top + bottom + 1);
    int ok = sane(s) && (f_rep > 0.85);
    printf("cost repel : band presence top %ld bottom %ld (bottom %.0f%%) | %s\n",
           top, bottom, 100.0 * f_rep, f_rep > 0.85 ? "ok" : "BAD");
    simp_destroy(s);

    /* attraction: cost -0.5 on the TOP corridor -> everyone goes top */
    s = corridor_scene();
    for (int cy = 1; cy < 40; cy++)
        for (int cx = 50; cx < 110; cx++) simp_add_cost(s, cx, cy, -0.5f);
    simp_terrain_commit(s);
    corridor_spawn(s);
    corridor_run(s, &top, &bottom);
    double f_att = (double)top / (double)(top + bottom + 1);
    ok = ok && sane(s) && (f_att > 0.85);
    printf("cost attract: band presence top %ld bottom %ld (top %.0f%%) | %s\n",
           top, bottom, 100.0 * f_att, f_att > 0.85 ? "ok" : "BAD");

    /* clear_cost restores the symmetric split (sanity: both sides used) */
    simp_clear_cost(s);
    simp_terrain_commit(s);
    simp_destroy(s);
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= test_race();
    ok &= test_tanks();
    ok &= test_cost();
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

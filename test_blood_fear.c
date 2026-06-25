/* test_blood_fear.c — blood-fear -> Dijkstra cost (CORPSE_DESIGN.md, 2026-06-25).
 *
 * The anti-choke pivot: every death stains its cell (simp_add_danger), and the
 * horde's animal instinct reroutes AWAY from cells soaked in other zombies'
 * blood. This REPLACES the corpse-pile nav cost (k_corpse, now default 0).
 *
 * Scene: a wall with a DIRECT gap (aligned spawn->goal, the shortest route) and
 * a DETOUR gap offset below. With no fear the horde funnels through the direct
 * gap. We paint sustained blood over the direct gap's approach (modeling a
 * killzone where a turret keeps killing), then:
 *
 *   - k_danger = 0 (fear off): the horde keeps using the direct gap;
 *   - k_danger = default: the bloody approach prices itself out and the horde
 *     reroutes through the clean detour. NOT a hard seal — both runs drain
 *     (fear deters, it doesn't wall off; a forced horde would still flood in).
 *
 * Part 2 verifies the decay matches danger_hl; part 3 determinism + no NaN.
 */
#include "sim_particles.h"
#include <stdio.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)
#define MAX_AGENTS 1500
#define MAX_STEPS 9000              /* 150 s cap */
#define WALL_X 40.0f                /* wall line (m), cell 80 */
#define DETOUR_Y 26.0f              /* crossings below this = detour */

typedef struct {
    int spawned, drained, steps;
    long cross_direct, cross_detour;
    int bad;
} RunStats;

/* paint a bloody band over the direct gap and its left approach (the killzone
 * in front of the turret), topping it up every step so it stays saturated. */
static void paint_blood(SimP *s) {
    /* dense, wider than the 4 m direct gap (y 28..32) so it saturates a SOLID
     * band with no low-cost thread for the flow to slip through */
    for (float x = 32.0f; x <= 40.0f; x += 0.4f)
        for (float y = 26.0f; y <= 34.0f; y += 0.4f)
            simp_add_danger(s, x, y, 0.45f, 0.05f);
}

static RunStats run(float k_danger, int blood) {
    RunStats st = { 0, 0, 0, 0, 0, 0 };
    SimP *s = simp_create(GW, GH, CELL, MAX_AGENTS);
    simp_params(s)->k_danger = k_danger;
    simp_params(s)->k_density = 0.0f;   /* isolate blood-fear: density & jam */
    simp_params(s)->k_jam     = 0.0f;   /* would themselves reroute the crowd */

    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
    /* wall at x = 40 m: DIRECT gap y 28..32 m (cells 56..64), DETOUR gap
     * y 20..24 m (cells 40..48), offset 8 m below. Both nav-open & passable. */
    for (int y = 1; y < GH - 1; y++) {
        bool direct = (y >= 56 && y < 64);
        bool detour = (y >= 40 && y < 48);
        if (!direct && !detour) simp_set_wall(s, 80, y, true);
    }
    for (int y = 54; y < 66; y++) simp_set_goal(s, 150, y, true);   /* goal on the right */
    simp_terrain_commit(s);

    /* hex-staggered spawn block left of the wall, NARROW and aligned with the
     * direct gap (y 28..32): with no fear the whole block funnels straight
     * through the direct gap, so fear must reroute it wholesale to the detour. */
    static float last_x[MAX_AGENTS];
    const float SPACING = 0.74f;
    for (int row = 0; ; row++) {
        float y = 28.0f + row * SPACING * 0.866f;
        if (y > 32.0f) break;
        float xoff = (row & 1) ? SPACING * 0.5f : 0.0f;
        for (int col = 0; ; col++) {
            float x = 8.0f + xoff + col * SPACING;
            if (x > 28.0f) break;
            int i = simp_spawn(s, x, y);
            if (i >= 0) { last_x[simp_slot_of(s, i)] = x; st.spawned++; }
        }
    }

    const float *px = simp_px(s), *py = simp_py(s);
    const int target = (int)(0.9f * (float)st.spawned);
    int step = 0;
    for (; step < MAX_STEPS; step++) {
        if (blood) paint_blood(s);
        st.drained += simp_step(s, DT);
        for (int i = 0; i < simp_count(s); i++) {
            int slot = simp_slot_of(s, i);
            if (last_x[slot] < WALL_X && px[i] >= WALL_X) {
                if (py[i] < DETOUR_Y) st.cross_detour++; else st.cross_direct++;
            }
            last_x[slot] = px[i];
            if (!isfinite(px[i]) || !isfinite(py[i])) st.bad++;
        }
        if (st.drained >= target) break;
    }
    st.steps = step;
    simp_destroy(s);
    return st;
}

/* Part 4: WALL-SCALE capability (anti-killbox). A wall with an OPEN slit (the
 * killbox lane, painted bloody) and a cheap BARRICADE band as the only solid
 * alternative (low wall_cost; never collapses here). With soft/zero danger the
 * horde pours through the open slit and drains. With the wall-scale default the
 * bloody slit OUT-COSTS the barricade, so the flow abandons the slit and the
 * horde presses the barricade instead (drain collapses, barricade pressure
 * jumps). Mirrors test_corpse_pile's pivot, but driven by blood. */
#define STEPS_BAR 5000
typedef struct { int spawned, drain; double barP; int bad; } BarStats;
static BarStats run_barricade(float kd) {
    BarStats st = { 0, 0, 0.0, 0 };
    SimP *s = simp_create(GW, GH, CELL, MAX_AGENTS);
    simp_params(s)->k_danger = kd;
    simp_params(s)->k_density = 0.0f; simp_params(s)->k_jam = 0.0f;
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
    float base = simp_wall_base_cost();
    /* wall at x=40 m: OPEN slit y 28..32 m (cells 56..64); BARRICADE band y 20..24 m
     * (cells 40..48) at a low breakthrough toll; the rest solid at the full toll. */
    for (int y = 1; y < GH - 1; y++) {
        if (y >= 56 && y < 64) continue;                 /* open slit */
        simp_set_wall(s, 80, y, true);
        simp_set_wall_cost(s, 80, y, (y >= 40 && y < 48) ? 0.05f * base : base);
    }
    for (int y = 54; y < 66; y++) simp_set_goal(s, 150, y, true);
    simp_terrain_commit(s);
    const float SP = 0.74f;                              /* spawn aligned with the slit */
    for (int row = 0; ; row++) {
        float y = 28.0f + row * SP * 0.866f; if (y > 32.0f) break;
        float xo = (row & 1) ? SP * 0.5f : 0.0f;
        for (int col = 0; ; col++) {
            float x = 8.0f + xo + col * SP; if (x > 28.0f) break;
            if (simp_spawn(s, x, y) >= 0) st.spawned++;
        }
    }
    const float *px = simp_px(s), *py = simp_py(s);
    const float *wp = simp_wall_pressure(s); const int *wc = simp_wall_cell(s);
    for (int step = 0; step < STEPS_BAR; step++) {
        paint_blood(s);
        st.drain += simp_step(s, DT);
        int n = simp_count(s);
        for (int i = 0; i < n; i++) {
            if (wp[i] > 0.0f) { int cell = wc[i]; int cy = cell / GW, cx = cell % GW;
                if (cx == 80 && cy >= 40 && cy < 48) st.barP += wp[i]; }
            if (!isfinite(px[i]) || !isfinite(py[i])) st.bad++;
        }
    }
    simp_destroy(s);
    return st;
}

/* Part 2: paint one burst, then decay with NO top-up; the field max must halve
 * after exactly danger_hl seconds. */
static int decay_ok(void) {
    SimP *s = simp_create(GW, GH, CELL, 16);
    float hl = simp_params(s)->danger_hl;
    simp_terrain_commit(s);
    simp_add_danger(s, 20.0f, 20.0f, 0.4f, 1.0f);
    simp_step(s, DT);                                  /* one step: field live */
    const float *dg = simp_danger_arr(s);
    int n = simp_grid_w(s) * simp_grid_h(s);
    float d0 = 0.0f; for (int i = 0; i < n; i++) if (dg[i] > d0) d0 = dg[i];
    int hl_steps = (int)(hl / DT);
    for (int k = 0; k < hl_steps; k++) simp_step(s, DT);
    float d1 = 0.0f; for (int i = 0; i < n; i++) if (dg[i] > d1) d1 = dg[i];
    float ratio = d1 / d0;
    int ok = (d0 > 0.5f) && (ratio > 0.45f && ratio < 0.55f);
    printf("decay: d0=%.4f d1=%.4f ratio=%.3f (want ~0.5 over hl=%.0fs) ok=%d\n",
           d0, d1, ratio, hl, ok);
    simp_destroy(s);
    return ok;
}

int main(void) {
    RunStats a = run(0.0f, 1);   /* fear off: keeps the direct gap          */
    RunStats b = run(6.0f, 1);   /* fear on : reroutes through the detour    */
    RunStats c = run(6.0f, 1);   /* determinism: identical to b              */

    printf("fear off: spawned %d | drained %d (%.0f%%) in %d steps | "
           "crossings direct %ld detour %ld\n",
           a.spawned, a.drained, 100.0 * a.drained / a.spawned, a.steps,
           a.cross_direct, a.cross_detour);
    printf("fear on : spawned %d | drained %d (%.0f%%) in %d steps | "
           "crossings direct %ld detour %ld\n",
           b.spawned, b.drained, 100.0 * b.drained / b.spawned, b.steps,
           b.cross_direct, b.cross_detour);

    /* fear off -> the direct gap dominates; fear on -> the detour dominates.
     * Both must drain well (fear reroutes, it does not seal). */
    int ok_baseline = a.cross_direct > 2 * a.cross_detour && a.drained >= (int)(0.85f * a.spawned);
    int ok_reroute  = b.cross_detour > 2 * b.cross_direct && b.drained >= (int)(0.85f * b.spawned);
    int ok_decay    = decay_ok();
    int ok_det      = (b.drained == c.drained) && (b.cross_direct == c.cross_direct)
                   && (b.cross_detour == c.cross_detour) && (b.steps == c.steps);

    /* Part 4: wall-scale anti-killbox. Soft/zero vs the wall-scale default. */
    BarStats d = run_barricade(0.0f);     /* no fear: pours through the open slit */
    BarStats e = run_barricade(400.0f);   /* wall-scale: abandons it, presses barricade */
    printf("killbox off (kd=0)  : spawned %d | drained %d | barricade pressure %.2f\n",
           d.spawned, d.drain, d.barP);
    printf("killbox on  (kd=400): spawned %d | drained %d | barricade pressure %.2f\n",
           e.spawned, e.drain, e.barP);
    /* wall-scale danger makes the bloody open slit out-cost the barricade: the
     * horde now PRESSES the barricade (pressure jumps from ~0) and partly stops
     * draining via the slit. Drain only drops PARTLY — the slit stays open and the
     * PBD still shoves a fraction through (fear deters, mass overrides). The
     * decisive signal is the barricade pressure jump. */
    int ok_killbox = (e.barP > 10.0 * d.barP) && (e.barP > 5.0) && (e.drain < d.drain);

    int ok_nan      = (a.bad == 0) && (b.bad == 0) && (c.bad == 0) && (d.bad == 0) && (e.bad == 0);

    printf("baseline-direct ok=%d reroute-detour ok=%d decay ok=%d killbox ok=%d det ok=%d nan ok=%d\n",
           ok_baseline, ok_reroute, ok_decay, ok_killbox, ok_det, ok_nan);
    int ok = ok_baseline && ok_reroute && ok_decay && ok_killbox && ok_det && ok_nan;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

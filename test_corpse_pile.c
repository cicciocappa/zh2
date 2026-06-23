/* test_corpse_pile.c — CORPSE_DESIGN.md §7-bis: corpse pile -> nav cost
 * (the anti-funnel pivot).
 *
 * Corpses already weigh on rho/jam (CORPSE_RHO), but that term saturates LOW
 * and stays far below WALL_ENTER: it can reroute the horde between OPEN roads,
 * never make it prefer crossing a BARRICADE. The new k_corpse term lets a DENSE
 * pile (corpse_mass accumulated as volume, attenuated by packing) climb toward a
 * "wall of corpses" cost — capped by k_corpse, saturating at wall_h — so a
 * clogged open kill-zone can become MORE expensive than a barricade's per-cell
 * breakthrough toll (simp_set_wall_cost). The Dijkstra then routes the horde to
 * press/break the barricade instead of queueing at the corpse-clogged gap. The
 * actual breakthrough+drain is game-side (defense.c siege); the CORE behaviour
 * verified here is the redistribution of PRESSURE.
 *
 * Corpse mass stays INFINITE in this slice: test_corpses is untouched, and the
 * packing term is wired but dormant (agents can't crest a sealed pile to
 * trample it — that arrives with the finite-mass slice). Non-permanence here
 * comes from DECAY, not packing.
 *
 * Parts:
 *  1. PIVOT. A wall with a direct OPEN gap (clogged by a corpse pile) and a
 *     BARRICADE band (solid, low wall_cost) as the only alternative. Three runs
 *     share identical physics, vary only the nav term:
 *       - k_corpse=0, k_jam=0  : baseline, horde queues at the clogged gap,
 *                                barricade untouched (pressure ~0).
 *       - k_jam=8, k_corpse=0  : jam alone CANNOT beat the barricade (it tops
 *                                out at k_jam << wall_cost) — still queues.
 *       - k_corpse=300         : the pile cost tops the barricade -> the horde
 *                                reroutes and presses the barricade (pressure>>).
 *  2. DECAY -> non-permanence. Pile, then no fresh kills: corpse_height decays
 *     monotonically to ~0 (the pile rots, no permanent defence).
 *  3. CLEAR (fire/acid). simp_corpse_clear removes the pile (physical corpses +
 *     fields) and reopens the gap: drain ~0 while clogged, >0 after the clear.
 *  4. Determinism (two identical pivot runs: identical pressure) + zero NaN.
 */
#define _POSIX_C_SOURCE 199309L
#include "sim_particles.h"
#include <stdio.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)
#define MAX_AGENTS 1600

/* ----- part 1: the pivot ------------------------------------------------- */

#define WALL_CX 80                 /* wall line at x = 40 m                   */
#define BARR_Y0 18                 /* barricade band cells [18, 22]           */
#define BARR_Y1 22
#define BARR_COST 150.0f           /* barricade breakthrough toll             */
#define GAP_Y0 58                  /* direct open gap, cells [58, 62]         */
#define GAP_Y1 62

typedef struct {
    double barr_pressure;          /* summed wall_pressure on the barricade   */
    int    drained, steps, bad;
    float  max_height;
} Pivot;

static void build_pivot_world(SimP *s) {
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
    /* wall line solid except the direct gap; the barricade band stays solid
     * too but carries a LOW per-cell toll, so it is the cheapest way through
     * once the gap is priced out by the pile. */
    for (int y = 1; y < GH - 1; y++) {
        if (y >= GAP_Y0 && y <= GAP_Y1) continue;
        simp_set_wall(s, WALL_CX, y, true);
    }
    for (int y = BARR_Y0; y <= BARR_Y1; y++)
        simp_set_wall_cost(s, WALL_CX, y, BARR_COST);
    for (int y = 12; y < 70; y++) simp_set_goal(s, 150, y, true);  /* both routes reach it */
    simp_terrain_commit(s);
}

/* fill the direct gap with a wall of corpses (the turret kill-zone). Infinite
 * mass: physically a near-seal, like test_corpses; each adds pi*r^2 of pile
 * volume to its nav cell -> the height climbs to wall_h and saturates. */
static void pile_gap(SimP *s) {
    for (int cy = GAP_Y0; cy <= GAP_Y1; cy++) {
        float y = (cy + 0.5f) * CELL;
        for (int k = 0; k < 3; k++)
            simp_corpse_add(s, 40.0f + 0.18f * (float)k, y, 0.32f, 1e9f);
    }
}

static Pivot run_pivot(float k_corpse, float k_jam) {
    Pivot st = { 0.0, 0, 0, 0, 0.0f };
    SimP *s = simp_create(GW, GH, CELL, MAX_AGENTS);
    simp_params(s)->k_density = 0.0f;     /* isolate: only the term under test */
    simp_params(s)->k_jam     = k_jam;
    simp_params(s)->k_corpse  = k_corpse;
    simp_params(s)->corpse_weight = 0.0f; /* idealized seal: isolate the NAV term
                                             from the §3 finite-mass shove */
    build_pivot_world(s);
    pile_gap(s);

    /* spawn block left of the wall, between gap (y~30 m) and barricade (y~10 m)
     * so neither route is favoured by distance — the toll decides. */
    int spawned = 0;
    const float SP = 0.74f;
    for (int row = 0; ; row++) {
        float y = 14.0f + row * SP * 0.866f;
        if (y > 26.0f) break;
        float xoff = (row & 1) ? SP * 0.5f : 0.0f;
        for (int col = 0; ; col++) {
            float x = 8.0f + xoff + col * SP;
            if (x > 30.0f) break;
            if (simp_spawn(s, x, y) >= 0) spawned++;
        }
    }

    const float *px = simp_px(s), *py = simp_py(s);
    const float *wp = simp_wall_pressure(s);
    const int   *wc = simp_wall_cell(s);
    const float *ch = simp_corpse_height(s);

    int step = 0;
    for (; step < 3600; step++) {              /* 60 s */
        st.drained += simp_step(s, DT);
        if ((step % 60) == 0) pile_gap(s);     /* turret keeps feeding the pile */
        for (int i = 0; i < simp_count(s); i++) {
            int c = wc[i];
            if (c >= 0) { int cy = c / GW, cx = c % GW;
                if (cx == WALL_CX && cy >= BARR_Y0 && cy <= BARR_Y1)
                    st.barr_pressure += wp[i]; }
            if (!isfinite(px[i]) || !isfinite(py[i])) st.bad++;
        }
        float h = ch[GAP_Y0 * GW + WALL_CX];
        if (h > st.max_height) st.max_height = h;
    }
    st.steps = step;
    simp_destroy(s);
    return st;
}

/* ----- part 2: decay -> non-permanence ----------------------------------- */

static int test_decay(void) {
    SimP *s = simp_create(GW, GH, CELL, 16);
    simp_params(s)->corpse_mass_hl = 4.0f;     /* short half-life to keep it brisk */
    simp_params(s)->corpse_weight = 0.0f;      /* no crowd here; keep it idealized */
    const float *ch = simp_corpse_height(s);
    int cell = 50 * GW + 50;
    for (int k = 0; k < 8; k++) simp_corpse_add(s, 25.0f, 25.0f, 0.4f, 1e9f);

    float prev = 1e30f; int mono = 1; float h0 = 0.0f, hN = 0.0f;
    for (int step = 0; step < 1200; step++) {  /* 20 s = 5 half-lives */
        simp_step(s, DT);
        float h = ch[cell];
        if (step == 0) h0 = h;
        if (h > prev + 1e-4f) mono = 0;        /* must never grow */
        prev = h; hN = h;
    }
    simp_destroy(s);
    int ok = mono && h0 > 1.0f && hN < 0.05f * h0;
    printf("part2 decay: h0=%.3f hN=%.4f monotone=%d -> %s\n",
           h0, hN, mono, ok ? "ok" : "FAIL");
    return ok;
}

/* ----- part 3: simp_corpse_clear reopens the gap ------------------------- */

static int test_clear(void) {
    SimP *s = simp_create(GW, GH, CELL, MAX_AGENTS);
    simp_params(s)->k_density = 0.0f;
    simp_params(s)->k_jam     = 0.0f;
    simp_params(s)->k_corpse  = 300.0f;
    simp_params(s)->corpse_weight = 0.0f;  /* idealized seal: the clog must hold
                                              until simp_corpse_clear removes it */
    /* single corridor: spawn left, one open gap at the wall, goal right */
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
    for (int y = 1; y < GH - 1; y++)
        if (!(y >= GAP_Y0 && y <= GAP_Y1)) simp_set_wall(s, WALL_CX, y, true);
    for (int y = GAP_Y0 - 2; y <= GAP_Y1 + 2; y++) simp_set_goal(s, 150, y, true);
    simp_terrain_commit(s);
    pile_gap(s);

    const float SP = 0.74f;
    for (int row = 0; ; row++) {
        float y = 26.0f + row * SP * 0.866f;
        if (y > 34.0f) break;
        for (int col = 0; ; col++) {
            float x = 8.0f + col * SP;
            if (x > 30.0f) break;
            simp_spawn(s, x, y);
        }
    }

    const float *ch = simp_corpse_height(s);
    const float *px = simp_px(s), *py = simp_py(s);
    int bad = 0;
    int drained_before = 0;
    for (int step = 0; step < 1800; step++) {  /* 30 s clogged */
        drained_before += simp_step(s, DT);
        if ((step % 60) == 0) pile_gap(s);
        for (int i = 0; i < simp_count(s); i++)
            if (!isfinite(px[i]) || !isfinite(py[i])) bad++;
    }
    float h_clog = ch[GAP_Y0 * GW + WALL_CX];
    int corpses_clog = simp_corpse_count(s);

    /* fire/acid sweep over the gap: removes corpses + zeroes the fields */
    for (int cy = GAP_Y0; cy <= GAP_Y1; cy++)
        simp_corpse_clear(s, 40.0f, (cy + 0.5f) * CELL, 1.0f);
    float h_clear = ch[GAP_Y0 * GW + WALL_CX];
    int corpses_clear = simp_corpse_count(s);

    int drained_after = 0;
    for (int step = 0; step < 1800; step++) { /* 30 s, gap reopened */
        drained_after += simp_step(s, DT);
        for (int i = 0; i < simp_count(s); i++)
            if (!isfinite(px[i]) || !isfinite(py[i])) bad++;
    }
    simp_destroy(s);

    int ok = (bad == 0) && (h_clog > 1.5f) && (h_clear < 1e-4f) &&
             (corpses_clear < corpses_clog) &&
             (drained_before < 30) && (drained_after > 5 * (drained_before + 1));
    printf("part3 clear: clogged h=%.3f corpses=%d drain=%d | after clear h=%.4f "
           "corpses=%d drain=%d (nan=%d) -> %s\n",
           h_clog, corpses_clog, drained_before, h_clear, corpses_clear,
           drained_after, bad, ok ? "ok" : "FAIL");
    return ok;
}

/* ----- part 5: dismembered splat == half a corpse (nav cost only) -------- */

static int test_splat_ratio(void) {
    SimP *s = simp_create(GW, GH, CELL, 16);
    const float *ch = simp_corpse_height(s);
    const float R = 0.36f;
    float xc = (40 + 0.5f) * CELL, yc = (40 + 0.5f) * CELL;  /* cell (40,40) */
    float xs = (80 + 0.5f) * CELL, ys = (40 + 0.5f) * CELL;  /* cell (80,40) */
    for (int k = 0; k < 4; k++) simp_corpse_add(s, xc, yc, R, 1e9f);   /* 4 corpses */
    for (int k = 0; k < 8; k++) simp_corpse_splat(s, xs, ys, R, 0.5f); /* 8 dismembered */
    simp_step(s, DT);
    float h_c = ch[40 * GW + 40], h_s = ch[40 * GW + 80];
    int corpses = simp_corpse_count(s);     /* splats add NO physical corpse */
    simp_destroy(s);
    /* both deposit 4*pi*R^2 of pile volume -> equal height; splats leave 0 discs */
    int ok = h_c > 0.1f && fabsf(h_s - h_c) < 0.02f * h_c && corpses == 4;
    printf("part5 splat ratio: 4 corpses h=%.4f | 8 splats h=%.4f (phys corpses=%d) -> %s\n",
           h_c, h_s, corpses, ok ? "ok" : "FAIL");
    return ok;
}

int main(void) {
    Pivot a = run_pivot(0.0f, 0.0f);   /* baseline: queue at gap, no barricade */
    Pivot j = run_pivot(0.0f, 8.0f);   /* jam alone: still can't beat barricade */
    Pivot c = run_pivot(300.0f, 0.0f); /* corpse term: reroute, press barricade */

    printf("part1 pivot (barricade pressure summed over run):\n");
    printf("  k=off jam=off : barr_pressure=%.2f max_pile_h=%.2f nan=%d\n",
           a.barr_pressure, a.max_height, a.bad);
    printf("  k=off jam=on  : barr_pressure=%.2f nan=%d\n", j.barr_pressure, j.bad);
    printf("  k=300 jam=off : barr_pressure=%.2f max_pile_h=%.2f nan=%d\n",
           c.barr_pressure, c.max_height, c.bad);

    int ok_pile   = c.max_height > 1.9f;                            /* saturated */
    int ok_pivot  = (c.barr_pressure > 5.0) &&
                    (c.barr_pressure > 5.0 * (a.barr_pressure + 0.2)) &&
                    (c.barr_pressure > 5.0 * (j.barr_pressure + 0.2));
    int nan_ok    = (a.bad == 0) && (j.bad == 0) && (c.bad == 0);

    int ok_decay  = test_decay();
    int ok_clear  = test_clear();
    int ok_splat  = test_splat_ratio();

    Pivot c2 = run_pivot(300.0f, 0.0f);
    int ok_det = (c2.barr_pressure == c.barr_pressure) && (c2.drained == c.drained);
    printf("part4 determinism: pressure %.4f==%.4f drained %d==%d -> %s\n",
           c.barr_pressure, c2.barr_pressure, c.drained, c2.drained,
           ok_det ? "ok" : "FAIL");

    int ok = ok_pile && ok_pivot && nan_ok && ok_decay && ok_clear && ok_splat && ok_det;
    printf("pile reached wall=%d  pivot=%d (jam<<corpse)  nan=%d\n",
           ok_pile, ok_pivot, nan_ok);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

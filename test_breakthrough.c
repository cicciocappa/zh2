/* test_breakthrough.c — per-cell wall breakthrough cost (SIMULAZIONE.md).
 *
 * Today every wall costs the same to enter (WALL_ENTER), so the Dijkstra can't
 * tell a building from a barricade: a sealed goal makes the horde press the
 * wall cell geometrically NEAREST on the path. simp_set_wall_cost promotes that
 * toll to a per-cell field: a high tier (palazzo/rock) routes the horde AWAY,
 * a lower tier (player barricade) becomes the preferred siege target — even on
 * a detour. Collision is independent: every wall cell here is solid, the cost
 * decides only WHERE the crowd leans.
 *
 * Scene: spawn block on the left, goal on the right, a FULLY sealed wall column
 * between them. Spawn and goal are centred (y = 30 m), so the nearest wall cell
 * is the MIDDLE band. A second "barricade" band sits 15 m off-centre (a detour).
 *
 *   - uniform: every wall cell = palazzo tier. The horde still sieges (the goal
 *     is sealed) but spreads along the whole wall: almost NO pressure lands on
 *     the off-centre band specifically;
 *   - hierarchy: only the off-centre band is dropped to the cheaper barricade
 *     tier -> the horde detours and concentrates its siege THERE.
 *
 * Asserted by the siege sensor (simp_wall_pressure / simp_wall_cell), causally:
 * the off-band carries a dominant share of the siege ONLY with the cheaper tier
 * (>> than uniform, where the same band gets ~nothing), while the uniform run
 * proves the crowd is genuinely pressing (just elsewhere). Plus determinism /
 * no NaN. The "spread vs concentrate" contrast is the per-cell cost at work.
 */
#include "sim_particles.h"
#include <stdio.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)
#define MAX_AGENTS 1400
#define STEPS 1500                  /* 25 s: reach the wall and lean on it */
#define WALL_CX 80                  /* sealed wall column (40 m)            */
#define MID_CY 60                   /* spawn/goal height (30 m), nearest band */
#define OFF_CY 90                   /* barricade band (45 m), a 15 m detour   */
#define BAND 4                      /* +/- cells counted as "in the band"     */

typedef struct { double mid, off, total; int pressing; int bad; } RunStats;

/* hierarchy = true: off-centre band gets the cheaper barricade tier. */
static RunStats run(int hierarchy) {
    RunStats st = { 0.0, 0.0, 0.0, 0, 0 };
    SimP *s = simp_create(GW, GH, CELL, MAX_AGENTS);
    const float PALAZZO   = 10.0f * simp_wall_base_cost();
    const float BARRICADE =        simp_wall_base_cost();

    /* border + a fully sealed wall column: the goal is reachable only by
     * breaking through, so the horde must lean on the wall somewhere. */
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
    for (int y = 1; y < GH - 1; y++) {
        simp_set_wall(s, WALL_CX, y, true);
        /* whole wall is palazzo; only the off-centre band may drop to barricade */
        int barricade = hierarchy && (y >= OFF_CY - BAND && y <= OFF_CY + BAND);
        simp_set_wall_cost(s, WALL_CX, y, barricade ? BARRICADE : PALAZZO);
    }
    for (int y = MID_CY - 6; y < MID_CY + 6; y++) simp_set_goal(s, 150, y, true);
    simp_terrain_commit(s);

    /* hex-staggered crowd left of the wall, centred on the middle band */
    const float SPACING = 0.74f;
    int spawned = 0;
    for (int row = 0; ; row++) {
        float y = 24.0f + row * SPACING * 0.866f;
        if (y > 36.0f) break;
        float xoff = (row & 1) ? SPACING * 0.5f : 0.0f;
        for (int col = 0; ; col++) {
            float x = 8.0f + xoff + col * SPACING;
            if (x > 30.0f) break;
            if (simp_spawn(s, x, y) >= 0) spawned++;
        }
    }
    (void)spawned;

    const int gw = simp_grid_w(s);
    for (int step = 0; step < STEPS; step++) {
        simp_step(s, DT);
        if (step < STEPS / 2) continue;          /* let the flow settle first */
        const float *wp = simp_wall_pressure(s);
        const int   *wc = simp_wall_cell(s);
        const float *px = simp_px(s), *py = simp_py(s);
        for (int i = 0; i < simp_count(s); i++) {
            if (!isfinite(px[i]) || !isfinite(py[i])) st.bad++;
            if (wc[i] < 0 || wp[i] <= 0.0f) continue;
            st.total += wp[i]; st.pressing++;
            int cy = wc[i] / gw;
            if (cy >= MID_CY - BAND && cy <= MID_CY + BAND) st.mid += wp[i];
            else if (cy >= OFF_CY - BAND && cy <= OFF_CY + BAND) st.off += wp[i];
        }
    }
    simp_destroy(s);
    return st;
}

int main(void) {
    RunStats u  = run(0);
    RunStats h  = run(1);
    RunStats h2 = run(1);            /* determinism: same inputs, same result */

    printf("uniform   : siege total=%.2f, on off-band=%.2f (%.0f%%) -> spread\n",
           u.total, u.off, 100.0 * u.off / (u.total + 1e-9));
    printf("hierarchy : siege total=%.2f, on off-band=%.2f (%.0f%%) -> barricade\n",
           h.total, h.off, 100.0 * h.off / (h.total + 1e-9));

    int ok_siege     = u.total > 10.0;                  /* uniform really sieges */
    int ok_spread    = u.off < 0.10 * u.total;          /* but not at the band   */
    int ok_concentr  = h.off > h.mid && h.off > 0.30 * h.total;   /* hierarchy: band dominates */
    int ok_causal    = h.off > 20.0 * (u.off + 1e-3);   /* the cheap tier caused it */
    int ok_det       = (h.mid == h2.mid) && (h.off == h2.off);
    int ok_nan       = (u.bad == 0) && (h.bad == 0);
    int ok = ok_siege && ok_spread && ok_concentr && ok_causal && ok_det && ok_nan;

    printf("uniform-sieges=%d spread=%d band-dominates=%d causal=%d determinism=%d nan=%d\n",
           ok_siege, ok_spread, ok_concentr, ok_causal, ok_det, ok_nan);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

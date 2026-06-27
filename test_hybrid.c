/* test_hybrid.c — Hybrid barricade: destructible structure -> draggable debris.
 *
 * A barricade is a destructible structure (HP + SIEGE sensor + nav wall, §7)
 * while intact; flagged Hybrid (def_struct_set_debris), its COLLAPSE drops a
 * draggable rubble disc per freed cell (DRAG_DESIGN.md) instead of vanishing
 * cleanly. The nav reroutes (cells freed) but the breach stays physically
 * cluttered — the horde shoves the debris out of the way.
 *
 * Corridor sealed by a wall structure across the only lane; a horde presses it
 * (siege) to reach the goal beyond. Two runs:
 *   - clean   (no debris flag): on collapse the cells just free, no draggables;
 *   - hybrid  (debris flagged):  on collapse N rubble discs appear and the horde
 *                                breaks through (drain > 0).
 * Plus determinism + no NaN.
 */
#include "defense.h"
#include <stdio.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)
#define LANE_Y0 52
#define LANE_Y1 68
#define BARR_CX 80
#define STEPS 4500

typedef struct { int collapsed, drag_after, drained, bad; } Run;

static Run run(int debris) {
    Run r = {0,0,0,0};
    SimP *s = simp_create(GW, GH, CELL, 6000);
    /* corridor: solid outside the lane (+ borders) */
    for (int cy = 0; cy < GH; cy++)
        for (int cx = 0; cx < GW; cx++)
            if (cy < LANE_Y0 || cy >= LANE_Y1 || cx == 0 || cx == GW - 1)
                simp_set_wall(s, cx, cy, true);
    for (int cy = LANE_Y0; cy < LANE_Y1; cy++) simp_set_goal(s, GW - 4, cy, true);
    simp_terrain_commit(s);

    DefGame *g = def_create(s, 6000);
    int id = def_add_structure(g, 220.0f, 0);
    for (int cy = LANE_Y0; cy < LANE_Y1; cy++) def_struct_cell(g, id, BARR_CX, cy);
    if (debris) def_struct_set_debris(g, id, 6.0f);
    simp_terrain_commit(s);

    /* horde filling the left of the lane, pressing toward the goal */
    for (float y = 26.6f; y < 33.4f; y += 0.7f)
        for (float x = 4.0f; x < 36.0f; x += 0.7f)
            def_spawn(g, x, y, BT_MAN);

    int collapsed_at = -1;
    for (int step = 0; step < STEPS; step++) {
        r.drained += simp_step(s, DT);
        def_update(g, DT);
        if (collapsed_at < 0 && def_struct_collapsed(g, id)) collapsed_at = step;
        const float *px = simp_px(s), *py = simp_py(s);
        for (int i = 0; i < simp_count(s); i++)
            if (!isfinite(px[i]) || !isfinite(py[i])) r.bad++;
        const float *dx = simp_drag_px(s), *dy = simp_drag_py(s);
        for (int j = 0; j < simp_drag_count(s); j++)
            if (!isfinite(dx[j]) || !isfinite(dy[j])) r.bad++;
    }
    r.collapsed = def_struct_collapsed(g, id);
    r.drag_after = simp_drag_count(s);
    def_destroy(g); simp_destroy(s);
    return r;
}

int main(void) {
    Run clean = run(0);
    Run hyb   = run(1);
    Run hyb2  = run(1);   /* determinism */

    printf("clean : collapsed=%d debris=%d drained=%d\n", clean.collapsed, clean.drag_after, clean.drained);
    printf("hybrid: collapsed=%d debris=%d drained=%d\n", hyb.collapsed, hyb.drag_after, hyb.drained);

    int ok_collapse = clean.collapsed && hyb.collapsed;         /* both fall      */
    int ok_clean    = clean.drag_after == 0;                    /* no debris flag */
    int ok_debris   = hyb.drag_after > 0;                       /* rubble appeared */
    int ok_breach   = hyb.drained > 50;                         /* horde gets through */
    int ok_det      = (hyb.drag_after == hyb2.drag_after) && (hyb.drained == hyb2.drained);
    int ok_nan      = (clean.bad == 0) && (hyb.bad == 0);
    int ok = ok_collapse && ok_clean && ok_debris && ok_breach && ok_det && ok_nan;

    printf("collapse=%d clean-no-debris=%d debris-appears=%d breach-drains=%d determinism=%d nan=%d\n",
           ok_collapse, ok_clean, ok_debris, ok_breach, ok_det, ok_nan);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

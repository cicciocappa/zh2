/* test_turret_siege.c — destructible turrets (defense.h).
 *
 * A bare turret is a point the horde ignores. def_turret_make_destructible binds
 * it to a 1-cell structure: the cell becomes a solid wall the horde PRESSES to
 * reach the goal beyond, so the existing SIEGE sensor + per-slot attack timer
 * chip it for free. At HP 0 it collapses and def_update stops firing it.
 *
 * Scene: a full-width wall seals a north goal; its centre cell is a DESTRUCTIBLE
 * turret (the rest are plain indestructible walls = the control). A crowd penned
 * to the south presses the whole line. Asserted:
 *   - the turret FIRES while alive (shots accrue),
 *   - its structure HP falls to 0 -> collapsed -> def_turret_disabled,
 *   - once disabled it never fires again (it is the only turret; shots freeze),
 *   - the indestructible control run never disables and keeps firing,
 *   - determinism (two runs identical) and zero NaN.
 *
 * Turret damage is pinned to 0 so the firing turret doesn't mow its own
 * besiegers (attrition would confound the siege) — we measure that it FIRES
 * (shots/fired), not that it kills.
 */
#include "defense.h"
#include "sim_particles.h"
#include <stdio.h>
#include <math.h>
#include <stdbool.h>

#define GW 120
#define GH 90
#define CELL 0.5f
#define DT (1.0f / 60.0f)
#define MAX_AGENTS 2000
#define MAX_STEPS 4000
#define WR 30                 /* seal row (cy) */
#define TURRET_HP 200.0f

/* place a packed block of agents just south of the seal, pressing north */
static void fill_crowd(DefGame *g) {
    float south_face = (WR + 1) * CELL;
    const float SP = 0.62f;
    for (int row = 0; ; row++) {
        float y = south_face + 0.9f + row * SP * 0.866f;
        if (y > south_face + 12.0f) break;
        float xoff = (row & 1) ? SP * 0.5f : 0.0f;
        for (float x = 6.0f + xoff; x <= 54.0f; x += SP)
            def_spawn(g, x, y, BT_MAN);
    }
}

/* build the sealed-goal scene. The centre seal cell is the turret's cell; the
 * rest of the row are plain walls. If destructible, the turret is bound to its
 * 1-cell structure. Returns the turret id; tcx/tcy receive its cell. */
static int build_ex(SimP **ps, DefGame **pg, bool destructible, float dmg,
                    bool seal, int *tcx, int *tcy) {
    SimP *s = simp_create(GW, GH, CELL, MAX_AGENTS);
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }

    int cxc = GW / 2;                       /* turret cell column */
    if (seal) {
        /* seal the whole row EXCEPT the turret's cell (turret raises that one) */
        for (int x = 1; x < GW - 1; x++) if (x != cxc) simp_set_wall(s, x, WR, true);
        for (int y = 6; y < 10; y++) for (int x = 40; x < 80; x++) simp_set_goal(s, x, y, true);
    } else {
        /* OPEN FIELD: no wall, a FULL-WIDTH goal to the north. The turret is a
         * lone obstacle the flow routes AROUND (flanked, into_wall ~ 0) — only
         * contact, not flow, can damage it. */
        for (int y = 4; y < 8; y++) for (int x = 1; x < GW - 1; x++) simp_set_goal(s, x, y, true);
    }
    simp_terrain_commit(s);

    DefGame *g = def_create(s, MAX_AGENTS);

    DefTurret t = {0};
    t.x = (cxc + 0.5f) * CELL; t.y = (WR + 0.5f) * CELL;
    t.arc_min = -3.1416f; t.arc_max = 3.1416f; t.sweep_dir = 1; t.sweep_speed = 3.0f;
    t.range = 40.0f; t.heavy = 0; t.piercing = 0;
    t.fire_period = 0.10f; t.damage = dmg;
    int tid = def_add_turret(g, &t);

    if (destructible) {
        def_turret_make_destructible(g, tid, TURRET_HP);
        simp_terrain_commit(s);               /* commit the turret's raised wall */
    } else {
        simp_set_wall(s, cxc, WR, true);      /* control: plain indestructible seal */
        simp_terrain_commit(s);
    }
    *tcx = cxc; *tcy = WR;
    *ps = s; *pg = g;
    return tid;
}
static int build(SimP **ps, DefGame **pg, bool destructible, float dmg, int *tcx, int *tcy) {
    return build_ex(ps, pg, destructible, dmg, true, tcx, tcy);
}

static int any_nan(SimP *s) {
    const float *px = simp_px(s), *py = simp_py(s);
    int n = simp_count(s);
    for (int i = 0; i < n; i++) if (isnan(px[i]) || isnan(py[i])) return 1;
    return 0;
}

/* run the destructible scene; report when the turret disabled and shot counts.
 * out_csum = position checksum for the determinism check. */
static int run_destructible(int *out_disable_step, int *out_shots_at_disable,
                            int *out_shots_end, int *out_fired_after, double *out_csum) {
    SimP *s; DefGame *g; int tcx, tcy;
    int tid = build(&s, &g, true, 0.0f, &tcx, &tcy);   /* dmg 0: no attrition */
    fill_crowd(g);
    int sid = def_cell_struct(g, tcx, tcy);
    int disable_step = -1, shots_at_disable = -1, fired_after = 0, nan = 0;
    for (int step = 0; step < MAX_STEPS; step++) {
        simp_step(s, DT);
        def_update(g, DT);
        if (any_nan(s)) { nan = 1; break; }
        int dis = def_turret_disabled(g, tid);
        if (dis && disable_step < 0) { disable_step = step; shots_at_disable = def_shots(g); }
        if (disable_step >= 0 && def_turret(g, tid)->fired) fired_after = 1;
    }
    (void)sid;
    const float *px = simp_px(s), *py = simp_py(s);
    double csum = 0; int n = simp_count(s);
    for (int i = 0; i < n; i++) csum += (double)px[i] * 1.0009 + (double)py[i] * 1.0003;
    *out_disable_step = disable_step;
    *out_shots_at_disable = shots_at_disable;
    *out_shots_end = def_shots(g);
    *out_fired_after = fired_after;
    *out_csum = csum;
    def_destroy(g); simp_destroy(s);
    return nan;
}

int main(void) {
    int fails = 0;

    /* ---- A0) REGRESSION: a turret on its own solid cell must still HIT the
     * horde. The muzzle sits in a wall -> wall_ray_t self-blocks unless the ray
     * starts beyond the emplacement (defense.c muzzle offset). dmg > 0, short
     * run, assert kills accrue. ---- */
    {
        SimP *s; DefGame *g; int tcx, tcy;
        build(&s, &g, true, 55.0f, &tcx, &tcy);
        fill_crowd(g);
        for (int step = 0; step < 600; step++) { simp_step(s, DT); def_update(g, DT); }
        int k = def_kills(g), sh = def_shots(g);
        printf("== A0) walled turret still kills ==\n");
        printf("  shots %d | kills %d\n", sh, k);
        if (sh <= 0) { printf("  FAIL: walled turret never fired\n"); fails++; }
        if (k <= 0)  { printf("  FAIL: walled turret fired but killed nobody (muzzle self-block)\n"); fails++; }
        if (sh > 0 && k > 0) printf("  OK: fires through its own emplacement and kills.\n");
        def_destroy(g); simp_destroy(s);
    }

    /* ---- A1) THE REPORTED BUG: a flanked turret in the OPEN must still be
     * attacked. No seal, a full-width goal to the north -> the flow routes AROUND
     * the lone turret cell (into_wall ~ 0), so the wall-siege never fires. Only
     * the CONTACT model (turret_contact_update) can damage it. A dense crowd
     * mobs past it; assert its HP falls and it collapses + goes silent. ---- */
    {
        SimP *s; DefGame *g; int tcx, tcy;
        int tid = build_ex(&s, &g, true, 0.0f, false, &tcx, &tcy);
        fill_crowd(g);
        int sid = def_cell_struct(g, tcx, tcy);
        float hp0 = def_struct_hp(g, sid);
        int collapse_step = -1;
        for (int step = 0; step < MAX_STEPS; step++) {
            simp_step(s, DT); def_update(g, DT);
            if (collapse_step < 0 && def_turret_disabled(g, tid)) collapse_step = step;
        }
        float hp1 = def_struct_hp(g, sid);
        printf("== A1) flanked turret in the open ==\n");
        printf("  HP %.0f -> %.0f | collapsed: %s (step %d) | disabled: %s\n",
               hp0, hp1, collapse_step >= 0 ? "YES" : "no", collapse_step,
               def_turret_disabled(g, tid) ? "YES" : "no");
        if (hp1 >= hp0) { printf("  FAIL: flanked turret took NO contact damage (the reported bug)\n"); fails++; }
        if (collapse_step < 0) { printf("  FAIL: swarmed turret never collapsed\n"); fails++; }
        if (hp1 < hp0 && collapse_step >= 0) printf("  OK: mobbed -> damaged -> collapsed -> silent.\n");
        def_destroy(g); simp_destroy(s);
    }

    /* ---- A) destructible turret: sieged to collapse, stops firing ---- */
    int dstep, shots_dis, shots_end, fired_after; double csum1;
    int nan = run_destructible(&dstep, &shots_dis, &shots_end, &fired_after, &csum1);

    printf("== A) destructible turret ==\n");
    printf("  disabled at step %d | shots at disable %d | shots end %d | fired after disable: %s | NaN: %s\n",
           dstep, shots_dis, shots_end, fired_after ? "YES" : "no", nan ? "YES" : "no");

    if (nan) { printf("  FAIL: NaN\n"); fails++; }
    if (shots_dis <= 0) { printf("  FAIL: turret never fired before collapse\n"); fails++; }
    if (dstep < 0) { printf("  FAIL: turret never disabled (siege didn't break it)\n"); fails++; }
    else if (dstep >= MAX_STEPS - 1) { printf("  FAIL: disabled only at the very end\n"); fails++; }
    if (fired_after) { printf("  FAIL: turret fired after being disabled\n"); fails++; }
    if (dstep >= 0 && shots_end != shots_dis) {
        printf("  FAIL: shots grew after disable (%d -> %d)\n", shots_dis, shots_end); fails++; }
    if (!nan && shots_dis > 0 && dstep > 0 && !fired_after && shots_end == shots_dis)
        printf("  OK: fired %d shots, collapsed, then silent.\n", shots_dis);

    /* ---- B) control: indestructible turret never disables, keeps firing ---- */
    SimP *s; DefGame *g; int tcx, tcy;
    int tid = build(&s, &g, false, 0.0f, &tcx, &tcy);
    fill_crowd(g);
    int ever_disabled = 0, shots_mid = 0;
    for (int step = 0; step < MAX_STEPS; step++) {
        simp_step(s, DT); def_update(g, DT);
        if (def_turret_disabled(g, tid)) ever_disabled = 1;
        if (step == MAX_STEPS / 2) shots_mid = def_shots(g);
    }
    int shots_b_end = def_shots(g);
    printf("== B) indestructible control ==\n");
    printf("  ever disabled: %s | shots mid %d -> end %d\n",
           ever_disabled ? "YES" : "no", shots_mid, shots_b_end);
    if (ever_disabled) { printf("  FAIL: indestructible turret got disabled\n"); fails++; }
    if (shots_b_end <= shots_mid) { printf("  FAIL: control turret stopped firing\n"); fails++; }
    if (!ever_disabled && shots_b_end > shots_mid) printf("  OK: never disabled, kept firing.\n");
    def_destroy(g); simp_destroy(s);

    /* ---- C) determinism ---- */
    int d2, sd2, se2, fa2; double csum2;
    run_destructible(&d2, &sd2, &se2, &fa2, &csum2);
    printf("== C) determinism ==\n");
    printf("  run1 csum %.6f | run2 csum %.6f | disable step %d vs %d\n",
           csum1, csum2, dstep, d2);
    if (csum1 != csum2 || dstep != d2 || sd2 != shots_dis) {
        printf("  FAIL: non-deterministic\n"); fails++; }
    else printf("  OK: identical.\n");

    printf(fails ? "\nFAILED (%d)\n" : "\nALL OK\n", fails);
    return fails ? 1 : 0;
}

/* test_base.c — §7 base & defeat verification (M5_DESIGN.md §7, defense.c).
 *
 * The base is the innermost siege-able structure (SIEGE_DESIGN.md, reused whole).
 * A central goal is sealed by a CORE ring; an OUTER destructible ring surrounds
 * it. The horde, pulled inward, sieges the outer ring (per-slot discrete attacks,
 * def_update) until it collapses and the nav reroutes; it then presses the core,
 * whose collapse is the loss (def_lost), NOT a reroute.
 *
 * Three runs:
 *  A) UNDEFENDED — no turrets: outer collapses (reroute), then the core falls and
 *     def_lost flips. The base loses on its own.
 *  B) DEFENDED   — turrets in the field mow the approach: the core never falls
 *     (def_lost == 0, kills high). The defensive loop is winnable.
 *  C) DETERMINISM — rerun A: same collapse/lost steps.
 * Plus: every structure cell pressed is solid, and zero NaN.
 */
#define _POSIX_C_SOURCE 199309L
#include "defense.h"
#include "sim_particles.h"
#include <stdio.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)
#define MAXA 4000
#define MAX_STEPS 12000

/* center of the base, in nav cells */
#define CX0 80
#define CY0 60

/* Square-ring perimeter of half-size h around the center, assigned to struct id. */
static void ring(DefGame *g, int id, int h) {
    for (int cy = CY0 - h; cy <= CY0 + h; cy++)
        for (int cx = CX0 - h; cx <= CX0 + h; cx++)
            if (cx == CX0 - h || cx == CX0 + h || cy == CY0 - h || cy == CY0 + h)
                def_struct_cell(g, id, cx, cy);
}

typedef struct {
    int defended;
    /* outputs */
    int outer_collapse_step, lost_step;
    long pressed_nonsolid;   /* must stay 0 */
    int  bad;                /* NaN count    */
    int  kills, spawned;
} Run;

static int spawn_horde(DefGame *g) {        /* dense block funneled at the west face */
    const float SP = 0.62f;
    int n = 0;
    for (int row = 0; ; row++) {
        float y = 20.0f + row * SP * 0.866f;
        if (y > 40.0f) break;                /* spans the ring's west face (y 26..34) */
        float xoff = (row & 1) ? SP * 0.5f : 0.0f;
        for (float x = 3.0f + xoff; x <= 22.0f; x += SP)
            if (def_spawn(g, x, y, BT_MAN) != SIMP_HANDLE_INVALID) n++;
    }
    return n;
}

static void add_turrets(DefGame *g) {       /* defended run: field of fire */
    for (int k = 0; k < 3; k++) {
        DefTurret t = {0};
        t.x = 18.0f; t.y = 22.0f + k * 8.0f;
        t.ang = 0.0f; t.arc_min = -1.0f; t.arc_max = 1.0f;  /* face east */
        t.sweep_speed = 2.5f; t.range = 45.0f;
        t.fire_period = 0.10f; t.damage = 50.0f; t.heavy = 0; t.piercing = 1;
        def_add_turret(g, &t);
    }
}

static Run run(int defended) {
    Run r = {0}; r.defended = defended;
    r.outer_collapse_step = -1; r.lost_step = -1;
    SimP *s = simp_create(GW, GH, CELL, MAXA);

    /* border walls */
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }

    DefGame *g = def_create(s, MAXA);
    int core  = def_add_structure(g, 200.0f, 1);   /* innermost = loss      */
    int outer = def_add_structure(g, 250.0f, 0);   /* reroutes on collapse  */
    ring(g, core, 4);
    ring(g, outer, 8);
    /* central goal sealed by the core ring */
    for (int cy = CY0 - 1; cy <= CY0 + 1; cy++)
        for (int cx = CX0 - 1; cx <= CX0 + 1; cx++) simp_set_goal(s, cx, cy, true);
    simp_terrain_commit(s);

    if (defended) add_turrets(g);
    r.spawned = spawn_horde(g);

    const float *px = simp_px(s), *py = simp_py(s);
    const float *wp = simp_wall_pressure(s);
    const int   *wc = simp_wall_cell(s);

    for (int step = 0; step < MAX_STEPS; step++) {
        simp_step(s, DT);

        /* validate the siege sensor against the CURRENT terrain, before
         * def_update may collapse a ring and free its cells (the sensor was
         * captured at the end of this step, when the ring was still solid). */
        int n = simp_count(s);
        for (int i = 0; i < n; i++) {
            if (!isfinite(px[i]) || !isfinite(py[i])) r.bad++;
            if (wp[i] > 0.0f) {
                int c = wc[i];
                if (c < 0 || !simp_is_wall(s, c % GW, c / GW)) r.pressed_nonsolid++;
            }
        }

        def_update(g, DT);

        if (r.outer_collapse_step < 0 && def_struct_collapsed(g, outer))
            r.outer_collapse_step = step;
        if (r.lost_step < 0 && def_lost(g)) { r.lost_step = step; break; }
        /* defended: stop once the field is cleared (base held) */
        if (defended && n == 0) break;
    }
    r.kills = def_kills(g);
    def_destroy(g); simp_destroy(s);
    return r;
}

static void report(const char *tag, Run r) {
    printf("%-12s spawned %d | outer-collapse@%d | lost@%d | kills %d | "
           "pressed-nonsolid %ld | nan %d\n",
           tag, r.spawned, r.outer_collapse_step, r.lost_step, r.kills,
           r.pressed_nonsolid, r.bad);
}

int main(void) {
    Run a = run(0);   /* undefended: base should fall            */
    Run b = run(1);   /* defended: base should hold              */
    Run c = run(0);   /* determinism                             */
    report("undefended", a);
    report("defended",   b);

    /* 1) the outer ring collapses (siege -> reroute via the new API)        */
    int ok_reroute = a.outer_collapse_step >= 0;
    /* 2) the core then falls: def_lost flips, strictly after the outer ring  */
    int ok_loss = a.lost_step > a.outer_collapse_step;
    /* 3) cell attribution: every pressed cell is solid                       */
    int ok_cells = a.pressed_nonsolid == 0 && b.pressed_nonsolid == 0;
    /* 4) defended: turrets hold the base (no loss) and do real work          */
    int ok_hold = b.lost_step < 0 && b.kills > (int)(0.5f * b.spawned);
    /* 5) determinism                                                          */
    int ok_det = c.outer_collapse_step == a.outer_collapse_step
                 && c.lost_step == a.lost_step;
    /* 6) no NaN                                                               */
    int ok_nan = (a.bad | b.bad | c.bad) == 0;

    int ok = ok_reroute && ok_loss && ok_cells && ok_hold && ok_det && ok_nan;
    printf("reroute=%d loss=%d cells=%d hold=%d det=%d nan=%d\n",
           ok_reroute, ok_loss, ok_cells, ok_hold, ok_det, ok_nan);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

/* test_turret_arc.c — limited aim arc + turn-then-shoot gate (torrette 2.0).
 *
 * Verifies the defense-layer aiming model introduced with directional
 * placement (Blocco 1):
 *
 *   1. ARC FILTER: a target outside the aim cone is never engaged, one
 *      inside is acquired and killed.
 *   2. TURN-THEN-SHOOT: with aim_tol > 0 the turret holds fire while the
 *      barrel slews onto the target and opens up only when aligned; the
 *      legacy gate-less turret (aim_tol = 0) fires immediately.
 *   3. ±PI SEAM: an arc authored across the wrap (facing = pi) engages
 *      bearings on both signs of the seam and ignores the opposite side.
 *   4. NO PINNING: a limited mount whose wrapped shortest path to the target
 *      crosses its mechanical stops slews the LONG way around inside the arc
 *      (regression: the naive clamp left the barrel stuck on the stop).
 *   5. IDLE SWEEP: with no targets the barrel oscillates between the arc
 *      extents (bounces, stays in bounds) and never fires.
 *   6. DETERMINISM: identical runs are bit-identical.
 *
 * Targets are DORMANT agents (simp_sleep): exact geometry, no walking.
 */
#include "defense.h"
#include <math.h>
#include <stdio.h>

#define DT (1.0f / 60.0f)
#define CAP 512
#define PI_F 3.14159265f

static SimP *world(void) { return simp_create(80, 80, 0.5f, CAP); } /* 40x40 m */

/* free-standing game turret (no emplacement wall): range 15, arc as given */
static int add_turret(DefGame *g, float x, float y, float facing,
                      float arc_deg, float aim_tol) {
    DefTurret t = {0};
    float half = arc_deg * (PI_F / 360.0f);
    t.x = x; t.y = y; t.ang = facing;
    t.arc_min = facing - half; t.arc_max = facing + half;
    t.sweep_dir = 1; t.sweep_speed = 2.5f;
    t.range = 15.0f; t.fire_period = 0.10f; t.damage = 60.0f;
    t.aim_tol = aim_tol;
    return def_add_turret(g, &t);
}

/* dormant standing target; returns its handle */
static SimPHandle target(DefGame *g, SimP *s, float x, float y) {
    SimPHandle h = def_spawn(g, x, y, BT_MAN);
    int i = simp_index_of(s, h);
    if (i >= 0) simp_sleep(s, i);
    return h;
}

static void run(SimP *s, DefGame *g, float secs) {
    for (int k = 0; k < (int)(secs / DT); k++) {
        simp_step(s, DT);
        def_update(g, DT);
    }
}

static int alive(SimP *s, SimPHandle h) { return simp_index_of(s, h) >= 0; }

/* 1) arc filter: inside killed, outside untouched */
static int t_arc_filter(void) {
    SimP *s = world(); DefGame *g = def_create(s, CAP);
    add_turret(g, 20, 20, 0.0f, 90.0f, DEF_AIM_TOL_STD);
    SimPHandle in  = target(g, s, 28.0f, 20.0f);   /* bearing 0: inside   */
    SimPHandle out = target(g, s, 12.0f, 20.0f);   /* bearing pi: outside */
    run(s, g, 5.0f);
    int ok = !alive(s, in) && alive(s, out) && def_kills(g) == 1;
    printf("arc filter : inside dead=%d outside alive=%d kills=%d | %s\n",
           !alive(s, in), alive(s, out), def_kills(g), ok ? "ok" : "BAD");
    def_destroy(g); simp_destroy(s);
    return ok;
}

/* 2) turn-then-shoot: gated turret holds fire while slewing (slew ~0.7 rad at
 * 1 rad/s => aligned after ~0.6 s); the legacy turret opens up immediately */
static int t_turn_then_shoot(void) {
    int shots_early[2], shots_late[2];
    for (int legacy = 0; legacy < 2; legacy++) {
        SimP *s = world(); DefGame *g = def_create(s, CAP);
        int tid = add_turret(g, 20, 20, 0.0f, 120.0f,
                             legacy ? 0.0f : DEF_AIM_TOL_STD);
        def_turret(g, tid)->sweep_speed = 1.0f;    /* slow slew: visible gate */
        target(g, s, 20.0f + 8.0f * cosf(0.7f), 20.0f + 8.0f * sinf(0.7f));
        run(s, g, 0.4f);                           /* still slewing */
        shots_early[legacy] = def_shots(g);
        run(s, g, 1.6f);                           /* aligned long since */
        shots_late[legacy] = def_shots(g);
        def_destroy(g); simp_destroy(s);
    }
    int ok = shots_early[0] == 0 && shots_late[0] > 0 && shots_early[1] > 0;
    printf("turn-shoot : gated early=%d late=%d | legacy early=%d | %s\n",
           shots_early[0], shots_late[0], shots_early[1], ok ? "ok" : "BAD");
    return ok;
}

/* 3) arc across the ±pi seam: both wrap signs engaged, opposite side ignored */
static int t_seam(void) {
    SimP *s = world(); DefGame *g = def_create(s, CAP);
    add_turret(g, 20, 20, PI_F, 90.0f, DEF_AIM_TOL_STD);
    SimPHandle w  = target(g, s, 10.0f, 20.0f);    /* bearing +pi   (inside) */
    SimPHandle sw = target(g, s, 20.0f + 10.0f * cosf(-3.0f),
                                 20.0f + 10.0f * sinf(-3.0f)); /* -3.0 rad,
                                    wraps to 0.28 off centre: inside        */
    SimPHandle e  = target(g, s, 28.0f, 20.0f);    /* bearing 0    (outside) */
    run(s, g, 8.0f);
    int ok = !alive(s, w) && !alive(s, sw) && alive(s, e) && def_kills(g) == 2;
    printf("seam       : west dead=%d sw dead=%d east alive=%d | %s\n",
           !alive(s, w), !alive(s, sw), alive(s, e), ok ? "ok" : "BAD");
    def_destroy(g); simp_destroy(s);
    return ok;
}

/* 4) no pinning: barrel at +2.5 rad, target at bearing -2.5 rad inside a 300°
 * arc — wrapped shortest path crosses the stops, the mount must go the long
 * way (5 rad at 2.5 rad/s = 2 s) instead of pinning on arc_max forever */
static int t_no_pinning(void) {
    SimP *s = world(); DefGame *g = def_create(s, CAP);
    int tid = add_turret(g, 20, 20, 0.0f, 300.0f, DEF_AIM_TOL_STD);
    def_turret(g, tid)->ang = 2.5f;
    SimPHandle h = target(g, s, 20.0f + 10.0f * cosf(-2.5f),
                                20.0f + 10.0f * sinf(-2.5f));
    run(s, g, 6.0f);
    int ok = !alive(s, h);
    printf("no pinning : target dead=%d ang=%.2f | %s\n",
           !alive(s, h), (double)def_turret(g, tid)->ang, ok ? "ok" : "BAD");
    def_destroy(g); simp_destroy(s);
    return ok;
}

/* 5) idle sweep: bounces between the extents, stays in bounds, silent */
static int t_idle_sweep(void) {
    SimP *s = world(); DefGame *g = def_create(s, CAP);
    int tid = add_turret(g, 20, 20, 0.0f, 90.0f, DEF_AIM_TOL_STD);
    DefTurret *t = def_turret(g, tid);
    float lo = 1e9f, hi = -1e9f;
    int flips = 0, prev = t->sweep_dir;
    for (int k = 0; k < (int)(6.0f / DT); k++) {
        simp_step(s, DT);
        def_update(g, DT);
        if (t->ang < lo) lo = t->ang;
        if (t->ang > hi) hi = t->ang;
        if (t->sweep_dir != prev) { flips++; prev = t->sweep_dir; }
    }
    int ok = flips >= 2 && lo >= t->arc_min - 1e-4f && hi <= t->arc_max + 1e-4f
             && hi - lo > 1.0f && def_shots(g) == 0;
    printf("idle sweep : flips=%d span=[%.2f,%.2f] arc=[%.2f,%.2f] shots=%d | %s\n",
           flips, (double)lo, (double)hi, (double)t->arc_min,
           (double)t->arc_max, def_shots(g), ok ? "ok" : "BAD");
    def_destroy(g); simp_destroy(s);
    return ok;
}

/* 6) determinism: scenario 1 twice, same shots/kills/final barrel angle */
static int t_determinism(void) {
    int shots[2], kills[2]; float ang[2];
    for (int r = 0; r < 2; r++) {
        SimP *s = world(); DefGame *g = def_create(s, CAP);
        int tid = add_turret(g, 20, 20, 0.0f, 90.0f, DEF_AIM_TOL_STD);
        target(g, s, 28.0f, 20.0f);
        target(g, s, 26.0f, 24.0f);
        target(g, s, 12.0f, 20.0f);
        run(s, g, 5.0f);
        shots[r] = def_shots(g); kills[r] = def_kills(g);
        ang[r] = def_turret(g, tid)->ang;
        def_destroy(g); simp_destroy(s);
    }
    int ok = shots[0] == shots[1] && kills[0] == kills[1] && ang[0] == ang[1];
    printf("determinism: shots %d/%d kills %d/%d ang %.6f/%.6f | %s\n",
           shots[0], shots[1], kills[0], kills[1],
           (double)ang[0], (double)ang[1], ok ? "ok" : "BAD");
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= t_arc_filter();
    ok &= t_turn_then_shoot();
    ok &= t_seam();
    ok &= t_no_pinning();
    ok &= t_idle_sweep();
    ok &= t_determinism();
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

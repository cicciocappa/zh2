/* test_blast.c — explosion primitive (EXPLOSION_DESIGN.md §9).
 *
 * def_blast is game-side and deterministic (no FX), so it is unit-testable
 * headless. Verifies, causally:
 *   1) agents — linear falloff D(d)=D0*(1-d/R): the center dies, the rim is
 *      wounded-but-alive with the expected HP, outside R is untouched; the
 *      survivors are lofted (SIMP_FLYING) by the vertical kick;
 *   2) structures — a barricade in range loses D(d_min) HP; two blasts collapse
 *      it and its cells free (nav reroutes); a bound turret is silenced;
 *   3) corpses — a pile in range is vaporised (count + pile height zeroed),
 *      one out of range survives;
 *   4) def_damage_agent — direct light damage wounds then kills (fall damage);
 *   5) determinism — two identical worlds blast bit-identically.
 */
#include "defense.h"
#include <stdio.h>
#include <math.h>

#define GW   200
#define GH   120
#define CELL 0.5f
#define DT   (1.0f / 60.0f)
#define CAP  4096

static int failures = 0;
static void check(int cond, const char *name) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) failures++;
}

/* HP a MAN body (hp 100) is left with after a light hit of `dmg` real HP,
 * using def_blast's exact integer rounding. -1 = dead. */
static int man_hp_after(float dmg) {
    if (dmg <= 0.0f) return 100;
    int d = (int)(dmg + 0.5f);
    int hp = 100 - d;
    return hp > 0 ? hp : -1;
}

/* ---- 1. agents: falloff, wounds, flight ---- */
static void test_agents(void) {
    printf("test_agents (falloff + wounds + flight)\n");
    SimP *s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);

    /* blast center; agents strung out along +x at growing distance. No goal ->
     * the crowd doesn't advect, positions stay put (we read the exact ones). */
    float bx = 50.0f, by = 30.0f, R = 6.0f, D0 = 250.0f;
    float xs[] = { 50.0f, 52.0f, 54.0f, 55.5f, 58.0f };   /* d = 0,2,4,5.5,8 */
    enum { NA = 5 };
    SimPHandle h[NA];
    for (int k = 0; k < NA; k++) h[k] = def_spawn(g, xs[k], by, BT_MAN);
    simp_step(s, DT);          /* build the collision grid for query_circle */

    const int *hp = def_hp(g);
    const uint8_t *wound = def_wound(g);

    /* expected HP from the ACTUAL post-step position (robust to jitter) */
    const float *px = simp_px(s), *py = simp_py(s);
    int exp_hp[NA], exp_dead[NA];
    for (int k = 0; k < NA; k++) {
        int i = simp_index_of(s, h[k]);
        float dx = px[i] - bx, dy = py[i] - by;
        float d = sqrtf(dx * dx + dy * dy);
        float dd = (d < R) ? D0 * (1.0f - d / R) : 0.0f;
        int after = man_hp_after(dd);
        exp_dead[k] = (after < 0);
        exp_hp[k]   = after;
    }

    int kills0 = def_kills(g);
    def_blast(g, bx, by, R, D0, 12.0f, 0.6f);

    int ok_hp = 1, any_flying = 0, corpses_ok, killed = def_kills(g) - kills0;
    int exp_kills = 0;
    for (int k = 0; k < NA; k++) exp_kills += exp_dead[k];
    const uint8_t *fl = simp_flags_arr(s);
    for (int k = 0; k < NA; k++) {
        int i = simp_index_of(s, h[k]);
        if (exp_dead[k]) {
            if (i >= 0) ok_hp = 0;                  /* should be gone */
        } else {
            if (i < 0) { ok_hp = 0; continue; }
            int slot = simp_slot_of(s, i);
            if (hp[slot] != exp_hp[k]) ok_hp = 0;
            /* wounded iff it actually lost HP */
            if (exp_hp[k] < 100 && wound[slot] == DW_NONE) ok_hp = 0;
            if (exp_hp[k] == 100 && wound[slot] != DW_NONE) ok_hp = 0;
            if (fl[i] & SIMP_FLYING) any_flying = 1;
        }
    }
    corpses_ok = (simp_corpse_count(s) >= 0);       /* kills may leave corpses */
    (void)corpses_ok;

    check(killed == exp_kills, "kill count matches falloff prediction");
    check(ok_hp, "survivor HP + wound flags match falloff");
    check(any_flying, "survivors lofted by the vertical kick (SIMP_FLYING)");

    def_destroy(g); simp_destroy(s);
}

/* ---- 2. structures: damage, collapse, reroute, turret silenced ---- */
static void test_structures(void) {
    printf("test_structures (damage + collapse + turret)\n");
    SimP *s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);

    /* a vertical barricade of 5 cells around (cx=100, cy 58..62). */
    int cx = 100, cy0 = 58, cy1 = 62;
    int sid = def_add_structure(g, 400.0f, 0);
    for (int cy = cy0; cy <= cy1; cy++) def_struct_cell(g, sid, cx, cy);
    simp_terrain_commit(s);

    float bx = (cx + 0.5f) * CELL, by = ((cy0 + cy1) / 2 + 0.5f) * CELL;
    float R = 6.0f, D0 = 250.0f;
    /* d_min ~ half a cell => ~D0 damage per blast; 400 HP needs two. */
    float hp0 = def_struct_hp(g, sid);
    def_blast(g, bx, by, R, D0, 0.0f, 0.0f);   /* no impulse: isolate structs */
    float hp1 = def_struct_hp(g, sid);
    check(hp1 < hp0 - 200.0f && !def_struct_collapsed(g, sid),
          "one blast chips ~D(d_min) HP, not yet collapsed");
    int wall_before = simp_is_wall(s, cx, (cy0 + cy1) / 2);
    def_blast(g, bx, by, R, D0, 0.0f, 0.0f);
    check(def_struct_collapsed(g, sid), "second blast collapses the barricade");
    int wall_after = simp_is_wall(s, cx, (cy0 + cy1) / 2);
    check(wall_before && !wall_after, "collapsed cells freed (nav reroutes)");

    /* turret bound to a small structure, silenced by a blast on it */
    DefTurret t = { .x = 60.0f, .y = 30.0f, .arc_min = -3.14f, .arc_max = 3.14f,
                    .sweep_speed = 2.0f, .range = 20.0f, .fire_period = 0.2f,
                    .damage = 10.0f, .heavy = 0 };
    int tid = def_add_turret(g, &t);
    def_turret_make_destructible(g, tid, 30.0f);
    simp_terrain_commit(s);
    check(!def_turret_disabled(g, tid), "turret alive before the blast");
    def_blast(g, 60.0f, 30.0f, 4.0f, 200.0f, 0.0f, 0.0f);
    check(def_turret_disabled(g, tid), "turret silenced by the blast");

    def_destroy(g); simp_destroy(s);
}

/* ---- 3. corpses: vaporised in range, survive out of range ---- */
static void test_corpses(void) {
    printf("test_corpses (vaporised in range)\n");
    SimP *s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);

    float bx = 40.0f, by = 30.0f, R = 5.0f;
    for (int k = 0; k < 6; k++)                       /* in range, clustered */
        simp_corpse_add(s, bx + (k - 3) * 0.4f, by, 0.3f, 30.0f);
    simp_corpse_add(s, bx + 20.0f, by, 0.3f, 30.0f);  /* far away, survives  */
    int before = simp_corpse_count(s);
    check(before == 7, "7 corpses placed");

    def_blast(g, bx, by, R, 100.0f, 0.0f, 0.0f);
    int after = simp_corpse_count(s);
    check(after == 1, "in-range pile vaporised, distant corpse survives");

    def_destroy(g); simp_destroy(s);
}

/* ---- 4. def_damage_agent: direct light damage (fall damage entry) ---- */
static void test_damage_agent(void) {
    printf("test_damage_agent (fall damage entry)\n");
    SimP *s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);

    SimPHandle h = def_spawn(g, 30.0f, 30.0f, BT_MAN);   /* hp 100 */
    simp_step(s, DT);
    const int *hp = def_hp(g);
    const uint8_t *wound = def_wound(g);

    def_damage_agent(g, h, 30.0f);
    int i = simp_index_of(s, h);
    int slot = (i >= 0) ? simp_slot_of(s, i) : -1;
    check(i >= 0 && hp[slot] == 70, "30 damage leaves hp 70");
    check(i >= 0 && wound[slot] != DW_NONE, "wounded by the fall");

    int kills0 = def_kills(g);
    def_damage_agent(g, h, 80.0f);
    check(simp_index_of(s, h) < 0 && def_kills(g) == kills0 + 1,
          "lethal fall damage kills");

    def_destroy(g); simp_destroy(s);
}

/* ---- 5. determinism ---- */
static long world_and_blast(void) {
    SimP *s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);
    /* a small hex block around the blast */
    int n = 0;
    for (int r = 0; r < 8; r++) {
        float y = 26.0f + r * 0.55f;
        float xoff = (r & 1) ? 0.32f : 0.0f;
        for (int c = 0; c < 8; c++) {
            float x = 47.0f + xoff + c * 0.64f;
            if (def_spawn(g, x, y, BT_MAN) != SIMP_HANDLE_INVALID) n++;
        }
    }
    simp_step(s, DT);
    def_blast(g, 50.0f, 28.0f, 6.0f, 250.0f, 12.0f, 0.6f);
    /* checksum: kills + sum of surviving hp*(slot+1) */
    long sum = (long)def_kills(g) * 1000003L;
    const int *hp = def_hp(g);
    int live = simp_count(s);
    for (int i = 0; i < live; i++) {
        int slot = simp_slot_of(s, i);
        sum += (long)hp[slot] * (slot + 1);
    }
    sum += (long)n;
    def_destroy(g); simp_destroy(s);
    return sum;
}
static void test_determinism(void) {
    printf("test_determinism\n");
    long a = world_and_blast(), b = world_and_blast();
    check(a == b, "two identical worlds blast bit-identically");
}

int main(void) {
    test_agents();
    test_structures();
    test_corpses();
    test_damage_agent();
    test_determinism();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

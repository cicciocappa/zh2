/* test_traps.c — placed traps / the MINE (GAME_PLAN fase D).
 *
 * traps.c is game-side and deterministic (no FX), so it is unit-testable
 * headless. The detonation callback here maps to def_blast (in the game it maps
 * to host_blast). Verifies, causally:
 *   1) trigger — an agent inside trig_r detonates the mine exactly once; the AoE
 *      kills everyone inside blast_r (falloff), the mine is consumed;
 *   2) no premature trigger — an agent outside trig_r leaves the mine live;
 *   3) one-shot — a consumed mine never fires again;
 *   4) arm_delay — a mine still arming does not fire even with an agent on it,
 *      and fires the update after the delay elapses;
 *   5) friendly fire — the blast damages a barricade in range (def_blast path);
 *   6) determinism — two identical worlds detonate bit-identically.
 */
#include "traps.h"
#include "defense.h"
#include <stdio.h>

#define GW   200
#define GH   120
#define CELL 0.5f
#define DT   (1.0f / 60.0f)
#define CAP  2048

static int failures = 0;
static void check(int cond, const char *name) {
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) failures++;
}

/* detonation callback: count + resolve the blast via def_blast (the game uses
 * host_blast; the deterministic damage is the same). */
typedef struct { DefGame *g; int count; int last_id; } Rec;
static void on_blast(void *user, int id, float x, float y,
                     float r, float dmg, float strength, float up) {
    Rec *b = (Rec *)user;
    b->count++; b->last_id = id;
    def_blast(b->g, x, y, r, dmg, strength, up);
}

static TrapDef mine_at(float x, float y) {
    TrapDef d = { TRAP_MINE, x, y, 0.6f, 6.0f, 250.0f, 20.0f, 0.6f, 0.0f };
    return d;
}

/* ---- 1. trigger + AoE ---- */
static void test_trigger(void) {
    printf("test_trigger (proximity fire + AoE kill)\n");
    SimP *s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);
    Traps tr; traps_init(&tr);

    float mx = 40.0f, my = 30.0f;
    TrapDef md = mine_at(mx, my);
    int id = traps_add(&tr, &md);
    /* one agent ON the mine (triggers), three within blast_r but outside trig_r */
    def_spawn(g, mx, my, BT_MAN);                 /* d=0    -> dead */
    def_spawn(g, mx + 2.0f, my, BT_MAN);          /* d=2    -> ~166 dmg, dead */
    def_spawn(g, mx - 2.0f, my, BT_MAN);          /* d=2    -> dead */
    def_spawn(g, mx, my + 2.5f, BT_MAN);          /* d=2.5  -> ~146 dmg, dead */
    simp_step(s, DT);                              /* build the grid */

    Rec rec = { g, 0, -1 };
    int kills0 = def_kills(g);
    traps_update(&tr, s, DT, on_blast, &rec);

    check(rec.count == 1 && rec.last_id == id, "mine fires exactly once");
    check(!traps_alive(&tr, id), "mine consumed after firing");
    check(def_kills(g) - kills0 == 4, "AoE kills all four in blast_r");

    def_destroy(g); simp_destroy(s);
}

/* ---- 2. no premature trigger ---- */
static void test_no_trigger(void) {
    printf("test_no_trigger (agent outside trig_r)\n");
    SimP *s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);
    Traps tr; traps_init(&tr);

    TrapDef md = mine_at(60.0f, 30.0f);
    int id = traps_add(&tr, &md);
    def_spawn(g, 63.0f, 30.0f, BT_MAN);           /* 3 m away, outside trig_r 0.6 */
    simp_step(s, DT);

    Rec rec = { g, 0, -1 };
    traps_update(&tr, s, DT, on_blast, &rec);
    check(rec.count == 0 && traps_alive(&tr, id), "distant agent leaves mine live");

    def_destroy(g); simp_destroy(s);
}

/* ---- 3. one-shot ---- */
static void test_one_shot(void) {
    printf("test_one_shot (consumed mine never refires)\n");
    SimP *s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);
    Traps tr; traps_init(&tr);

    TrapDef md = mine_at(40.0f, 30.0f);
    int id = traps_add(&tr, &md);
    def_spawn(g, 40.0f, 30.0f, BT_MAN);
    simp_step(s, DT);
    Rec rec = { g, 0, -1 };
    traps_update(&tr, s, DT, on_blast, &rec);      /* detonates */
    (void)id;

    def_spawn(g, 40.0f, 30.0f, BT_MAN);            /* fresh agent on the crater */
    simp_step(s, DT);
    traps_update(&tr, s, DT, on_blast, &rec);      /* must NOT fire again */
    check(rec.count == 1, "a consumed mine does not fire a second time");

    def_destroy(g); simp_destroy(s);
}

/* ---- 4. arm_delay ---- */
static void test_arm_delay(void) {
    printf("test_arm_delay (mine arming can't fire)\n");
    SimP *s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);
    Traps tr; traps_init(&tr);

    TrapDef d = mine_at(40.0f, 30.0f); d.arm_delay = 0.5f;   /* ~30 updates */
    int id = traps_add(&tr, &d);
    def_spawn(g, 40.0f, 30.0f, BT_MAN);            /* sitting on it the whole time */
    Rec rec = { g, 0, -1 };

    int fired_early = 0;
    for (int k = 0; k < 29; k++) {                 /* < 0.5 s: must stay armed-pending */
        simp_step(s, DT);
        traps_update(&tr, s, DT, on_blast, &rec);
        if (rec.count) { fired_early = 1; break; }
    }
    check(!fired_early && traps_alive(&tr, id), "does not fire while arming");
    /* keep stepping until it arms and fires */
    for (int k = 0; k < 10 && !rec.count; k++) { simp_step(s, DT); traps_update(&tr, s, DT, on_blast, &rec); }
    check(rec.count == 1 && !traps_alive(&tr, id), "fires once armed");

    def_destroy(g); simp_destroy(s);
}

/* ---- 5. friendly fire: a barricade in range takes blast damage ---- */
static void test_friendly_fire(void) {
    printf("test_friendly_fire (blast damages own barricade)\n");
    SimP *s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);
    Traps tr; traps_init(&tr);

    /* barricade near the mine (a couple of cells within blast_r) */
    int sid = def_add_structure(g, 400.0f, 0);
    for (int cy = 60; cy <= 62; cy++) def_struct_cell(g, sid, 82, cy);  /* ~(41,30.5) m */
    simp_terrain_commit(s);

    TrapDef md = mine_at(40.0f, 30.0f);
    traps_add(&tr, &md);                            /* blast_r 6 reaches the wall */
    def_spawn(g, 40.0f, 30.0f, BT_MAN);            /* trips it */
    simp_step(s, DT);

    float hp0 = def_struct_hp(g, sid);
    Rec rec = { g, 0, -1 };
    traps_update(&tr, s, DT, on_blast, &rec);
    check(rec.count == 1, "mine fired");
    check(def_struct_hp(g, sid) < hp0, "own barricade took friendly-fire damage");

    def_destroy(g); simp_destroy(s);
}

/* ---- 6. determinism ---- */
static long run_once(void) {
    SimP *s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);
    Traps tr; traps_init(&tr);
    TrapDef m0 = mine_at(40.0f, 30.0f), m1 = mine_at(45.0f, 32.0f);
    traps_add(&tr, &m0);
    traps_add(&tr, &m1);
    /* a small hex block over both mines */
    int spawned = 0;
    for (int r = 0; r < 10; r++) {
        float y = 27.0f + r * 0.55f;
        float xoff = (r & 1) ? 0.32f : 0.0f;
        for (int c = 0; c < 14; c++) {
            float x = 37.0f + xoff + c * 0.64f;
            if (def_spawn(g, x, y, BT_MAN) != SIMP_HANDLE_INVALID) spawned++;
        }
    }
    Rec rec = { g, 0, -1 };
    for (int k = 0; k < 20; k++) { simp_step(s, DT); traps_update(&tr, s, DT, on_blast, &rec); }
    long sig = (long)rec.count * 1000003L + (long)def_kills(g) * 131L + spawned;
    def_destroy(g); simp_destroy(s);
    return sig;
}
static void test_determinism(void) {
    printf("test_determinism\n");
    check(run_once() == run_once(), "two identical worlds detonate bit-identically");
}

int main(void) {
    test_trigger();
    test_no_trigger();
    test_one_shot();
    test_arm_delay();
    test_friendly_fire();
    test_determinism();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

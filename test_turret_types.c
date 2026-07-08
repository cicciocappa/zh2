/* test_turret_types.c — FLAME/ACID cone-AoE turrets + elemental status
 * (torrette 2.0, Blocco 2).
 *
 *   1. CONE: one flame shot tick hits EVERY target inside ±cone_half of the
 *      barrel (checked against a brute-force angle predicate), spares those
 *      in arc but outside the cone, and a wall shields line of sight.
 *   2. BURN: a target in the jet ignites ONCE (single DEF_EV_IGNITE despite
 *      refresh), burns and dies of DoT (DEF_EV_DEATH, kill counted).
 *   3. EXPIRE: with the turret removed after a short lick of flame, the
 *      status ticks its duration out and clears — the victim survives with
 *      reduced HP (the outfit scar stays host-side).
 *   4. ACID: same machine, DST_ACID + DEF_EV_ACID, slower DoT that still
 *      kills a walker in a sustained jet.
 *   5. CLEAR: sustained jetting sweeps the corpses out of the cone
 *      (simp_corpse_clear cadence); a corpse behind the turret survives.
 *   6. DETERMINISM + no NaN.
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

static int add_jet(DefGame *g, float x, float y, float facing, int kind) {
    DefTurret t = {0};
    float half = 90.0f * (PI_F / 360.0f);
    t.x = x; t.y = y; t.ang = facing;
    t.arc_min = facing - half; t.arc_max = facing + half;
    t.sweep_dir = 1; t.sweep_speed = 2.5f;
    t.kind = kind;                       /* cone_half/aim: defaults at add */
    t.range = (kind == TUR_FLAME) ? 12.0f : 18.0f;
    t.fire_period = (kind == TUR_FLAME) ? 0.15f : 0.25f;
    t.damage = (kind == TUR_FLAME) ? 6.0f : 4.0f;
    return def_add_turret(g, &t);
}

static SimPHandle target(DefGame *g, SimP *s, float x, float y, DefBody b) {
    SimPHandle h = def_spawn(g, x, y, b);
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

/* event counters */
static int ev_ignite, ev_acid, ev_death;
static void on_ev(void *u, int slot, int i, DefBody b, DefEvent ev) {
    (void)u; (void)slot; (void)i; (void)b;
    if (ev == DEF_EV_IGNITE) ev_ignite++;
    else if (ev == DEF_EV_ACID) ev_acid++;
    else if (ev == DEF_EV_DEATH) ev_death++;
}
static void ev_reset(void) { ev_ignite = ev_acid = ev_death = 0; }

static uint8_t status_of(DefGame *g, SimP *s, SimPHandle h) {
    int i = simp_index_of(s, h);
    return (i >= 0) ? def_status(g)[simp_slot_of(s, i)] : 255;
}

/* 1) cone AoE vs brute force + LoS shield. Turret locked on the aim target
 * (bearing 0, nearest): the first tick must ignite exactly the agents within
 * ±cone_half of the barrel with clear line of sight. */
static int t_cone(void) {
    SimP *s = world(); DefGame *g = def_create(s, CAP);
    int tid = add_jet(g, 20, 20, 0.0f, TUR_FLAME);
    float ch = def_turret(g, tid)->cone_half;      /* default (~0.21 rad) */
    enum { NT = 6 };
    float bear[NT] = { 0.0f, 0.12f, -0.12f, 0.45f, -0.45f, 0.10f };
    float dist[NT] = { 8.0f, 9.0f,   9.0f,  9.0f,   9.0f, 10.0f };
    SimPHandle h[NT];
    for (int k = 0; k < NT; k++)
        h[k] = target(g, s, 20.0f + dist[k] * cosf(bear[k]),
                            20.0f + dist[k] * sinf(bear[k]), BT_MAN);
    /* wall across the ray to target 5 (bearing 0.10, d=10): shielded */
    { float wx = 20.0f + 5.0f * cosf(0.10f), wy = 20.0f + 5.0f * sinf(0.10f);
      int cx = (int)(wx / 0.5f), cy = (int)(wy / 0.5f);
      for (int dy = -1; dy <= 1; dy++) simp_set_wall(s, cx, cy + dy, true);
      simp_terrain_commit(s); }
    /* first shot leaves at fire_period (barrel starts aligned on bearing 0) */
    run(s, g, 0.2f);
    int ok = def_shots(g) >= 1;
    const float *px = simp_px(s), *py = simp_py(s);
    float ang = def_turret(g, tid)->ang;
    for (int k = 0; k < NT; k++) {
        int i = simp_index_of(s, h[k]);
        float b = atan2f(py[i] - 20.0f, px[i] - 20.0f);
        float d = fabsf(b - ang); while (d > PI_F) d = fabsf(d - 2.0f * PI_F);
        int in_cone = (d <= ch) && (k != 5);       /* 5 = walled off */
        int burning = (status_of(g, s, h[k]) == DST_BURNING);
        if (burning != in_cone) ok = 0;
        if (k == 5 && burning) ok = 0;             /* the wall must shield */
    }
    printf("cone       : shots=%d statuses match brute force + LoS | %s\n",
           def_shots(g), ok ? "ok" : "BAD");
    def_destroy(g); simp_destroy(s);
    return ok;
}

/* 2) burn to death: IGNITE once, then the DoT finishes the job */
static int t_burn(void) {
    SimP *s = world(); DefGame *g = def_create(s, CAP);
    def_set_event_cb(g, on_ev, NULL); ev_reset();
    add_jet(g, 20, 20, 0.0f, TUR_FLAME);
    SimPHandle h = target(g, s, 27.0f, 20.0f, BT_MAN);
    run(s, g, 10.0f);
    int ok = simp_index_of(s, h) < 0 && def_kills(g) == 1 &&
             ev_ignite == 1 && ev_death == 1;
    printf("burn       : dead=%d kills=%d ignite_ev=%d death_ev=%d | %s\n",
           simp_index_of(s, h) < 0, def_kills(g), ev_ignite, ev_death,
           ok ? "ok" : "BAD");
    def_destroy(g); simp_destroy(s);
    return ok;
}

/* 3) expire: a short lick, turret removed, the status burns out and clears */
static int t_expire(void) {
    SimP *s = world(); DefGame *g = def_create(s, CAP);
    def_set_event_cb(g, on_ev, NULL); ev_reset();
    int tid = add_jet(g, 20, 20, 0.0f, TUR_FLAME);
    SimPHandle h = target(g, s, 27.0f, 20.0f, BT_OBESE);   /* 160 HP: survives */
    run(s, g, 0.5f);
    int lit = (status_of(g, s, h) == DST_BURNING);
    def_remove_turret(g, tid);                             /* jet gone */
    run(s, g, 7.0f);                                       /* > BURN_DUR */
    int i = simp_index_of(s, h);
    int hp = (i >= 0) ? def_hp(g)[simp_slot_of(s, i)] : -1;
    int ok = lit && i >= 0 && status_of(g, s, h) == DST_NONE &&
             hp > 0 && hp < 160 && ev_ignite == 1;
    printf("expire     : lit=%d alive=%d status=%d hp=%d/160 | %s\n",
           lit, i >= 0, status_of(g, s, h), hp, ok ? "ok" : "BAD");
    def_destroy(g); simp_destroy(s);
    return ok;
}

/* 4) acid: DST_ACID + DEF_EV_ACID, kills a walker under a sustained jet */
static int t_acid(void) {
    SimP *s = world(); DefGame *g = def_create(s, CAP);
    def_set_event_cb(g, on_ev, NULL); ev_reset();
    add_jet(g, 20, 20, 0.0f, TUR_ACID);
    SimPHandle h = target(g, s, 30.0f, 20.0f, BT_MAN);
    run(s, g, 0.5f);
    int st = status_of(g, s, h);
    run(s, g, 10.0f);
    int ok = st == DST_ACID && ev_acid == 1 && ev_ignite == 0 &&
             simp_index_of(s, h) < 0 && def_kills(g) == 1;
    printf("acid       : status=%d acid_ev=%d dead=%d | %s\n",
           st, ev_acid, simp_index_of(s, h) < 0, ok ? "ok" : "BAD");
    def_destroy(g); simp_destroy(s);
    return ok;
}

/* 5) corpse clear: sustained jetting sweeps the piles out of the cone */
static int t_clear(void) {
    SimP *s = world(); DefGame *g = def_create(s, CAP);
    add_jet(g, 20, 20, 0.0f, TUR_FLAME);
    /* 600 HP keep the jet on; at 30 m the tank clears the pile by >1 m (a
     * corpse overlapping the disc would PBD-shove it out of range) */
    target(g, s, 30.0f, 20.0f, BT_TANK);
    for (int k = 0; k < 5; k++)                 /* pile inside the cone */
        simp_corpse_add(s, 24.0f + 0.7f * k, 20.0f, 0.27f, 60.0f);
    simp_corpse_add(s, 14.0f, 20.0f, 0.27f, 60.0f);   /* behind: survives */
    int before = simp_corpse_count(s);
    run(s, g, 2.5f);                            /* > CORPSE_CLEAR_PERIOD */
    int after = simp_corpse_count(s);
    int ok = before == 6 && after == 1;
    printf("clear      : corpses %d -> %d (dietro=1) shots=%d | %s\n",
           before, after, def_shots(g), ok ? "ok" : "BAD");
    def_destroy(g); simp_destroy(s);
    return ok;
}

/* 6) determinism + no NaN on the burn scenario with a small crowd */
static int t_determinism(void) {
    int shots[2], kills[2]; double sum[2];
    for (int r = 0; r < 2; r++) {
        SimP *s = world(); DefGame *g = def_create(s, CAP);
        add_jet(g, 20, 20, 0.0f, TUR_FLAME);
        add_jet(g, 22, 24, -1.2f, TUR_ACID);
        for (int k = 0; k < 12; k++)
            target(g, s, 24.0f + (float)(k % 4), 18.0f + (float)(k / 4) * 1.5f,
                   (k % 3 == 0) ? BT_OBESE : BT_MAN);
        run(s, g, 4.0f);            /* alcuni sopravvivono: checksum non banale */
        shots[r] = def_shots(g); kills[r] = def_kills(g);
        sum[r] = 0.0;
        const float *px = simp_px(s), *py = simp_py(s);
        for (int i = 0; i < simp_count(s); i++) {
            if (!isfinite(px[i]) || !isfinite(py[i])) sum[r] = 1e300;
            sum[r] += (double)px[i] * 1.000001 + (double)py[i];
        }
        def_destroy(g); simp_destroy(s);
    }
    int ok = shots[0] == shots[1] && kills[0] == kills[1] && sum[0] == sum[1]
             && sum[0] < 1e299;
    printf("determinism: shots %d/%d kills %d/%d chk %.6f/%.6f | %s\n",
           shots[0], shots[1], kills[0], kills[1], sum[0], sum[1],
           ok ? "ok" : "BAD");
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= t_cone();
    ok &= t_burn();
    ok &= t_expire();
    ok &= t_acid();
    ok &= t_clear();
    ok &= t_determinism();
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

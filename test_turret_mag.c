/* test_turret_mag.c — turret magazines (BIOMASS_DESIGN §5, §11).
 *
 *   1. LEGACY: mag_size 0 vs a mag too big to ever empty -> shots, kills and
 *      agent positions BIT-IDENTICAL (the mag path is pure gating).
 *   2. MAG N: exactly N shots, then silence for reload_s (def_turret_reloading
 *      counts down), then fire resumes on cadence.
 *   3. RELOAD_NOW: mid-reload refill -> countdown zeroed, fire resumes at once.
 *   4. FLAME: the jet decrements per activation TICK — N ticks then reload.
 *   5. LURE: the fire lure dies out during the reload (fired = 0 -> linger
 *      expires -> stamp removed EXACTLY) and comes back when fire resumes.
 *   6. DETERMINISM: same scenario twice, bit-identical.
 */
#include "defense.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DT (1.0f / 60.0f)
#define CAP 1024

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* 60x40 m open world, no goal (targets idle in place), tough targets parked
 * in front of the turret spot so it always has something to shoot. */
static SimP *make_world(DefGame **gout) {
    SimP *s = simp_create(120, 80, 0.5f, CAP);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, CAP);
    for (int i = 0; i < 24; i++) {
        float x = 22.0f + (float)(i % 6) * 0.9f, y = 17.0f + (float)(i / 6) * 0.9f;
        if (simp_free_at(s, x, y, 0.4f)) def_spawn(g, x, y, BT_OBESE);
    }
    *gout = g;
    return s;
}

static int add_mag_turret(DefGame *g, int kind, int mag, float reload_s) {
    DefTurret t = {0};
    t.x = 30.0f; t.y = 20.0f; t.range = 12.0f;
    t.arc_min = -3.1416f; t.arc_max = 3.1416f;
    t.sweep_dir = 1; t.sweep_speed = 3.0f;
    t.kind = kind;
    t.fire_period = (kind == TUR_FLAME) ? 0.15f : 0.10f;
    t.damage = 1.0f;
    t.mag_size = mag; t.reload_s = reload_s;
    return def_add_turret(g, &t);
}

static double checksum(SimP *s) {
    double sum = 0.0;
    const float *px = simp_px(s), *py = simp_py(s);
    for (int i = 0; i < simp_count(s); i++) {
        if (!isfinite(px[i]) || !isfinite(py[i])) return 1e300;    /* NaN trip */
        sum += (double)px[i] * 1.000001 + (double)py[i];
    }
    return sum;
}

/* (1) legacy guard: the mag machinery must be pure gating. */
static void test_legacy(void) {
    int shots[2], kills[2]; double cks[2];
    for (int run = 0; run < 2; run++) {
        DefGame *g; SimP *s = make_world(&g);
        add_mag_turret(g, TUR_LIGHT, run ? 1000000 : 0, 5.0f);
        for (int k = 0; k < (int)(10.0f / DT); k++) { simp_step(s, DT); def_update(g, DT); }
        shots[run] = def_shots(g); kills[run] = def_kills(g); cks[run] = checksum(s);
        def_destroy(g); simp_destroy(s);
    }
    CHECK(shots[0] == shots[1] && kills[0] == kills[1] && cks[0] == cks[1]
          && cks[0] < 1e299,
          "legacy: mag 0 vs never-empty identical (shots %d/%d kills %d/%d)",
          shots[0], shots[1], kills[0], kills[1]);
    CHECK(shots[0] > 50, "legacy: turret actually fired (%d shots)", shots[0]);
}

/* (2)+(3)+(6): mag N cycle, reload_now, determinism. */
static double mag_cycle(int use_reload_now, int *shots_out) {
    DefGame *g; SimP *s = make_world(&g);
    const int MAG = 5; const float RELOAD = 2.0f;
    int tid = add_mag_turret(g, TUR_LIGHT, MAG, RELOAD);
    int k_empty = -1, k_resume = -1, k_now = -1;
    int steps = (int)(8.0f / DT);
    for (int k = 0; k < steps; k++) {
        int before = def_shots(g);
        simp_step(s, DT); def_update(g, DT);
        if (k_empty < 0 && def_shots(g) == MAG && before < MAG) {
            k_empty = k;
            CHECK(def_turret_reloading(g, tid) > RELOAD - DT * 1.5f,
                  "countdown starts at reload_s (%.2f)",
                  (double)def_turret_reloading(g, tid));
        }
        if (k_empty >= 0 && k_resume < 0 && def_shots(g) > MAG) k_resume = k;
        if (use_reload_now && k_empty >= 0 && k_now < 0 && k - k_empty == 30) {
            def_turret_reload_now(g, tid);          /* mid-reload (0.5 s in) */
            k_now = k;
            CHECK(def_turret_reloading(g, tid) == 0.0f, "reload_now zeroes countdown");
        }
        if (k_empty >= 0 && k_resume < 0)
            CHECK(def_shots(g) == MAG, "silent while reloading (step %d)", k);
    }
    CHECK(k_empty >= 0, "mag emptied");
    CHECK(k_resume >= 0, "fire resumed");
    if (!use_reload_now) {
        /* silence = reload_s, then the fire timer refills (~fire_period) */
        float gap = (float)(k_resume - k_empty) * DT;
        CHECK(gap >= RELOAD - DT && gap <= RELOAD + 0.1f + 3.0f * DT,
              "silence lasts reload_s (gap %.3f s)", (double)gap);
        CHECK(def_turret_reloading(g, tid) == 0.0f, "ready after cycle");
    } else {
        float gap = (float)(k_resume - k_now) * DT;
        CHECK(k_now >= 0 && gap <= 0.1f + 3.0f * DT,
              "fire resumes right after reload_now (gap %.3f s)", (double)gap);
    }
    *shots_out = def_shots(g);
    double cks = checksum(s);
    CHECK(cks < 1e299, "no NaN");
    def_destroy(g); simp_destroy(s);
    return cks;
}

/* (4) flame: activation ticks spend the mag. */
static void test_flame(void) {
    DefGame *g; SimP *s = make_world(&g);
    int tid = add_mag_turret(g, TUR_FLAME, 3, 4.0f);
    int ticks_at_empty = -1;
    for (int k = 0; k < (int)(3.0f / DT); k++) {
        simp_step(s, DT); def_update(g, DT);
        if (ticks_at_empty < 0 && def_turret_reloading(g, tid) > 0.0f)
            ticks_at_empty = def_shots(g);
    }
    CHECK(ticks_at_empty == 3, "flame: 3 jet ticks then reload (got %d)", ticks_at_empty);
    CHECK(def_shots(g) == 3, "flame: silent for the whole reload");
    def_destroy(g); simp_destroy(s);
}

/* (5) the lure stamp is removed while the turret reloads, and returns after. */
static void test_lure_reload(void) {
    DefGame *g; SimP *s = make_world(&g);
    int n = simp_grid_w(s) * simp_grid_h(s);
    float *base = (float *)malloc((size_t)n * sizeof(float));
    memcpy(base, simp_user_cost(s), (size_t)n * sizeof(float));   /* pristine */
    int tid = add_mag_turret(g, TUR_LIGHT, 5, 3.0f);
    def_set_fire_lure(g, -0.5f, 6.0f, 0.5f);        /* linger << reload */
    int stamped = 0, removed_mid_reload = 0, restamped = 0;
    for (int k = 0; k < (int)(8.0f / DT); k++) {
        simp_step(s, DT); def_update(g, DT);
        int diff = memcmp(base, simp_user_cost(s), (size_t)n * sizeof(float)) != 0;
        float rel = def_turret_reloading(g, tid);
        if (diff && rel == 0.0f && !stamped) stamped = 1;
        if (!diff && rel > 0.0f && rel < 2.0f) removed_mid_reload = 1;
        if (stamped && removed_mid_reload && diff && rel == 0.0f) restamped = 1;
    }
    CHECK(stamped, "lure stamped while firing");
    CHECK(removed_mid_reload, "lure removed during reload (past linger)");
    CHECK(restamped, "lure re-stamped after reload");
    free(base); def_destroy(g); simp_destroy(s);
}

int main(void) {
    test_legacy();

    int sh1, sh2, sh3;
    double c1 = mag_cycle(0, &sh1);
    double c2 = mag_cycle(0, &sh2);
    CHECK(c1 == c2 && sh1 == sh2, "determinism: mag cycle bit-identical");
    mag_cycle(1, &sh3);
    CHECK(sh3 >= sh1, "reload_now run fires at least as much");

    test_flame();
    test_lure_reload();

    if (fails == 0) printf("test_turret_mag: OK\n");
    else            printf("test_turret_mag: %d FAILURES\n", fails);
    return fails ? 1 : 0;
}

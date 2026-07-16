/* test_soldier.c — playable soldier (soldier.h, SOLDIER_DESIGN.md).
 *
 *   1. DRIVE: open-field walk covers ground at ~walk speed; driving into a
 *      wall stops at the SDF (no penetration), zero NaN.
 *   2. MITRA: held trigger fires at the exact period; the first agent on the
 *      ray is hit at full damage in the open, attenuated (0 < dmg < full)
 *      through a fence cell (opacity 0.3), occluded (tracer only, no hit)
 *      behind a full wall.
 *   3. CONTATTO: zombies in touch range drain HP per-toucher and the
 *      knockback pushes the soldier away with no drive input; sustained
 *      contact puts him DOWN — body + lure removed (cost_user BIT-identical
 *      to the pre-deploy baseline), deploy refused during the lockout and
 *      accepted after it.
 *   4. LURE: a marching column bends toward the standing soldier — the
 *      integrated occupancy near him grows by a large factor vs lure off.
 *   5. DETERMINISMO: two identical drive-and-shoot runs are bit-identical.
 */
#define _POSIX_C_SOURCE 199309L
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "soldier.h"

#define DT (1.0f / 60.0f)
#define CAP 4096

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok  %s\n", msg); \
    else { printf("  FAIL %s\n", msg); g_fail = 1; } \
} while (0)

static SimP *make_world(void) {
    SimP *s = simp_create(120, 80, 0.5f, CAP);      /* 60 x 40 m */
    for (int cy = 36; cy <= 44; cy++)
        for (int cx = 116; cx <= 119; cx++) simp_set_goal(s, cx, cy, true);
    simp_terrain_commit(s);
    return s;
}

static double checksum(const SimP *s) {
    double sum = 0.0;
    const float *px = simp_px(s), *py = simp_py(s);
    for (int i = 0; i < simp_count(s); i++) {
        if (!isfinite(px[i]) || !isfinite(py[i])) return 1e300;
        sum += (double)px[i] * 1.000001 + (double)py[i];
    }
    return sum;
}

/* ---- shot recorder --------------------------------------------------------- */

typedef struct {
    SimP *s;
    int shots, hits;
    float last_dmg;
    float last_ex, last_ey;
    int kill;                   /* apply the hit as an instant kill */
} Rec;

static void on_shot(void *ud, float ox, float oy, float ex, float ey,
                    int hit, float dmg) {
    (void)ox; (void)oy;
    Rec *r = (Rec *)ud;
    r->shots++;
    r->last_ex = ex; r->last_ey = ey;
    if (hit >= 0) {
        r->hits++;
        r->last_dmg = dmg;
        if (r->kill) simp_kill(r->s, hit);
    }
}

/* ---- 1. drive -------------------------------------------------------------- */

static void test_drive(void) {
    printf("1. drive (open field + wall stop)\n");
    SimP *s = make_world();
    Soldier *sol = soldier_create(s, NULL);
    CHECK(soldier_deploy(sol, 10.0f, 20.0f), "deploy in campo aperto");

    for (int k = 0; k < (int)(5.0f / DT); k++) {
        soldier_step(sol, 1.0f, 0.0f, 0, 0, false, DT, NULL, NULL);
        simp_step(s, DT);
    }
    float walked = soldier_x(sol) - 10.0f;
    printf("     percorsi %.1f m in 5 s\n", (double)walked);
    CHECK(walked > 15.0f && walked < 22.0f, "avanza ~ a velocita' di marcia");
    CHECK(fabsf(soldier_y(sol) - 20.0f) < 0.5f, "niente deriva laterale");

    /* wall column right ahead: drive into it for 5 more seconds */
    int wallcx = (int)((soldier_x(sol) + 3.0f) / 0.5f);
    for (int cy = 0; cy < 80; cy++) simp_set_wall(s, wallcx, cy, true);
    simp_terrain_commit(s);
    for (int k = 0; k < (int)(5.0f / DT); k++) {
        soldier_step(sol, 1.0f, 0.0f, 0, 0, false, DT, NULL, NULL);
        simp_step(s, DT);
    }
    float wall_x = wallcx * 0.5f;
    printf("     fermo a %.2f m dal muro (bordo cella %.2f)\n",
           (double)(wall_x - soldier_x(sol)), (double)wall_x);
    CHECK(soldier_x(sol) < wall_x - 0.2f, "il muro lo ferma (SDF)");
    CHECK(isfinite(soldier_x(sol)) && isfinite(soldier_y(sol)), "zero NaN");
    soldier_destroy(sol);
    simp_destroy(s);
}

/* ---- 2. machine gun --------------------------------------------------------- */

static void test_gun(void) {
    printf("2. mitra (rateo, transmit, occlusione)\n");
    SimP *s = make_world();
    SoldierDef d; soldier_def_defaults(&d);
    d.lure_w = 0.0f;                     /* isolate the gun */
    Soldier *sol = soldier_create(s, &d);
    soldier_deploy(sol, 10.0f, 20.0f);

    /* pack of dormants 8 m east */
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            simp_spawn_dormant(s, 18.0f + c * 0.7f, 18.9f + r * 0.7f);
    simp_step(s, DT);                    /* bin the grid once */

    Rec rec = { s, 0, 0, 0, 0, 0, 0 };
    int nsteps = (int)(3.0f / DT);
    for (int k = 0; k < nsteps; k++) {
        soldier_step(sol, 0, 0, 20.0f, 20.0f, true, DT, on_shot, &rec);
        simp_step(s, DT);
    }
    int want = (int)(3.0f / d.gun_period);
    printf("     %d colpi in 3 s (attesi ~%d), %d a segno, dmg %.1f\n",
           rec.shots, want, rec.hits, (double)rec.last_dmg);
    CHECK(abs(rec.shots - want) <= 1, "rateo esatto (accumulatore)");
    CHECK(rec.hits == rec.shots, "pack davanti: ogni colpo a segno");
    CHECK(fabsf(rec.last_dmg - d.gun_damage) < 1e-3f,
          "campo aperto: danno pieno");

    /* fence between (solid + opacity 0.3): damage attenuated, still hits */
    for (int cy = 30; cy < 50; cy++) {
        simp_set_wall(s, 28, cy, true);
        simp_set_opacity(s, 28, cy, 0.3f);
    }
    simp_terrain_commit(s);
    Rec rf = { s, 0, 0, 0, 0, 0, 0 };
    for (int k = 0; k < 30; k++) {
        soldier_step(sol, 0, 0, 20.0f, 20.0f, true, DT, on_shot, &rf);
        simp_step(s, DT);
    }
    printf("     dietro cancellata: dmg %.1f (pieno %.1f)\n",
           (double)rf.last_dmg, (double)d.gun_damage);
    CHECK(rf.hits > 0, "la cancellata si spara attraverso");
    CHECK(rf.last_dmg > 0.2f * d.gun_damage &&
          rf.last_dmg < 0.95f * d.gun_damage, "danno attenuato dal transmit");

    /* full wall: occluded, tracer stops at the wall, no hit */
    for (int cy = 30; cy < 50; cy++) simp_set_opacity(s, 28, cy, 1.0f);
    simp_terrain_commit(s);
    Rec rw = { s, 0, 0, 0, 0, 0, 0 };
    for (int k = 0; k < 30; k++) {
        soldier_step(sol, 0, 0, 20.0f, 20.0f, true, DT, on_shot, &rw);
        simp_step(s, DT);
    }
    printf("     dietro muro pieno: %d a segno su %d, tracer a x %.1f\n",
           rw.hits, rw.shots, (double)rw.last_ex);
    CHECK(rw.hits == 0 && rw.shots > 0, "muro pieno: occluso");
    CHECK(rw.last_ex <= 28 * 0.5f + 0.6f, "tracer troncato al muro");
    soldier_destroy(sol);
    simp_destroy(s);
}

/* ---- 3. contact / down ------------------------------------------------------ */

static void test_contact(void) {
    printf("3. contatto (HP, knockback, DOWN, rimozione lure esatta)\n");
    SimP *s = make_world();

    /* cost_user baseline BEFORE any stamp */
    int ncells = simp_grid_w(s) * simp_grid_h(s);
    float *base = (float *)malloc((size_t)ncells * sizeof(float));
    memcpy(base, simp_user_cost(s), (size_t)ncells * sizeof(float));

    SoldierDef d; soldier_def_defaults(&d);
    d.hp_max = 40.0f;                    /* quick death for the down test */
    d.touch_pad = 0.35f;                 /* wide bite reach: with the stock
                                            pad the contact margin is ~0.1 m
                                            and the knockback drift dies with
                                            the contact after a few steps */
    Soldier *sol = soldier_create(s, &d);

    /* knockback probe, open field: ONE dormant just inside bite reach from
     * the west (NO disc overlap: an overlapped spawn gets ejected west by
     * the PBD at recovered-velocity speed and the contact dies in 3 steps —
     * measured), no drive -> the bite pushes the soldier east until the
     * widened reach breaks */
    soldier_deploy(sol, 25.0f, 25.0f);
    simp_spawn_dormant(s, 24.25f, 25.0f);
    simp_step(s, DT);
    float x0 = soldier_x(sol);
    int max_touch = 0;
    for (int k = 0; k < 60; k++) {
        soldier_step(sol, 0, 0, 0, 0, false, DT, NULL, NULL);
        if (soldier_touchers(sol) > max_touch) max_touch = soldier_touchers(sol);
        simp_step(s, DT);
    }
    printf("     touchers max %d, spinta est %.2f m, hp %.1f/%.0f\n",
           max_touch, (double)(soldier_x(sol) - x0),
           (double)soldier_hp(sol), (double)d.hp_max);
    CHECK(max_touch >= 1, "contatto rilevato");
    CHECK(soldier_hp(sol) < d.hp_max, "i morsi drenano HP");
    CHECK(soldier_x(sol) > x0 + 0.05f, "knockback: spinto via senza input");
    soldier_recall(sol);

    /* DOWN: pin a clump of dormants into a wall CORNER and keep driving in
     * (in the open the heavy body shoves the light dormants aside and the
     * contact breaks — measured; the corner blocks the lateral escape). */
    for (int c = 30; c < 44; c++) { simp_set_wall(s, 35, c, true);
                                    simp_set_wall(s, c, 35, true); }
    simp_terrain_commit(s);
    simp_spawn_dormant(s, 18.4f, 18.4f);
    simp_spawn_dormant(s, 19.0f, 18.4f);
    simp_spawn_dormant(s, 18.4f, 19.0f);
    simp_spawn_dormant(s, 19.0f, 19.0f);
    simp_step(s, DT);
    CHECK(soldier_deploy(sol, 19.8f, 19.8f), "deploy nel cantone");
    int steps = 0;
    while (soldier_active(sol) && steps < (int)(30.0f / DT)) {
        soldier_step(sol, -1.0f, -1.0f, 0, 0, false, DT, NULL, NULL);
        simp_step(s, DT);
        steps++;
    }
    printf("     a terra dopo %.1f s di mischia\n", (double)steps * DT);
    CHECK(!soldier_active(sol), "contatto sostenuto -> DOWN");
    CHECK(soldier_down(sol) > 0.0f, "lockout armato");
    CHECK(memcmp(base, simp_user_cost(s),
                 (size_t)ncells * sizeof(float)) == 0,
          "lure rimosso BIT-esatto al down");
    CHECK(!soldier_deploy(sol, 40.0f, 20.0f), "deploy rifiutato nel lockout");
    for (int k = 0; k < (int)((d.down_s + 1.0f) / DT); k++) {
        soldier_step(sol, 0, 0, 0, 0, false, DT, NULL, NULL);
        simp_step(s, DT);
    }
    CHECK(soldier_down(sol) == 0.0f, "lockout scaduto");
    CHECK(soldier_deploy(sol, 40.0f, 20.0f), "deploy di nuovo permesso");
    CHECK(fabsf(soldier_hp(sol) - d.hp_max) < 1e-4f, "HP pieni al respawn");

    /* manual recall also restores cost_user bit-exactly */
    for (int k = 0; k < 120; k++) {      /* wander: several restamps */
        soldier_step(sol, 0.7f, 0.7f, 0, 0, false, DT, NULL, NULL);
        simp_step(s, DT);
    }
    soldier_recall(sol);
    CHECK(memcmp(base, simp_user_cost(s),
                 (size_t)ncells * sizeof(float)) == 0,
          "recall dopo vagabondaggio: cost_user bit-esatto");
    free(base);
    soldier_destroy(sol);
    simp_destroy(s);
}

/* ---- 4. lure bends routes ---------------------------------------------------- */

/* March a column west->east past an off-axis soldier; return the integrated
 * occupancy (agent-steps) of the NORTH half (y > 23) of the mid band — the
 * straight route never goes there, only the bend toward the soldier does. */
static double lure_run(int lure_on) {
    SimP *s = make_world();
    simp_params(s)->k_density = 0.0f;   /* pin the density/jam routing: the
                                           column fans out on its own rho and
                                           pollutes the north-half baseline */
    simp_params(s)->k_jam = 0.0f;
    SoldierDef d; soldier_def_defaults(&d);
    d.hp_max = 1e9f;                     /* he must survive the mob */
    if (!lure_on) d.lure_w = 0.0f;
    Soldier *sol = soldier_create(s, &d);
    float sx = 30.0f, sy = 26.0f;        /* ~5 m north of the column */
    soldier_deploy(sol, sx, sy);

    for (int k = 0; k < 80; k++) {       /* column x 2..14.4, y 19.5..21.5 */
        float gx = 2.0f + (float)(k % 20) * 0.65f;
        float gy = 19.5f + (float)(k / 20) * 0.65f;
        if (simp_free_at(s, gx, gy, 0.3f)) simp_spawn(s, gx, gy);
    }
    double occ = 0.0;
    const float *px = simp_px(s), *py = simp_py(s);
    for (int k = 0; k < (int)(40.0f / DT); k++) {
        soldier_step(sol, 0, 0, 0, 0, false, DT, NULL, NULL);
        simp_step(s, DT);
        for (int i = 0; i < simp_count(s); i++)
            if (px[i] > 24.0f && px[i] < 36.0f && py[i] > 23.0f) occ += 1.0;
    }
    if (checksum(s) > 1e299) occ = -1.0;              /* NaN trip */
    soldier_destroy(sol);
    simp_destroy(s);
    return occ;
}

static void test_lure_drift(void) {
    printf("4. lure (la colonna piega verso il soldato)\n");
    double off = lure_run(0), on = lure_run(1);
    printf("     occupazione meta' nord: off %.0f, on %.0f\n", off, on);
    CHECK(off >= 0.0 && on >= 0.0, "zero NaN nelle due run");
    CHECK(on > 3.0 * off + 300.0, "attrazione misurata (deviazione a nord)");
}

/* ---- 5. determinism ----------------------------------------------------------- */

static double determinism_run(void) {
    SimP *s = make_world();
    Soldier *sol = soldier_create(s, NULL);
    soldier_deploy(sol, 10.0f, 20.0f);
    for (int r = 0; r < 6; r++)
        for (int c = 0; c < 6; c++)
            simp_spawn(s, 30.0f + c * 0.7f, 17.0f + r * 0.7f);
    Rec rec = { s, 0, 0, 0, 0, 0, 1 };   /* kill on hit */
    for (int k = 0; k < (int)(10.0f / DT); k++) {
        float mvx = (k / 120) % 2 ? 1.0f : -0.3f;    /* weave */
        float mvy = (k / 90) % 2 ? 0.5f : -0.5f;
        soldier_step(sol, mvx, mvy, 35.0f, 20.0f, (k / 30) % 2 == 0,
                     DT, on_shot, &rec);
        simp_step(s, DT);
    }
    double sum = checksum(s) + rec.shots * 7.0 + rec.hits * 13.0
               + (double)soldier_hp(sol) + (double)soldier_x(sol) * 3.0;
    soldier_destroy(sol);
    simp_destroy(s);
    return sum;
}

static void test_determinism(void) {
    printf("5. determinismo\n");
    double a = determinism_run(), b = determinism_run();
    CHECK(a == b, "due run bit-identiche");
    CHECK(a < 1e299, "zero NaN");
}

int main(void) {
    test_drive();
    test_gun();
    test_contact();
    test_lure_drift();
    test_determinism();
    if (g_fail) { printf("test_soldier: FAIL\n"); return 1; }
    printf("test_soldier: OK (drive, mitra, contatto/down, lure, determinismo)\n");
    return 0;
}

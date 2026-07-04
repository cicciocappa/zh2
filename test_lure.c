/* test_lure.c — richiamo da fuoco (def_set_fire_lure, 2026-07-04).
 *
 *   1. GAMEPLAY: corridoio ovest->est, torretta distruttibile 5.5 m fuori
 *      asse (fuori reach di contatto, dentro il suo range). Lure OFF: il
 *      fiume le scorre accanto, la torretta spara impunita (hp ~pieni).
 *      Lure ON: il rumore piega le rotte dentro il disco, l'orda la mobba,
 *      il contatto la consuma (hp molto giu' o crollata).
 *   2. RIMOZIONE ESATTA: due torrette a dischi sovrapposti sopra una chiazza
 *      dipinta vicino al clamp (-0.6): sparano (lure applicati), poi mappa
 *      svuotata -> silenzio oltre il linger -> cost_user BIT-IDENTICO alla
 *      baseline (i delta letti di ritorno battono il clamp a -0.8).
 *   3. DETERMINISMO: due run dello scenario 1-ON bit-identiche.
 */
#include "defense.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define DT (1.0f / 60.0f)
#define CAP 4096

static SimP *make_world(float k_danger) {
    SimP *s = simp_create(120, 80, 0.5f, CAP);      /* 60 x 40 m */
    simp_params(s)->k_danger = k_danger;            /* esperimento sangue vs lure */
    for (int cy = 36; cy <= 44; cy++)
        for (int cx = 116; cx <= 119; cx++) simp_set_goal(s, cx, cy, true);
    simp_terrain_commit(s);
    return s;
}

static int add_turret(DefGame *g, float x, float y, float range, float hp) {
    DefTurret t = {0};
    t.x = x; t.y = y; t.range = range;
    t.arc_min = -3.1416f; t.arc_max = 3.1416f;
    t.sweep_dir = 1; t.sweep_speed = 3.0f;
    t.fire_period = 0.10f; t.damage = 55.0f;
    int tid = def_add_turret(g, &t);
    def_turret_make_destructible(g, tid, hp);
    return tid;
}

/* corridor run; lure_w < 0 = on. Returns final turret struct hp via *hp_out,
 * kills via *kills_out, and a position checksum for determinism. */
static double corridor_run(float lure_w, float k_danger, float *hp_out, int *kills_out) {
    SimP *s = make_world(k_danger);
    DefGame *g = def_create(s, CAP);
    def_set_turret_contact(g, 0.0f, 2.0f);          /* reach di gioco */
    int tid = add_turret(g, 30.0f, 25.5f, 12.0f, 250.0f);
    simp_terrain_commit(s);
    if (lure_w < 0.0f) def_set_fire_lure(g, lure_w, 8.0f, 2.5f);

    DefRect r = { 1.0f, 18.0f, 2.0f, 4.0f };
    DefDirectorCfg dc = {0};
    dc.rects = &r; dc.nrects = 1; dc.spawn_radius = 0.34f;
    dc.base_rate = 15.0f; dc.wave_period = 1e9f;    /* flat rate */
    dc.seed = 0xC0FFEEu;
    DefDirector *d = def_director_create(g, &dc);

    for (int k = 0; k < (int)(40.0f / DT); k++) {
        def_director_update(d, DT);
        simp_step(s, DT);
        def_update(g, DT);
    }
    int sid = -1;
    for (int q = 0; q < def_struct_count(g); q++)
        if (def_struct_is_turret(g, q)) sid = q;
    *hp_out = def_struct_hp(g, sid);
    *kills_out = def_kills(g);
    (void)tid;
    double sum = 0.0;
    const float *px = simp_px(s), *py = simp_py(s);
    for (int i = 0; i < simp_count(s); i++) {
        if (!isfinite(px[i]) || !isfinite(py[i])) sum = 1e300;   /* NaN trip */
        sum += (double)px[i] * 1.000001 + (double)py[i];
    }
    def_director_destroy(d); def_destroy(g); simp_destroy(s);
    return sum;
}

static int exact_removal(void) {
    SimP *s = make_world(400.0f);
    /* chiazza attrattiva dipinta vicino al clamp: i lure sopra la spingono
     * a -0.8 e il ripristino cieco fallirebbe */
    for (int cy = 46; cy <= 58; cy++)
        for (int cx = 52; cx <= 68; cx++) simp_add_cost(s, cx, cy, -0.6f);
    int n = simp_grid_w(s) * simp_grid_h(s);
    float *base = (float *)malloc((size_t)n * sizeof(float));
    memcpy(base, simp_user_cost(s), (size_t)n * sizeof(float));

    DefGame *g = def_create(s, CAP);
    add_turret(g, 29.0f, 26.0f, 14.0f, 5000.0f);    /* dischi sovrapposti */
    add_turret(g, 33.0f, 26.0f, 14.0f, 5000.0f);
    simp_terrain_commit(s);
    def_set_fire_lure(g, -0.5f, 6.0f, 2.5f);

    for (int i = 0; i < 24; i++) {                  /* bersagli in range */
        float x = 24.0f + (float)(i % 6) * 0.8f, y = 20.0f + (float)(i / 6) * 0.8f;
        if (simp_free_at(s, x, y, 0.34f)) simp_spawn(s, x, y);
    }
    int fired_seen = 0, lure_seen = 0;
    for (int k = 0; k < (int)(4.0f / DT); k++) {
        simp_step(s, DT);
        def_update(g, DT);
        if (def_shots(g) > 0) fired_seen = 1;
        if (memcmp(base, simp_user_cost(s), (size_t)n * sizeof(float)) != 0)
            lure_seen = 1;                          /* stamp attivo */
    }
    for (int i = simp_count(s) - 1; i >= 0; i--) simp_kill(s, i);
    for (int k = 0; k < (int)(4.0f / DT); k++) {    /* silenzio > linger 2.5 */
        simp_step(s, DT);
        def_update(g, DT);
    }
    int restored = memcmp(base, simp_user_cost(s), (size_t)n * sizeof(float)) == 0;
    int ok = fired_seen && lure_seen && restored;
    printf("removal: fired=%d stamped=%d restored=%d | %s\n",
           fired_seen, lure_seen, restored, ok ? "ok" : "BAD");
    free(base); def_destroy(g); simp_destroy(s);
    return ok;
}

int main(void) {
    float hp_off, hp_on, hp_on2;
    int k_off, k_on, k_on2;
    corridor_run(0.0f, 0.0f, &hp_off, &k_off);
    double c1 = corridor_run(-0.8f, 0.0f, &hp_on, &k_on);
    double c2 = corridor_run(-0.8f, 0.0f, &hp_on2, &k_on2);

    int ok1 = hp_off > 240.0f && k_off > 0;         /* impunita senza lure   */
    int ok2 = hp_on < 120.0f && hp_off - hp_on > 100.0f;  /* mobbata col lure */
    int ok3 = c1 == c2 && hp_on == hp_on2 && k_on == k_on2 && c1 < 1e299;
    printf("corridor: off hp=%.0f kills=%d | on hp=%.0f kills=%d | %s %s\n",
           (double)hp_off, k_off, (double)hp_on, k_on,
           ok1 ? "off-ok" : "off-BAD", ok2 ? "on-ok" : "on-BAD");
    printf("determinism: %s\n", ok3 ? "ok" : "BAD");

    /* interazione col sangue (k_danger di gioco 400): la paura del sangue si
     * accumula sui punti-morte e contrasta il richiamo. MISURA informativa +
     * assert debole: il lure deve comunque mordere piu' del caso spento. */
    float hp_bld;
    int k_bld;
    corridor_run(-0.8f, 400.0f, &hp_bld, &k_bld);
    int ok4 = hp_bld < hp_off - 30.0f;
    printf("blood interplay: on+danger hp=%.0f kills=%d (off=%.0f, on-clean=%.0f) | %s\n",
           (double)hp_bld, k_bld, (double)hp_off, (double)hp_on, ok4 ? "ok" : "BAD");
    ok3 = ok3 && ok4;

    int ok = ok1 && ok2 && ok3 && exact_removal();
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

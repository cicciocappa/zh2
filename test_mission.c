/* test_mission.c — mission state machine verification (GAME_PLAN fase A).
 *
 *   1. SURVIVE vinta: PREP a tempo (exit muti, piazzamento aperto), ASSAULT
 *      col delay dell'exit rispettato, WON allo scadere del timer col core
 *      in piedi; budget della scena applicato.
 *   2. SURVIVE persa: core giu' (def_struct_damage) prima del timer -> LOST,
 *      stato terminale.
 *   3. CLEAR vinta: PREP illimitata (esce solo con mission_go), pool ESATTI
 *      (emitted == somma pool, mai oltre), ASSAULT resta finche' c'e' un
 *      vivo, WON a mappa pulita; i dormienti non bloccano la vittoria.
 *   4. Determinismo: due run dello scenario CLEAR bit-identiche + no NaN.
 */
#include "mission.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DT (1.0f / 60.0f)
#define MAXA 4096

static void survive_scene(Scene *sc) {
    memset(sc, 0, sizeof *sc);
    sc->cell = 0.5f; sc->world_w = 40.0f; sc->world_h = 30.0f;
    sc->gw = 80; sc->gh = 60;
    sc->n_goal = 1; sc->goal[0] = (SceneRect){ 36, 13, 2, 4, 0 };
    sc->mission = (SceneMission){ SCENE_MISSION_SURVIVE, 20.0f, 4.0f, 300.0f };
    sc->n_exit = 1;
    sc->exits[0] = (SceneExit){ 1, 10, 2, 10, 10.0f, 2.0f, 0 };  /* delay 2, inf */
}

static int scenario_survive_won(void) {
    Scene sc; survive_scene(&sc);
    SimP *s = scene_instantiate(&sc, MAXA);
    DefGame *g = def_create(s, MAXA);
    Mission m;
    int ok = mission_create(&m, &sc, s, g, NULL, NULL) == 0;
    ok = ok && m.state == MISSION_PREP && mission_placement_open(&m) &&
         def_budget(g) == 300;

    int prep_silent = 1, delay_silent = 1;
    float t = 0.0f;
    int steps_max = (int)((4.0f + 20.0f + 3.0f) / DT);
    for (int k = 0; k < steps_max && m.state != MISSION_WON; k++) {
        simp_step(s, DT);
        mission_update(&m, DT);
        t += DT;
        if (m.state == MISSION_PREP && simp_count(s) > 0) prep_silent = 0;
        if (m.state == MISSION_ASSAULT && m.t_state < 1.9f && simp_count(s) > 0)
            delay_silent = 0;                       /* exit delay 2 s honored */
        if (m.state == MISSION_ASSAULT && !mission_placement_open(&m))
            ok = 0;                       /* LOOP_DESIGN C: build during assault */
        if (t < 3.9f && m.state != MISSION_PREP) ok = 0;   /* prep 4 s holds  */
    }
    ok = ok && m.state == MISSION_WON && prep_silent && delay_silent &&
         mission_emitted(&m) > 0 && !mission_placement_open(&m);
    printf("survive won: t=%.1f emitted=%d state=%d | %s\n",
           (double)t, mission_emitted(&m), m.state, ok ? "ok" : "BAD");
    mission_destroy(&m); def_destroy(g); simp_destroy(s);
    return ok;
}

static int scenario_survive_lost(void) {
    Scene sc; survive_scene(&sc);
    SimP *s = scene_instantiate(&sc, MAXA);
    DefGame *g = def_create(s, MAXA);
    int core = def_add_structure(g, 200.0f, 1);     /* is_core: fall = loss  */
    Mission m;
    int ok = mission_create(&m, &sc, s, g, NULL, NULL) == 0;

    float t = 0.0f;
    for (int k = 0; k < (int)(10.0f / DT); k++) {
        simp_step(s, DT);
        if (t > 6.0f) def_struct_damage(g, core, 50.0f);   /* core chipped   */
        mission_update(&m, DT);
        t += DT;
        if (m.state == MISSION_LOST) break;
    }
    ok = ok && m.state == MISSION_LOST && def_lost(g) && t < 9.0f;
    for (int k = 0; k < 60; k++) mission_update(&m, DT);   /* terminal stays */
    ok = ok && m.state == MISSION_LOST;
    printf("survive lost: t=%.1f | %s\n", (double)t, ok ? "ok" : "BAD");
    mission_destroy(&m); def_destroy(g); simp_destroy(s);
    return ok;
}

static void clear_scene(Scene *sc) {
    memset(sc, 0, sizeof *sc);
    sc->cell = 0.5f; sc->world_w = 40.0f; sc->world_h = 30.0f;
    sc->gw = 80; sc->gh = 60;
    sc->n_goal = 1; sc->goal[0] = (SceneRect){ 36, 13, 2, 4, 0 };
    sc->n_pack = 1; sc->pack[0] = (SceneRect){ 30, 25, 3, 3, 0 };  /* dormienti */
    sc->mission = (SceneMission){ SCENE_MISSION_CLEAR, 0.0f, 0.0f, -1.0f };
    sc->n_exit = 2;
    sc->exits[0] = (SceneExit){ 1, 5, 2, 8, 15.0f, 0.0f, 30 };
    sc->exits[1] = (SceneExit){ 1, 18, 2, 8, 15.0f, 1.0f, 20 };
}

/* runs the CLEAR scenario; if trace != NULL writes emitted-per-step there
 * (cap steps), returns final agent xy checksum in *chk. */
static int scenario_clear(int *trace, int trace_cap, double *chk) {
    Scene sc; clear_scene(&sc);
    SimP *s = scene_instantiate(&sc, MAXA);
    DefGame *g = def_create(s, MAXA);
    Mission m;
    int ok = mission_create(&m, &sc, s, g, NULL, NULL) == 0;
    int dormant0 = simp_count(s);
    ok = ok && dormant0 > 0;                        /* pack piazzato         */

    for (int k = 0; k < (int)(6.0f / DT); k++) {    /* PREP illimitata tiene */
        simp_step(s, DT); mission_update(&m, DT);
    }
    ok = ok && m.state == MISSION_PREP && simp_count(s) == dormant0;
    mission_go(&m);
    ok = ok && m.state == MISSION_ASSAULT;

    int step = 0, over_pool = 0;
    for (; step < (int)(30.0f / DT) && m.state == MISSION_ASSAULT; step++) {
        simp_step(s, DT);
        mission_update(&m, DT);
        if (mission_emitted(&m) > 50) over_pool = 1;
        if (trace && step < trace_cap) trace[step] = mission_emitted(&m);
        if (mission_emitted(&m) == 50) {
            int alldone = 1;
            for (int i = 0; i < m.ndir; i++)
                if (!def_director_done(m.dir[i])) alldone = 0;
            if (alldone) break;
        }
    }
    ok = ok && !over_pool && mission_emitted(&m) == 50 && mission_pool(&m) == 50;
    /* ancora vivi -> niente vittoria */
    for (int k = 0; k < 30; k++) { simp_step(s, DT); mission_update(&m, DT); }
    ok = ok && m.state == MISSION_ASSAULT;
    /* pulizia: uccidi tutti i NON dormienti (indice decrescente, M3.4) */
    const uint8_t *fl = simp_flags_arr(s);
    for (int i = simp_count(s) - 1; i >= 0; i--)
        if (!(fl[i] & SIMP_DORMANT)) simp_kill(s, i);
    simp_step(s, DT);
    mission_update(&m, DT);
    ok = ok && m.state == MISSION_WON && simp_count(s) > 0;  /* dormienti restano */

    /* no NaN + checksum posizioni per il determinismo */
    const float *px = simp_px(s), *py = simp_py(s);
    double sum = 0.0;
    for (int i = 0; i < simp_count(s); i++) {
        if (!isfinite(px[i]) || !isfinite(py[i])) ok = 0;
        sum += (double)px[i] * 1.000001 + (double)py[i];
    }
    if (chk) *chk = sum;
    printf("clear: emitted=%d/%d dormant=%d state=%d | %s\n",
           mission_emitted(&m), mission_pool(&m), simp_count(s), m.state,
           ok ? "ok" : "BAD");
    mission_destroy(&m); def_destroy(g); simp_destroy(s);
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= scenario_survive_won();
    ok &= scenario_survive_lost();

    enum { TCAP = 2048 };
    static int tr1[TCAP], tr2[TCAP];
    double c1 = 0, c2 = 0;
    ok &= scenario_clear(tr1, TCAP, &c1);
    ok &= scenario_clear(tr2, TCAP, &c2);
    int det = c1 == c2;
    for (int i = 0; i < TCAP && det; i++) if (tr1[i] != tr2[i]) det = 0;
    printf("determinism: chk %.3f vs %.3f | %s\n", c1, c2, det ? "ok" : "BAD");
    ok &= det;

    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

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
 *   5. Ondate scriptate (LOOP_DESIGN A): annuncio con countdown e info esatte,
 *      exit MUTE in pausa, conteggi per ondata esatti (mai oltre il pool),
 *      gruppo multi-exit simultaneo, mix scriptato (tank 100 -> 10 tank
 *      esatti), "chiama la prossima" (secondi risparmiati > 0, poi 0 se gia'
 *      attiva), CLEAR = ondate finite + mappa pulita; determinismo (due run
 *      bit-identiche).
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

/* ---- ondate scriptate (LOOP_DESIGN A) ---- */

static int gTanks;      /* BT_TANK spawned, tallied via the director callback */
static void wave_spawn_cb(void *user, SimPHandle h, DefBody body, unsigned roll) {
    (void)user; (void)h; (void)roll;
    if (body == BT_TANK) gTanks++;
}

static void wave_scene(Scene *sc) {
    memset(sc, 0, sizeof *sc);
    sc->cell = 0.5f; sc->world_w = 40.0f; sc->world_h = 30.0f;
    sc->gw = 80; sc->gh = 60;
    sc->n_goal = 1; sc->goal[0] = (SceneRect){ 36, 13, 2, 4, 0 };
    sc->mission = (SceneMission){ SCENE_MISSION_CLEAR, 0.0f, 0.0f, -1.0f };
    sc->n_exit = 2;                       /* rate/delay/pool ignored with waves */
    sc->exits[0] = (SceneExit){ 1, 5, 2, 8, 0.0f, 0.0f, 0 };
    sc->exits[1] = (SceneExit){ 1, 18, 2, 8, 0.0f, 0.0f, 0 };
    sc->n_wave = 3;                       /* mix pinned: tank count is exact  */
    sc->waves[0] = (SceneWave){ 1, 0, 20, 15.0f, 0, 0 };
    sc->waves[1] = (SceneWave){ 2, 0, 10, 15.0f, 100, -1 };  /* all tanks     */
    sc->waves[2] = (SceneWave){ 2, 1, 10, 15.0f, 0, 0 };     /* simultaneous  */
}

static int scenario_waves(double *chk) {
    Scene sc; wave_scene(&sc);
    SimP *s = scene_instantiate(&sc, MAXA);
    DefGame *g = def_create(s, MAXA);
    Mission m;
    gTanks = 0;
    int ok = mission_create(&m, &sc, s, g, wave_spawn_cb, NULL) == 0;
    m.wave_pause = 3.0f;
    ok = ok && mission_wave_total(&m) == 2 && mission_wave_current(&m) == 0 &&
         !mission_wave_pending(&m, NULL) && mission_pool(&m) == 40 &&
         mission_call_next(&m) == 0.0f;             /* niente da chiamare in PREP */

    mission_go(&m);
    MissionWaveInfo wi;
    ok = ok && m.state == MISSION_ASSAULT && mission_wave_current(&m) == 1 &&
         mission_wave_pending(&m, &wi) &&
         wi.index == 1 && wi.total == 2 && wi.n == 1 &&
         wi.e[0].exit_idx == 0 && wi.e[0].count == 20 &&
         wi.countdown > 2.9f && wi.countdown <= 3.0f;

    /* pausa d'annuncio: exit mute per tutta la finestra */
    for (int k = 0; k < (int)(2.5f / DT); k++) { simp_step(s, DT); mission_update(&m, DT); }
    ok = ok && mission_emitted(&m) == 0 && mission_wave_pending(&m, NULL);

    /* l'ondata 1 parte da sola allo scadere; emette ESATTAMENTE 20 */
    int over = 0, step = 0;
    for (; step < (int)(20.0f / DT); step++) {
        simp_step(s, DT); mission_update(&m, DT);
        if (mission_emitted(&m) > 20) over = 1;
        if (mission_wave_pending(&m, &wi) && wi.index == 2) break;  /* annuncio 2 */
    }
    ok = ok && !over && mission_emitted(&m) == 20 && mission_wave_current(&m) == 2 &&
         wi.index == 2 && wi.n == 2 &&
         wi.e[0].tank_pct == 100 && wi.e[1].exit_idx == 1;

    /* chiama la prossima: secondi risparmiati > 0, poi 0 (gia' attiva) */
    for (int k = 0; k < 30; k++) { simp_step(s, DT); mission_update(&m, DT); }
    float saved = mission_call_next(&m);
    ok = ok && saved > 2.0f && saved < 3.0f;        /* ~3 s meno i 30 step    */
    ok = ok && mission_call_next(&m) == 0.0f && !mission_wave_pending(&m, NULL);

    /* l'ondata 2 (due exit simultanee) completa il pool: 40 esatti */
    for (int k = 0; k < (int)(20.0f / DT); k++) {
        simp_step(s, DT); mission_update(&m, DT);
        if (mission_emitted(&m) > 40) over = 1;
        if (mission_wave_current(&m) > mission_wave_total(&m)) break;
    }
    ok = ok && !over && mission_emitted(&m) == 40 && gTanks == 10 &&
         mission_wave_current(&m) == 3 && !mission_wave_pending(&m, NULL) &&
         mission_call_next(&m) == 0.0f;

    /* vivi in giro -> niente vittoria; mappa pulita -> WON */
    for (int k = 0; k < 30; k++) { simp_step(s, DT); mission_update(&m, DT); }
    ok = ok && m.state == MISSION_ASSAULT;
    for (int i = simp_count(s) - 1; i >= 0; i--) simp_kill(s, i);
    simp_step(s, DT); mission_update(&m, DT);
    ok = ok && m.state == MISSION_WON;

    const float *px = simp_px(s), *py = simp_py(s);
    double sum = 0.0;
    for (int i = 0; i < simp_count(s); i++) {
        if (!isfinite(px[i]) || !isfinite(py[i])) ok = 0;
        sum += (double)px[i] * 1.000001 + (double)py[i];
    }
    if (chk) *chk = sum + gTanks;
    printf("waves: emitted=%d/%d tanks=%d saved=%.2f state=%d | %s\n",
           mission_emitted(&m), mission_pool(&m), gTanks, (double)saved,
           m.state, ok ? "ok" : "BAD");
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

    double w1 = 0, w2 = 0;                 /* ondate scriptate (LOOP_DESIGN A) */
    ok &= scenario_waves(&w1);
    ok &= scenario_waves(&w2);
    int wdet = w1 == w2;
    printf("waves determinism: chk %.3f vs %.3f | %s\n", w1, w2, wdet ? "ok" : "BAD");
    ok &= wdet;

    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

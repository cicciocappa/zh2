/* mission.c — mission state machine. See mission.h / GAME_PLAN fase A. */

#include "mission.h"
#include <stdio.h>
#include <string.h>

int mission_create(Mission *m, const Scene *sc, SimP *s, DefGame *g,
                   DefSpawnFn on_spawn, void *user) {
    memset(m, 0, sizeof *m);
    if (sc->mission.kind == SCENE_MISSION_NONE) return -1;   /* legacy scene */

    m->kind = sc->mission.kind;
    m->survive_s = sc->mission.survive_s;
    m->prep_s = sc->mission.prep_s > 0.0f ? sc->mission.prep_s : 0.0f;
    m->state = MISSION_PREP;
    m->s = s; m->g = g;
    m->on_spawn = on_spawn; m->spawn_user = user;
    m->wave_pause = 15.0f;                 /* host overrides (balance.cfg)   */

    if (sc->mission.budget >= 0.0f)
        def_set_budget(g, (int)sc->mission.budget);

    m->n_exit = sc->n_exit < SCENE_MAX_RECT ? sc->n_exit : SCENE_MAX_RECT;
    for (int i = 0; i < m->n_exit; i++) {
        const SceneExit *e = &sc->exits[i];
        m->exit_rect[i] = (DefRect){ e->x, e->y, e->w, e->h };
    }

    if (sc->n_wave > 0) {                  /* scripted waves (LOOP_DESIGN A) */
        for (int i = 0; i < sc->n_wave && m->n_wv < SCENE_MAX_WAVE; i++) {
            if (sc->waves[i].exit_idx >= m->n_exit || sc->waves[i].count < 1) {
                fprintf(stderr, "mission: wave %d con exit %d inesistente — "
                        "scartata\n", sc->waves[i].wave, sc->waves[i].exit_idx);
                continue;
            }
            m->wv[m->n_wv++] = sc->waves[i];
        }
        /* stable insertion sort by group number (file order within a group) */
        for (int i = 1; i < m->n_wv; i++) {
            SceneWave w = m->wv[i]; int j = i - 1;
            while (j >= 0 && m->wv[j].wave > w.wave) { m->wv[j+1] = m->wv[j]; j--; }
            m->wv[j+1] = w;
        }
        for (int i = 0; i < m->n_wv; ) {   /* group boundaries               */
            int j = i;
            while (j < m->n_wv && m->wv[j].wave == m->wv[i].wave) j++;
            m->wv_first[m->n_group] = i; m->wv_n[m->n_group] = j - i;
            m->n_group++; i = j;
        }
        for (int i = 0; i < m->n_wv; i++) m->pool_total += m->wv[i].count;
        if (m->n_group == 0)
            fprintf(stderr, "mission: righe wave presenti ma nessuna valida — "
                    "assalto muto, sistemare la scena\n");
        return 0;                          /* directors are made per wave    */
    }

    int unlimited = 0;                     /* legacy: one director per exit  */
    for (int i = 0; i < sc->n_exit && m->ndir < SCENE_MAX_RECT; i++) {
        const SceneExit *e = &sc->exits[i];
        DefRect r = { e->x, e->y, e->w, e->h };
        DefDirectorCfg dc = {0};
        dc.rects = &r; dc.nrects = 1;                /* one director per exit */
        dc.spawn_radius = 0.34f;
        dc.base_rate = e->rate;
        dc.rate_ramp = 0.0f;                         /* scripted: flat rate   */
        dc.start_delay = e->delay;
        dc.pool = e->pool;
        dc.seed = 0x5EED0000u + (uint32_t)i * 0x9E3779B9u;   /* per-exit, deterministic */
        dc.on_spawn = on_spawn; dc.user = user;
        m->dir[m->ndir++] = def_director_create(g, &dc);
        if (e->pool > 0) m->pool_total += e->pool; else unlimited = 1;
    }
    if (unlimited) m->pool_total = 0;
    if (m->kind == SCENE_MISSION_CLEAR && (unlimited || m->ndir == 0))
        fprintf(stderr, "mission: CLEAR con exit illimitate o assenti — "
                "non potra' mai essere vinta, sistemare la scena\n");
    return 0;
}

void mission_destroy(Mission *m) {
    for (int i = 0; i < m->ndir; i++) def_director_destroy(m->dir[i]);
    memset(m, 0, sizeof *m);
}

/* Fire the current wave group NOW: retire the previous group's directors and
 * build one per entry (finite pool = count, no delay). Deterministic seeds
 * from (group, entry). */
static void wave_start(Mission *m) {
    for (int i = 0; i < m->ndir; i++) {
        m->emitted_done += def_director_emitted(m->dir[i]);
        def_director_destroy(m->dir[i]);
    }
    m->ndir = 0;
    const DefTuning *t = def_tuning(m->g);
    int first = m->wv_first[m->group_cur], n = m->wv_n[m->group_cur];
    for (int k = 0; k < n && m->ndir < SCENE_MAX_RECT; k++) {
        const SceneWave *w = &m->wv[first + k];
        DefDirectorCfg dc = {0};
        dc.rects = &m->exit_rect[w->exit_idx]; dc.nrects = 1;
        dc.spawn_radius = 0.34f;
        dc.base_rate = w->rate;
        dc.pool = w->count;
        dc.seed = 0x5EED0000u
                + (uint32_t)(m->group_cur * 61 + k) * 0x9E3779B9u;
        dc.on_spawn = m->on_spawn; dc.user = m->spawn_user;
        if (w->tank_pct >= 0 || w->obese_pct >= 0) {   /* scripted mix; the  */
            dc.mix_override = 1;                       /* omitted half keeps */
            dc.tank_pct  = w->tank_pct  >= 0 ? w->tank_pct  : t->mix_tank_base;
            dc.obese_pct = w->obese_pct >= 0 ? w->obese_pct : t->mix_obese_base;
        }
        m->dir[m->ndir++] = def_director_create(m->g, &dc);
    }
    m->wv_active = 1; m->pause_left = 0.0f;
}

void mission_go(Mission *m) {
    if (m->state != MISSION_PREP) return;
    m->state = MISSION_ASSAULT; m->t_state = 0.0f;
    if (m->n_group > 0) {                  /* announce wave 1 */
        m->group_cur = 0; m->wv_active = 0; m->pause_left = m->wave_pause;
    }
}

/* live agents that count for CLEAR: everything not dormant (a sleeping pack
 * out of the way is scenery; a woken one lost its flag and counts). */
static int live_awake(const SimP *s) {
    int n = simp_count(s), alive = 0;
    const uint8_t *fl = simp_flags_arr(s);
    for (int i = 0; i < n; i++)
        if (!(fl[i] & SIMP_DORMANT)) alive++;
    return alive;
}

void mission_update(Mission *m, float dt) {
    if (m->state == MISSION_WON || m->state == MISSION_LOST) return;
    m->t_state += dt;

    if (def_lost(m->g)) { m->state = MISSION_LOST; m->t_state = 0.0f; return; }

    if (m->state == MISSION_PREP) {
        if (m->prep_s > 0.0f && m->t_state >= m->prep_s)
            mission_go(m);                 /* same entry as the player's go  */
        return;                                      /* exits silent in PREP */
    }

    /* ASSAULT */
    if (m->n_group > 0 && m->group_cur < m->n_group) {   /* scripted waves  */
        if (!m->wv_active) {                             /* announcement    */
            m->pause_left -= dt;
            if (m->pause_left <= 0.0f) wave_start(m);
        }
        if (m->wv_active) {
            int done = 1;
            for (int i = 0; i < m->ndir; i++) {
                def_director_update(m->dir[i], dt);
                if (!def_director_done(m->dir[i])) done = 0;
            }
            if (done) {                    /* pool out -> announce the next  */
                m->group_cur++; m->wv_active = 0; m->pause_left = m->wave_pause;
            }
        }
    } else if (m->n_group == 0)
        for (int i = 0; i < m->ndir; i++) def_director_update(m->dir[i], dt);

    if (m->kind == SCENE_MISSION_SURVIVE) {
        if (m->t_state >= m->survive_s) { m->state = MISSION_WON; m->t_state = 0.0f; }
    } else if (m->n_group > 0) {                     /* CLEAR, waves */
        if (m->group_cur >= m->n_group && live_awake(m->s) == 0)
            { m->state = MISSION_WON; m->t_state = 0.0f; }
    } else {                                         /* CLEAR, legacy */
        int done = 1;
        for (int i = 0; i < m->ndir; i++)
            if (!def_director_done(m->dir[i])) { done = 0; break; }
        if (done && m->ndir > 0 && live_awake(m->s) == 0)
            { m->state = MISSION_WON; m->t_state = 0.0f; }
    }
}

MissionState mission_state(const Mission *m) { return m->state; }

float mission_time_left(const Mission *m) {
    if (m->state == MISSION_PREP && m->prep_s > 0.0f)
        return m->prep_s - m->t_state;
    if (m->state == MISSION_ASSAULT && m->kind == SCENE_MISSION_SURVIVE)
        return m->survive_s - m->t_state;
    return -1.0f;
}

int mission_emitted(const Mission *m) {
    int n = m->emitted_done;               /* retired wave groups */
    for (int i = 0; i < m->ndir; i++) n += def_director_emitted(m->dir[i]);
    return n;
}

int mission_pool(const Mission *m) { return m->pool_total; }

/* LOOP_DESIGN C (2026-07-16): building is allowed DURING the assault too —
 * the host swaps the placement wallet to biomass (pl_set_wallet) and offers
 * the field subset of the catalog. Terminal states stay closed. */
int mission_placement_open(const Mission *m) {
    return m->state == MISSION_PREP || m->state == MISSION_ASSAULT;
}

/* ---- scripted waves (LOOP_DESIGN A) ---- */

int mission_wave_total(const Mission *m) { return m->n_group; }

int mission_wave_current(const Mission *m) {
    if (m->n_group == 0 || m->state == MISSION_PREP) return 0;
    return m->group_cur + 1;               /* total+1 = all emitted (mop-up) */
}

int mission_wave_pending(const Mission *m, MissionWaveInfo *out) {
    if (m->state != MISSION_ASSAULT || m->n_group == 0) return 0;
    if (m->wv_active || m->group_cur >= m->n_group) return 0;
    if (out) {
        memset(out, 0, sizeof *out);
        out->index = m->group_cur + 1; out->total = m->n_group;
        out->countdown = m->pause_left > 0.0f ? m->pause_left : 0.0f;
        int first = m->wv_first[m->group_cur], n = m->wv_n[m->group_cur];
        for (int k = 0; k < n && out->n < MISSION_WAVE_INFO_ENTRIES; k++) {
            const SceneWave *w = &m->wv[first + k];
            out->e[out->n].exit_idx  = w->exit_idx;
            out->e[out->n].count     = w->count;
            out->e[out->n].tank_pct  = w->tank_pct;
            out->e[out->n].obese_pct = w->obese_pct;
            out->e[out->n].rate      = w->rate;
            out->n++;
        }
    }
    return 1;
}

float mission_call_next(Mission *m) {
    if (!mission_wave_pending(m, NULL)) return 0.0f;
    float saved = m->pause_left > 0.0f ? m->pause_left : 0.0f;
    wave_start(m);
    return saved;
}

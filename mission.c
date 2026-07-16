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

    if (sc->mission.budget >= 0.0f)
        def_set_budget(g, (int)sc->mission.budget);

    int unlimited = 0;
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

void mission_go(Mission *m) {
    if (m->state == MISSION_PREP) { m->state = MISSION_ASSAULT; m->t_state = 0.0f; }
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
            { m->state = MISSION_ASSAULT; m->t_state = 0.0f; }
        return;                                      /* exits silent in PREP */
    }

    /* ASSAULT */
    for (int i = 0; i < m->ndir; i++) def_director_update(m->dir[i], dt);

    if (m->kind == SCENE_MISSION_SURVIVE) {
        if (m->t_state >= m->survive_s) { m->state = MISSION_WON; m->t_state = 0.0f; }
    } else {                                         /* CLEAR */
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
    int n = 0;
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

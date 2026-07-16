/* mission.h — mission state machine (GAME_PLAN fase A).
 *
 * Game-side glue ABOVE defense: turns a demo into a MATCH. Owns one spawn
 * director per scripted `exit` of the scene (defense §8, with start_delay +
 * finite pool), gates their emission to the ASSAULT state, and decides
 * WIN/LOSE:
 *
 *   PREP    — the sim runs (world is alive) but exits are silent. Ends after
 *             prep_s seconds, or on mission_go() when prep_s == 0 (unlimited
 *             fortification, the player launches the assault).
 *   ASSAULT — directors emit. SURVIVE wins when survive_s elapse with the
 *             core standing; CLEAR wins when every director has drained its
 *             pool AND no live non-dormant agent remains. def_lost (core
 *             collapsed) loses in any state.
 *   WON / LOST — terminal; directors stop.
 *
 * SCRIPTED WAVES (LOOP_DESIGN A). If the scene declares `wave` lines the
 * assault runs as a SEQUENCE OF ANNOUNCED WAVES instead of the continuous
 * tap: each wave group (entries sharing a wave number, possibly on several
 * exits at once) is preceded by an announcement PAUSE of wave_pause seconds
 * (the player's decision window — mission_wave_pending feeds the HUD banner);
 * mission_call_next skips the pause and returns the seconds saved, which the
 * host converts to the biomass bonus (Kingdom Rush style). A wave ends when
 * its whole pool has been emitted; then the next pause starts. With waves the
 * `exit` lines become pure spawn rects (their rate/delay/pool are ignored)
 * and CLEAR is finite by construction. Scenes without waves keep the legacy
 * per-exit directors, bit-identical.
 *
 * The mission does NOT call def_update/simp_step: the host keeps its loop and
 * calls mission_update once per fixed step after them. Deterministic: per-exit
 * director seeds derived from the exit index. Zero core APIs (GAME_PLAN §
 * "regola invariante").
 *
 * A scene without a `mission` line (kind NONE) predates the machine: the host
 * should keep its legacy behavior (endless demo / AppLevel survive timer);
 * mission_create returns -1 in that case (nothing allocated).
 */
#ifndef MISSION_H
#define MISSION_H

#include "scene.h"
#include "defense.h"

typedef enum {
    MISSION_PREP = 0, MISSION_ASSAULT, MISSION_WON, MISSION_LOST
} MissionState;

/* HUD snapshot of the ANNOUNCED wave (valid while mission_wave_pending
 * returns 1): group index, countdown, and up to a few entries to summarize
 * ("40 da NORD-EST, tank 10%" — direction comes from the exit rect). */
#define MISSION_WAVE_INFO_ENTRIES 8
typedef struct {
    int   index, total;          /* 1-based wave number / total waves        */
    float countdown;             /* s until it fires (pause left)            */
    int   n;                     /* entries (clamped to the array)           */
    struct { int exit_idx, count, tank_pct, obese_pct; float rate; }
          e[MISSION_WAVE_INFO_ENTRIES];
} MissionWaveInfo;

typedef struct {
    SceneMissionKind kind;       /* SURVIVE | CLEAR                          */
    float survive_s;             /* SURVIVE: win after this long in ASSAULT  */
    float prep_s;                /* 0 = unlimited PREP (exit via mission_go) */
    MissionState state;
    float t_state;               /* seconds spent in the current state       */
    SimP *s; DefGame *g;
    DefDirector *dir[SCENE_MAX_RECT]; int ndir;   /* one per scene exit, or
                                     one per entry of the CURRENT wave group */
    int pool_total;              /* sum of pools (0 if any exit unlimited)   */
    /* scripted waves (LOOP_DESIGN A; n_group == 0 = legacy continuous tap) */
    SceneWave wv[SCENE_MAX_WAVE]; int n_wv;       /* sorted by group number  */
    int   wv_first[SCENE_MAX_WAVE], wv_n[SCENE_MAX_WAVE]; int n_group;
    DefRect exit_rect[SCENE_MAX_RECT]; int n_exit;/* wave spawn rects        */
    DefSpawnFn on_spawn; void *spawn_user;        /* kept for wave directors */
    int   group_cur;             /* group announced/emitting; == n_group = done */
    int   wv_active;             /* 0 = announcement pause, 1 = emitting     */
    float pause_left;            /* s left in the announcement pause         */
    float wave_pause;            /* pause length (host sets it; default 15)  */
    int   emitted_done;          /* emitted by already-destroyed wave groups */
} Mission;

/* Build the mission from the scene: one director per `exit` (spawn cb passed
 * through for renderer tagging), budget applied if declared. Returns 0, or -1
 * if the scene declares no mission (legacy: m is zeroed, use the old path).
 * CLEAR with an unlimited exit (pool 0) is a design error: warned on stderr
 * (the mission can never be won). */
int  mission_create(Mission *m, const Scene *sc, SimP *s, DefGame *g,
                    DefSpawnFn on_spawn, void *user);
void mission_destroy(Mission *m);

/* PREP -> ASSAULT now (player's "go"; no-op outside PREP). */
void mission_go(Mission *m);

/* Advance timers, run the directors (ASSAULT only), settle WIN/LOSE.
 * Call once per fixed step, after def_update. */
void mission_update(Mission *m, float dt);

/* ---- read access (HUD / tests) ---- */
MissionState mission_state(const Mission *m);
/* Seconds left in the current phase: PREP countdown, or SURVIVE countdown.
 * < 0 = no deadline (unlimited PREP, CLEAR assault, terminal states). */
float mission_time_left(const Mission *m);
int   mission_emitted(const Mission *m);    /* total spawned by all exits    */
int   mission_pool(const Mission *m);       /* pool_total (0 = unlimited)    */
int   mission_placement_open(const Mission *m);  /* 1 = building allowed
                                     (PREP + ASSAULT since LOOP_DESIGN C;
                                     the host picks currency and subset) */

/* ---- scripted waves (LOOP_DESIGN A) ---- */
int   mission_wave_total(const Mission *m);   /* wave groups (0 = legacy tap) */
/* 1-based group currently announced or emitting; 0 before the assault,
 * total+1 when every wave has been emitted (mop-up / survive tail). */
int   mission_wave_current(const Mission *m);
/* 1 = a wave is ANNOUNCED (assault, pause running): fills *out for the HUD. */
int   mission_wave_pending(const Mission *m, MissionWaveInfo *out);
/* Skip the announcement pause ("chiama la prossima"): the wave fires NOW.
 * Returns the seconds saved (the host converts them to the biomass bonus);
 * 0 if no wave is pending. */
float mission_call_next(Mission *m);

#endif /* MISSION_H */

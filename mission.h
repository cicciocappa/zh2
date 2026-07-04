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

typedef struct {
    SceneMissionKind kind;       /* SURVIVE | CLEAR                          */
    float survive_s;             /* SURVIVE: win after this long in ASSAULT  */
    float prep_s;                /* 0 = unlimited PREP (exit via mission_go) */
    MissionState state;
    float t_state;               /* seconds spent in the current state       */
    SimP *s; DefGame *g;
    DefDirector *dir[SCENE_MAX_RECT]; int ndir;   /* one per scene exit      */
    int pool_total;              /* sum of pools (0 if any exit unlimited)   */
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
int   mission_placement_open(const Mission *m);  /* 1 = building allowed (PREP) */

#endif /* MISSION_H */

/* destruct.h — destructible decor props (DESTRUCT_DESIGN.md).
 *
 * The horde shatters tables/chairs into flying debris and topples signs/traffic
 * lights when it reaches them. Game-side module, core-agnostic like defense.c:
 * it owns per-prop RUNTIME state, detects contact via simp_query_circle, and
 * fires a one-shot burst event the host turns into an FX particle burst. The sim
 * core never sees props (pure decor) — this only reads agent positions/velocities.
 *
 * Deterministic: the trigger decisions (which prop falls, when) depend only on
 * the (exact, between steps) spatial query and the per-prop timer. The FX itself
 * is visual (host RNG) and out of scope for deterministic tests.
 */
#ifndef DESTRUCT_H
#define DESTRUCT_H

#include "sim_particles.h"
#include "scene.h"
#include "props.h"

enum {
    DESTRUCT_INERT = 0,   /* not destructible: render normally, never managed */
    DESTRUCT_ALIVE,       /* destructible, intact: render normally, watched   */
    DESTRUCT_TOPPLING,    /* knocked-down animation in progress: render tilted */
    DESTRUCT_GONE         /* destroyed: not rendered                          */
};

/* Fired once when a prop bursts into pieces (after any topple). dir = world XZ
 * heading (rad) the pieces should fly, captured from the crowd's push at contact. */
typedef void (*DestructBurstFn)(int prop_index, const char *debris,
                                float x, float y, float dir, void *ud);

typedef struct {
    uint8_t state[SCENE_MAX_PROP];
    float   timer[SCENE_MAX_PROP];   /* topple countdown (s)                  */
    float   topple_total[SCENE_MAX_PROP];
    float   dir[SCENE_MAX_PROP];     /* captured push heading (rad)           */
    const PropDef *def[SCENE_MAX_PROP]; /* catalog entry, resolved at init
                                          (borrowed: the catalog must outlive
                                          this; re-init after catalog edits)  */
    int     n;                        /* = scene n_prop at init               */
} Destruct;

/* Resolve which props are destructible (via the catalog) and mark them ALIVE.
 * Caches each prop's PropDef so update never does string lookups. */
void destruct_init(Destruct *d, const Scene *sc, const PropCatalog *cat);

/* Advance one step: detect contact on ALIVE props, tick TOPPLING ones, fire
 * on_burst at the burst moment. Call AFTER simp_step. Returns nonzero if any
 * prop changed state this step (the host re-uploads the prop mesh). */
int destruct_update(Destruct *d, const SimP *s, const Scene *sc, float dt,
                    DestructBurstFn on_burst, void *ud);

/* Programmatic burst (EXPLOSION_DESIGN.md §4/§8): shatter prop i NOW, skipping
 * any topple (a blast wave doesn't tip it over slowly). Works on INERT props
 * too — decor caught in a blast bursts generically (empty debris style => the
 * host picks a default preset). No-op if already GONE or i out of range. dir =
 * debris fly heading (rad, e.g. away from the blast center). Fires on_burst once
 * and leaves the prop GONE. */
void destruct_force(Destruct *d, const Scene *sc, int i, float dir,
                    DestructBurstFn on_burst, void *ud);

/* Render helpers (host reads per frame). */
int   destruct_state(const Destruct *d, int i);
float destruct_topple_t(const Destruct *d, int i);   /* 0..1 tilt progress    */
float destruct_dir(const Destruct *d, int i);
int   destruct_animating(const Destruct *d);          /* any TOPPLING right now */

#endif /* DESTRUCT_H */

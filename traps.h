/* traps.h — placed traps (GAME_PLAN fase D). v1: the MINE.
 *
 * Game-side, core-agnostic like defense.c / destruct.c: it talks to the sim ONLY
 * through the public simp_* API (a proximity query), and it does NOT know about
 * explosions or FX. When a trap fires it calls back TrapBlastFn with the blast
 * parameters; the caller decides what "explode" means — the game maps it to
 * host_blast (def_blast + FX), a test maps it to def_blast alone. This keeps the
 * trigger logic pure and deterministically testable, and reuses the explosion
 * primitive built in EXPLOSION_DESIGN (no new core API).
 *
 * A MINE is a one-shot pressure trap: placed in PREP, it arms (after arm_delay),
 * and the first live agent inside its trigger radius detonates it — an AoE blast
 * with FULL friendly fire (the blast hits the player's own barricades/turrets in
 * range, via def_blast). Once fired it is CONSUMED (traps_alive -> 0).
 *
 * Call order per step: simp_step(s, dt)  then  traps_update(...)  (the proximity
 * query needs the collision grid fresh, i.e. right after the step) — same rule
 * as defense/destruct.
 */
#ifndef TRAPS_H
#define TRAPS_H

#include "sim_particles.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRAPS_MAX 256

typedef enum { TRAP_MINE = 0 } TrapKind;

typedef struct {
    TrapKind kind;
    float x, y;          /* placement (m)                                       */
    float trig_r;        /* proximity trigger radius (m): an agent this close fires it */
    float blast_r;       /* explosion radius passed to on_blast (m)             */
    float dmg;           /* explosion peak damage D0                            */
    float strength;      /* impulse strength                                    */
    float up;            /* impulse up_ratio (loft)                             */
    float arm_delay;     /* s before the mine can fire (0 = armed immediately)  */
} TrapDef;

/* Fired when a trap detonates. id = trap id (the host can drop a consumed-mine
 * decal / crater on it). (x,y,r,dmg,strength,up) are the blast parameters —
 * feed them to host_blast (game) or def_blast (test). */
typedef void (*TrapBlastFn)(void *user, int id, float x, float y,
                            float r, float dmg, float strength, float up);

typedef struct {
    TrapDef def[TRAPS_MAX];
    uint8_t live[TRAPS_MAX];   /* 1 = live (armed or arming), 0 = consumed      */
    float   arm_t[TRAPS_MAX];  /* countdown to armed (s), <= 0 = armed          */
    int     n;
} Traps;

void traps_init(Traps *t);
/* Register a trap (copied). Returns its id (>= 0), or -1 if the table is full. */
int  traps_add(Traps *t, const TrapDef *def);
int  traps_count(const Traps *t);
/* 1 while the trap is still live (not yet detonated), 0 once consumed/invalid. */
int  traps_alive(const Traps *t, int id);
/* The trap's placement/params (position, radii…) for rendering/inspection.
 * Borrowed, valid until the next traps_add; NULL if the id is out of range. */
const TrapDef *traps_get(const Traps *t, int id);

/* Remove the most recently added trap (PREP undo, place.c): pops the last id off
 * the table. Returns 1 if it removed a still-LIVE trap (a clean PREP undo), 0 if
 * the table is empty or the last trap was already consumed (LIFO broken — the
 * caller must not treat it as undone). Valid only while placement is the sole
 * mutator (PREP), like defense's remove-LAST primitives. */
int  traps_remove_last(Traps *t);

/* Advance one step: tick arm timers, then for each live ARMED mine detonate if a
 * live agent is within trig_r (fires on_blast, consumes the mine). Deterministic
 * (traps scanned in id order; the query is exact between steps). Call AFTER
 * simp_step. A blast may kill agents a later mine in the same update would have
 * triggered on — chain reactions resolve in id order. */
void traps_update(Traps *t, const SimP *s, float dt,
                  TrapBlastFn on_blast, void *user);

#ifdef __cplusplus
}
#endif
#endif /* TRAPS_H */

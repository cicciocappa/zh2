/* soldier.h — playable soldier (SOLDIER_DESIGN.md, LOOP_DESIGN F).
 *
 * Game-side module over the particle core, zero-dep (sim_particles + libm).
 * The soldier's BODY is a DRAGGABLE disc (not an agent): invisible to
 * turret/AoE queries, to goal drain and to mission live-counts, but it
 * collides with the horde and the walls and absorbs the PBD shove with real
 * momentum — contact knockback comes for free (SOLDIER_DESIGN decisions,
 * 2026-07-16). Driving uses simp_drag_set_vel with a BLEND (accelerate
 * toward the wanted velocity): overwriting the raw velocity every step
 * would erase the shove just absorbed.
 *
 * The module owns: drive, machine-gun timing + hitscan, mobile attraction
 * stamp (negative user cost, delta read-back pattern from def_set_fire_lure),
 * contact damage -> HP -> DOWN state. The HOST owns: input mapping, grenade
 * (bio_take + host_blast), FX, camera, respawn placement, and mapping the
 * shot callback to def_damage_agent.
 *
 * CONSTRAINT (documented, not enforced): the module keys its body by
 * draggable POOL INDEX, which is unstable across simp_drag_remove. Do not
 * remove other draggables while the soldier is deployed (in-game nothing
 * does: bin/car undo is PREP-only and the soldier exists only in assault).
 *
 * Deterministic: no RNG, no allocation in soldier_step.
 */
#ifndef SOLDIER_H
#define SOLDIER_H

#include <stdbool.h>
#include "sim_particles.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float radius;       /* body radius (m)                                  */
    float mass;         /* walker units (PBD shove ratio)                   */
    float speed;        /* wanted walk speed (m/s)                          */
    float accel;        /* drive accel toward wanted velocity (m/s^2);
                           doubles as the knockback recovery rate           */
    float hp_max;
    float touch_dps;    /* HP/s drained PER zombie in contact               */
    float touch_knock;  /* extra push-away accel while touched (m/s^2)      */
    float touch_pad;    /* contact reach beyond summed radii (m)            */
    float gun_range;    /* m                                                */
    float gun_period;   /* s between shots                                  */
    float gun_damage;   /* HP per shot, scaled by ray transmittance         */
    float lure_w;       /* attraction weight (< 0 = on, core clamp -0.8)    */
    float lure_r;       /* attraction radius (m)                            */
    float down_s;       /* respawn lockout after going down (s)             */
} SoldierDef;

void soldier_def_defaults(SoldierDef *d);

typedef struct Soldier Soldier;

/* One callback per trigger pull. (ox,oy)->(ex,ey) is the tracer segment
 * (end = first agent hit, else wall/max-range truncation). hit_index is the
 * DENSE agent index of the victim or -1 on a miss — convert it to a handle
 * (or apply damage) IMMEDIATELY, it dies on the first kill. dmg is already
 * scaled by simp_ray_transmit (fence attenuation, ENTITY_DESIGN §4). */
typedef void (*SoldierShotFn)(void *ud, float ox, float oy,
                              float ex, float ey, int hit_index, float dmg);

Soldier *soldier_create(SimP *s, const SoldierDef *def);  /* NULL = defaults */
void     soldier_destroy(Soldier *sol);

/* Add the body at (x,y). Fails (false) if already deployed, still in the
 * DOWN lockout, or the draggable pool is full. HP resets to hp_max. */
bool soldier_deploy(Soldier *sol, float x, float y);
/* Manual exit: remove body + lure stamp. No DOWN lockout. */
void soldier_recall(Soldier *sol);

/* Step BEFORE simp_step (writes the drive velocity the step integrates;
 * queries ride the collision grid of the PREVIOUS step, per staleness rules).
 * (mvx,mvy) = wanted move direction (any magnitude, normalized inside,
 * 0,0 = stop); (aimx,aimy) = world aim point; fire = trigger held.
 * While inactive only the DOWN timer ticks. Going down (HP <= 0) removes
 * body + lure and arms the lockout — watch soldier_active() flip. */
void soldier_step(Soldier *sol, float mvx, float mvy,
                  float aimx, float aimy, bool fire, float dt,
                  SoldierShotFn shot, void *ud);

/* --- climbing (wall vault, SOLDIER_DESIGN) ---------------------------------
 * soldier_climb_begin removes the body (untouchable: no contact, no shove,
 * lure lifted) and glides the REPORTED position from the current spot to
 * (ex,ey) over dur seconds; when the timer expires the body re-appears at
 * (ex,ey) with HP preserved (this is NOT a redeploy). The HOST owns the
 * detection (which cells are climbable, where the landing is) and the whole
 * climb fiction (clips, anchors); the module owns the body lifecycle so
 * camera/HUD keep a valid soldier_x/y through the vault. Recall mid-climb
 * cancels it. Fails if not deployed or already climbing. */
bool  soldier_climb_begin(Soldier *sol, float ex, float ey, float dur);
bool  soldier_climbing(const Soldier *sol);
float soldier_climb_frac(const Soldier *sol);  /* 0..1 while climbing */

bool  soldier_active(const Soldier *sol);
float soldier_x(const Soldier *sol);       /* valid while active */
float soldier_y(const Soldier *sol);
float soldier_hp(const Soldier *sol);
float soldier_hp_max(const Soldier *sol);
float soldier_down(const Soldier *sol);    /* lockout left (s), 0 = ready */
int   soldier_touchers(const Soldier *sol);/* zombies in contact last step */
/* Draggable pool index of the body while active, -1 otherwise. For the
 * renderer: draw the soldier's own model on this disc instead of the
 * generic draggable box. Unstable across simp_drag_remove, like the pool. */
int   soldier_body_index(const Soldier *sol);

#ifdef __cplusplus
}
#endif
#endif /* SOLDIER_H */

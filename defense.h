/* defense.h — M5 defensive-gameplay layer over sim_particles (M5_DESIGN.md).
 *
 * Game-side, core-agnostic: it talks to the core ONLY through the public simp_*
 * API. Holds the per-slot enemy state (HP, body type, wound) indexed by
 * simp_slot_of (stable for an agent's whole life, M3.1), and the turrets that
 * mow the horde via simp_query_ray (hitscan + piercing + line-of-sight).
 *
 * Slice 1 scope (this file): turret sweep/dwell/fire, light vs heavy damage,
 * the 3-way wound branch, death (corpse / gib). NOT here yet: the base/siege
 * loss condition (§7) and the spawn director (§8) — both build on this.
 *
 * Call order per frame: simp_step(s, dt)  then  def_update(g, dt)  (the turret
 * queries need the collision grid current, i.e. right after the step).
 */
#ifndef DEFENSE_H
#define DEFENSE_H

#include "sim_particles.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Body type → base HP, physical params, heavy-turret toughness, bounty.
 * HP decreasing obese > man > woman > child (considerazioni.txt). Tank is a
 * special: survives several heavy hits before gibbing (no tank corpses). */
typedef enum { BT_OBESE, BT_MAN, BT_WOMAN, BT_CHILD, BT_TANK, BT_COUNT } DefBody;

/* Wound state, set ONCE when a light hit leaves the enemy alive. Mutually
 * exclusive (considerazioni.txt 9-15). The renderer reads this to swap
 * outfit/body VAT; CRAWLING also slows the agent via simp_set_vpref. */
typedef enum { DW_NONE, DW_BLOODY, DW_MAIMED_ARM, DW_CRAWLING } DefWound;

typedef struct {
    float x, y;            /* position (m)                                  */
    float ang;             /* current aim direction (rad)                   */
    float arc_min, arc_max;/* sweep arc extents (rad, no ±pi wrap assumed)  */
    int   sweep_dir;       /* +1 / -1                                       */
    float sweep_speed;     /* rad/s (also the dwell turn rate)              */
    float range;           /* m                                             */
    float fire_period;     /* s between shots (light < heavy)               */
    float fire_timer;
    float damage;          /* HP per shot (light); ignored by heavy (gibs)  */
    int   heavy;           /* 0 = light, 1 = heavy                          */
    int   piercing;        /* light/heavy upgrade: ray pierces all on line  */
    /* set by def_update, for rendering (muzzle flash / tracer feedback):   */
    int   fired;           /* fired on the last update                      */
    float last_t;          /* hit distance of the last shot (else range)    */
    float tracer_ttl;      /* > 0 → draw a tracer; decays each update       */
} DefTurret;

typedef struct DefGame DefGame;

/* cap must match the core's max_agents (per-slot arrays are slot-indexed). */
DefGame *def_create(SimP *s, int cap);
void     def_destroy(DefGame *g);

/* Spawn a typed enemy: simp_spawn_desc + per-slot HP/body/wound init.
 * Returns its handle, or SIMP_HANDLE_INVALID if the core refused (full/wall). */
SimPHandle def_spawn(DefGame *g, float x, float y, DefBody body);

/* Register a turret (copied); returns its id, or -1 if the table is full. */
int        def_add_turret(DefGame *g, const DefTurret *t);
DefTurret *def_turret(DefGame *g, int id);
int        def_turret_count(const DefGame *g);

/* ---- §7 base & defeat: destructible structures ----
 *
 * A structure is a group of nav cells (walls) sharing one HP pool. The horde
 * sieges it via the SIEGE sensor (simp_wall_pressure/_cell): each agent pressing
 * a structure cell to reach the goal beyond runs a per-slot attack timer that
 * chips the pool. At HP 0 the structure COLLAPSES — its cells are freed and the
 * nav recommits, so the horde reroutes itself (M3/SIEGE, already proven). Rings
 * of walls around the base are ordinary structures; the CORE is the one flagged
 * is_core: its collapse is the loss condition (def_lost), not a reroute.
 *
 * Setup (before stepping): add structures, assign their cells, then build the
 * scene goal at the center (simp_set_goal) so the horde is pulled inward. The
 * siege is processed inside def_update (the sensor is fresh right after the
 * step), so the per-frame order stays simp_step → def_update. */

/* Add a structure with hp_max HP; is_core != 0 marks the loss-condition core.
 * Returns its id (>= 0), or -1 if the table is full. */
int   def_add_structure(DefGame *g, float hp_max, int is_core);
/* Assign nav cell (cx,cy) to structure id and raise the wall there. Call
 * simp_terrain_commit yourself once after assigning all cells. */
void  def_struct_cell(DefGame *g, int id, int cx, int cy);

int   def_struct_count(const DefGame *g);
/* Structure id owning nav cell (cx,cy), or -1 (none / freed on collapse). Lets
 * the renderer rebuild the live structure mesh (collapsed cells disappear). */
int   def_cell_struct(const DefGame *g, int cx, int cy);
float def_struct_hp(const DefGame *g, int id);      /* current, 0 if collapsed */
float def_struct_hp_max(const DefGame *g, int id);
int   def_struct_collapsed(const DefGame *g, int id);
int   def_lost(const DefGame *g);                   /* 1 once the core has fallen */

/* Advance all turrets: acquire/dwell or sweep, fire on cadence (only with a
 * target in arc+range), apply damage → wounds / death. Then process the siege
 * on all structures (per-slot attack timers, collapse → reroute / loss).
 * Call after simp_step. */
void def_update(DefGame *g, float dt);

/* ---- read access (tests / HUD) ---- */
int  def_kills(const DefGame *g);
int  def_shots(const DefGame *g);
/* Count of LIVE agents currently in wound state w (scans live slots). */
int  def_count_wound(const DefGame *g, DefWound w);
/* Per-slot arrays (index with simp_slot_of). */
const int     *def_hp(const DefGame *g);
const uint8_t *def_wound(const DefGame *g);
const uint8_t *def_body(const DefGame *g);

#ifdef __cplusplus
}
#endif
#endif /* DEFENSE_H */

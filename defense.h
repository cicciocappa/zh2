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

/* ---- render hook: hit/death events ----
 * Fired during def_update so the renderer can play one-shot animations. HIT =
 * a non-lethal light hit landed (agent survives → flinch + stun). DEATH = a
 * light kill, fired just BEFORE the agent is removed (index i still valid → read
 * px/py/radius). DEF_EV_GIB = morte esplosiva (kill pesante): il renderer spawna
 * i gib volanti (GFX §5), nessun cadavere intatto (lascia solo gore-nav, §7-bis).
 *
 * Gore d'IMPATTO, emesso UNA volta quando una ferita FRESCA viene inflitta
 * (prima di HIT, che parte solo sul sanguinamento): WOUND_BLEED = pochi gib
 * piccoli; WOUND_ARM = gib piccoli + 1 arto volante; WOUND_LEGS = gib piccoli +
 * 2 arti volanti. Mappati dall'host a vat_layer_gib_wound/_limb.
 * Core-agnostic: defense.c non tocca il renderer; l'host collega gli eventi. */
typedef enum { DEF_EV_HIT, DEF_EV_DEATH, DEF_EV_GIB,
               DEF_EV_WOUND_BLEED, DEF_EV_WOUND_ARM, DEF_EV_WOUND_LEGS } DefEvent;
typedef void (*DefEventFn)(void *user, int slot, int i, DefBody body, DefEvent ev);
void def_set_event_cb(DefGame *g, DefEventFn cb, void *user);

/* ---- §8 placement budget ----
 * A static per-level pot the player spends to place turrets. Slice 1: a plain
 * counter; the game sets a turret's cost. def_spend deducts if affordable. */
void def_set_budget(DefGame *g, int budget);
int  def_budget(const DefGame *g);
int  def_spend(DefGame *g, int cost);   /* 1 = afforded (deducted), 0 = too poor */

/* ---- §8 spawn director ----
 *
 * Waves emitted from border rects, BURST-FREE: each attempt spawns only where
 * simp_free_at is clear, so the emitter auto-throttles to the exit's capacity
 * (no PBD ejection). Emission rate and enemy toughness RAMP over waves. Fully
 * game-side, no core primitive. Decoupled from scene.h: the caller passes plain
 * rects (in meters). The optional callback fires once per successful spawn so
 * the renderer can tag the fresh agent (e.g. pin its VAT variant). */

typedef struct { float x, y, w, h; } DefRect;

/* user-supplied, NULL to ignore. h = new agent handle, body its type, roll a
 * per-spawn random word (for cosmetic variant choice on the renderer side). */
typedef void (*DefSpawnFn)(void *user, SimPHandle h, DefBody body, unsigned roll);

typedef struct {
    const DefRect *rects; int nrects; /* emission areas (copied)              */
    float spawn_radius;               /* free_at probe radius (0 → 0.34 m)    */
    float base_rate;                  /* enemies/s during wave 0              */
    float rate_ramp;                  /* added enemies/s per subsequent wave  */
    float wave_period;                /* seconds per wave (0 → 20 s)          */
    uint32_t seed;                    /* RNG seed (0 → default); determinism  */
    DefSpawnFn on_spawn; void *user;
} DefDirectorCfg;

typedef struct DefDirector DefDirector;

DefDirector *def_director_create(DefGame *g, const DefDirectorCfg *cfg);
void def_director_destroy(DefDirector *d);
/* Emit this frame's quota (burst-free). Call once per step. */
void def_director_update(DefDirector *d, float dt);
int  def_director_wave(const DefDirector *d);      /* current wave index (0-based) */
int  def_director_emitted(const DefDirector *d);   /* total agents spawned so far  */

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

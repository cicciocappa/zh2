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

/* Turret kind (torrette 2.0, Blocco 2). LIGHT/HEAVY = the hitscan pair
 * (heavy gibs). FLAME/ACID = cone-AoE status throwers: no ray, no tracer —
 * each shot tick hits EVERY live agent in range within ±cone_half of the
 * barrel that passes line of sight, dealing t->damage direct (silent, no
 * flinch/wound roll) plus the elemental status below. Values 0/1 match the
 * legacy `heavy` field; def_add_turret keeps kind and heavy in sync. */
typedef enum { TUR_LIGHT = 0, TUR_HEAVY = 1, TUR_FLAME = 2, TUR_ACID = 3 }
        DefTurretKind;

/* Elemental status (FLAME/ACID hits), per-slot like wounds. BURNING ticks
 * fire DoT until it expires (refreshed while the victim stays in the jet) —
 * the host swaps the outfit on DEF_EV_IGNITE (14 = charred) and emits
 * flame+smoke on the agent; ACID likewise (DEF_EV_ACID, outfit 15, green
 * fumes). Mutually exclusive, FIRST application wins (refresh same-type
 * only); the swapped outfit is permanent (scar), only the DoT expires. */
typedef enum { DST_NONE = 0, DST_BURNING = 1, DST_ACID = 2 } DefStatus;

typedef struct {
    float x, y;            /* position (m)                                  */
    float ang;             /* current aim direction (rad)                   */
    float arc_min, arc_max;/* sweep arc extents (rad, no ±pi wrap assumed)  */
    int   sweep_dir;       /* +1 / -1                                       */
    float sweep_speed;     /* rad/s (also the dwell turn rate)              */
    float aim_tol;         /* rad: fires only when |aim error| <= this
                              (turn-then-shoot). <= 0 = NO gate (legacy
                              fire-while-slewing — the M5 tests are tuned on
                              it); game turrets use DEF_AIM_TOL_STD.        */
    float range;           /* m                                             */
    float fire_period;     /* s between shots (light < heavy)               */
    float fire_timer;
    float damage;          /* HP per shot (light); ignored by heavy (gibs)  */
    int   heavy;           /* 0 = light, 1 = heavy (legacy; see kind)       */
    int   kind;            /* DefTurretKind; def_add_turret syncs heavy     */
    float cone_half;       /* FLAME/ACID: AoE half-angle (rad, 0 = default
                              ~12°); doubles as their fire-alignment gate   */
    float clear_timer;     /* FLAME/ACID: internal corpse-clear cadence     */
    int   piercing;        /* light/heavy upgrade: ray pierces all on line  */
    /* set by def_update, for rendering (muzzle flash / tracer feedback):   */
    int   fired;           /* fired on the last update                      */
    float last_t;          /* hit distance of the last shot (else range)    */
    float tracer_ttl;      /* > 0 → draw a tracer; decays each update       */
} DefTurret;

/* Standard fire-alignment tolerance (~7°) for game turrets (placement,
 * authored scene cones). Not applied by def_add_turret: aim_tol <= 0 keeps
 * the legacy gate-less behaviour the M5 test scenarios are calibrated on. */
#define DEF_AIM_TOL_STD 0.12f

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

/* Hybrid barricade (DRAG_DESIGN.md): make structure id scatter DRAGGABLE DEBRIS
 * on collapse instead of just vanishing — one finite-mass rubble disc per freed
 * cell (mass in walker units), which the horde then shoves out of the breach.
 * The nav still reroutes (cells freed), but the lane stays physically cluttered.
 * mass <= 0 (default) = clean collapse. No-op for the core. Set before stepping. */
void  def_struct_set_debris(DefGame *g, int id, float mass);

int   def_struct_count(const DefGame *g);
int   def_struct_cap(void);   /* table size — line placement pre-checks room */

/* Undo teardown (PREP_UI_DESIGN §6): LIFO-only removal — accepts ONLY the
 * LAST id, so no live id is ever invalidated. Cells freed cleanly (no debris,
 * no core/loss logic); caller batches simp_terrain_commit. Returns 1, or 0 if
 * not last / not removable (core, turret-backing struct, bound/luring turret). */
int   def_remove_structure(DefGame *g, int id);
int   def_remove_turret(DefGame *g, int tid);
/* Direct HP damage (explosions — EXPLOSION_DESIGN §8 — and tests). Collapse
 * behaves exactly like the siege path (cells freed, reroute, debris, loss on
 * core). No-op on invalid/collapsed ids. */
void  def_struct_damage(DefGame *g, int id, float dmg);

/* ---- explosions (EXPLOSION_DESIGN.md §3/§8) ----
 * def_blast: one event (x,y,radius r, damage dmg, impulse strength/up_ratio)
 * that damages agents (linear falloff, dead -> corpses), pushes/lofts the
 * survivors, damages every structure with a cell in range (D at the nearest
 * cell -> collapse+reroute), and vaporises corpses in range. Friendly fire is
 * full (barricades/turrets/props in range take damage too). Deterministic.
 * def_damage_agent: direct light HP to one live agent (fall damage, hazards). */
void  def_blast(DefGame *g, float x, float y, float r, float dmg,
                float strength, float up_ratio);
void  def_damage_agent(DefGame *g, SimPHandle h, float dmg);
/* def_gib_agent: dismember one agent outright (explosive-death path: GIB event,
 * blood pool, no corpse). For a lethal fall landing. */
void  def_gib_agent(DefGame *g, SimPHandle h);
/* Structure id owning nav cell (cx,cy), or -1 (none / freed on collapse). Lets
 * the renderer rebuild the live structure mesh (collapsed cells disappear). */
int   def_cell_struct(const DefGame *g, int cx, int cy);
/* Bbox (inclusive cell coords) of every cell EVER assigned to a structure —
 * grows with runtime placement, never shrinks on collapse (conservative: the
 * renderer sweeps it and def_cell_struct filters freed cells). Returns 0 and
 * leaves the outputs untouched if no cell was assigned yet. */
int   def_struct_bbox(const DefGame *g, int *cx0, int *cy0, int *cx1, int *cy1);
float def_struct_hp(const DefGame *g, int id);      /* current, 0 if collapsed */
float def_struct_hp_max(const DefGame *g, int id);
int   def_struct_collapsed(const DefGame *g, int id);
int   def_lost(const DefGame *g);                   /* 1 once the core has fallen */

/* ---- destructible turrets (siege-able like structures) ----
 *
 * A bare turret is a point: not solid, not a goal, so the horde streams past it
 * and never attacks. Binding it to a structure makes its nav cell a solid wall
 * (barricade tier) the horde PRESSES to reach the goal beyond — the existing
 * SIEGE sensor + per-slot attack timer chip it for free. On collapse the turret
 * stops firing (def_update skips it) and the cell frees. Strategic consequence:
 * a turret is attacked only when it sits ON the path to a goal (e.g. inside a
 * breached ring) — exposed turrets in the open get flanked, not assaulted.
 *
 * Call simp_terrain_commit() once after binding all turrets (def_struct_cell
 * raises the walls but batches the recommit, like the wall structures). Returns
 * the backing structure id, or -1 (bad turret id / structure table full). */
int   def_turret_make_destructible(DefGame *g, int tid, float hp);
/* 1 if turret tid is destructible AND its structure has collapsed (renderer:
 * stop drawing/firing it). 0 for a live or indestructible turret. */
int   def_turret_disabled(const DefGame *g, int tid);
/* 1 if structure id backs a turret — the renderer skips it in the wall mesh so
 * the turret model isn't double-drawn as a steel box. */
int   def_struct_is_turret(const DefGame *g, int id);
/* Tune the contact siege of turrets (a turret has few attackers vs a long wall,
 * so it needs higher per-agent DPS + wider reach to fall at a comparable rate).
 * dps = HP/s per contacting agent; reach = m beyond the emplacement half-cell.
 * <= 0 keeps the current value. HP stays the per-turret toughness knob. */
void  def_set_turret_contact(DefGame *g, float dps, float reach);
/* Fire lure ("richiamo da fuoco", 2026-07-04): a turret that FIRES attracts
 * the horde — the noise. While a turret has fired within the last linger_s
 * (and isn't disabled), a disc of NEGATIVE user cost (weight at the centre,
 * linear falloff to the rim) is stamped around its emplacement via
 * simp_add_cost; when it falls silent or dies the stamp is removed EXACTLY
 * (per-cell applied deltas are recorded — the core clamp at -0.8 makes blind
 * subtraction wrong under overlaps). Routes bend toward active turrets, the
 * contact siege eats the mob-ed ones: turrets must be defended. weight >= 0
 * disables (default: off — the game host opts in). radius in m. */
void  def_set_fire_lure(DefGame *g, float weight, float radius, float linger_s);

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
               DEF_EV_WOUND_BLEED, DEF_EV_WOUND_ARM, DEF_EV_WOUND_LEGS,
               /* elemental status applied (once per victim, Blocco 2): the
                * host swaps the outfit (IGNITE -> 14, ACID -> 15) and starts
                * per-agent flame/fume FX by polling def_status. */
               DEF_EV_IGNITE, DEF_EV_ACID } DefEvent;
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
    /* scripted exits (GAME_PLAN fase A / TODO M5b — one director per exit): */
    float start_delay;                /* s of silence before the first spawn  */
    int   pool;                       /* total agents to emit; 0 = unlimited  */
} DefDirectorCfg;

typedef struct DefDirector DefDirector;

DefDirector *def_director_create(DefGame *g, const DefDirectorCfg *cfg);
void def_director_destroy(DefDirector *d);
/* Emit this frame's quota (burst-free). Call once per step. */
void def_director_update(DefDirector *d, float dt);
int  def_director_wave(const DefDirector *d);      /* current wave index (0-based) */
int  def_director_emitted(const DefDirector *d);   /* total agents spawned so far  */
int  def_director_pool(const DefDirector *d);      /* configured pool (0 = unlimited) */
int  def_director_done(const DefDirector *d);      /* 1 = finite pool exhausted    */

/* ---- read access (tests / HUD) ---- */
int  def_kills(const DefGame *g);
int  def_shots(const DefGame *g);
/* Count of LIVE agents currently in wound state w (scans live slots). */
int  def_count_wound(const DefGame *g, DefWound w);
/* Per-slot arrays (index with simp_slot_of). */
const int     *def_hp(const DefGame *g);
const uint8_t *def_wound(const DefGame *g);
const uint8_t *def_body(const DefGame *g);
const uint8_t *def_status(const DefGame *g);   /* DefStatus per slot */

#ifdef __cplusplus
}
#endif
#endif /* DEFENSE_H */

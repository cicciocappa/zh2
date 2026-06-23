/* sim_particles.h — Horde-as-granular-particles simulation core (M1+M2).
 *
 * Renderer-agnostic. No external dependencies. Single-precision floats.
 * Companion / successor of the continuum core (sim.h): same grid conventions,
 * but the horde is now a set of DISCRETE agents simulated as a 2D granular
 * material. Three ingredients:
 *
 *   1) GLOBAL NAVIGATION — a flow field. Dijkstra (8-neighbour) from the goal
 *      cells over the nav grid gives a potential phi; a smoothed, normalized
 *      direction field is derived from it and bilinearly sampled by agents.
 *      Cost is O(grid), shared by all agents. Recomputed only when terrain or
 *      goals change (lazy, via dirty flag). Walls are crossable at a huge
 *      cost rather than blocked: open routes always win, but a fully sealed
 *      goal still attracts the horde, which presses against its walls (siege).
 *
 *   2) LOCAL DYNAMICS — steering toward the flow direction with bounded
 *      acceleration, plus per-agent speed variation and directional noise
 *      (kills lockstep, gives the organic "boiling" look).
 *
 *   3) NON-PENETRATION — position-based dynamics (PBD). Overlapping discs are
 *      separated by directly projecting positions (mass-weighted), iterated
 *      a few times per step (Gauss-Seidel over a uniform grid rebuilt each
 *      step with a counting sort). Unconditionally stable at packing density.
 *      Effective velocity is recovered as (x_new - x_old)/dt, so crowd
 *      pushing propagates as real momentum. Walls are handled through a
 *      precomputed signed-distance field (chamfer transform of solid cells).
 *
 * Knobs that matter:
 *   - pbd_iters  : crowd stiffness dial. 1-2 = soft/compressible crowd,
 *                  4+ = rigid granular packing (analog of relax_iters).
 *   - noise_ang  : per-step random steering perturbation (radians).
 *   - v_jitter   : per-agent speed spread (fraction of v_max).
 *
 * BEHAVIOUR STATE — per-agent flag byte (simp_flags_arr):
 *   - SIMP_DORMANT: steers toward zero velocity: stands still, but still
 *     collides, absorbs pushes and reacts to impulses; not drained by goal
 *     cells. Spawn with simp_spawn_dormant (sleeping packs placed on the
 *     map), wake with simp_wake_radius (explosions, noise) or simp_wake_all
 *     (scripted events: sunrise, alarms).
 *   - SIMP_FLYING: ballistic flight on a fake third axis (z, vz). While
 *     airborne: no steering, no damping, no collisions (excluded from the
 *     collision grid), walls ignored above wall_h. Gravity pulls z back to
 *     0; on landing the flag clears, horizontal momentum is mostly killed
 *     (landing_damp) and the agent is reported in the landed buffer (as
 *     handles — fall damage is gameplay's job). Launch via
 *     simp_apply_impulse_ex with up_ratio > 0. Flying agents can leave the
 *     world bounds temporarily; they are clamped back on landing. Goal
 *     cells DO drain overflying agents (simplification, revisit if needed).
 *
 * AGENT TYPES (M3.5) — the core has no notion of "tank" or "runner": types
 * are just per-agent parameters. simp_spawn_desc receives radius, preferred
 * speed and mass explicitly instead of deriving them from defaults + jitter.
 * Mass is in walker units (1.0 = a default-radius agent); PBD only ever uses
 * mass RATIOS, so a mass-10 tank shoves mass-1 walkers aside and is barely
 * deflected in return. Game-side data (HP, type id, ...) goes in arrays
 * indexed by SLOT, as with handles. Spawning an agent larger than the
 * default radius coarsens the collision grid globally (cell = 2*r_max):
 * correct but slower for everyone — keep boss radii within ~2x the default.
 *
 * NAVIGATION COSTS (M3.5 + M3.6 + M3.7) — Dijkstra edges are scaled per
 * destination cell:
 *   edge = dist * clamp(1 + k_density * min(rho/rho_max, 1)
 *                         + k_jam     * min(jam/rho_max, 1) + user, 0.2).
 *   - USER COST (simp_add_cost): screamer lures (w < 0 attracts), fear or
 *     mud (w > 0 repels). Additive per cell, clamped to [-0.8, 100]: never
 *     negative edges, and never competitive with the WALL_ENTER toll.
 *   - DENSITY (Continuum Crowds light): rho is a per-nav-cell histogram of
 *     grounded agents (+ a fixed weight per corpse), blurred 3x3 and smoothed
 *     over time (EMA). With k_density > 0 crowded routes get pricier, so the
 *     horde fans out around jams on its own. rho_max is the packing density
 *     of a cell. k_density = 0 disables the term entirely.
 *   - JAM (M3.7): same histogram, but each agent contributes its STILLNESS
 *     instead of 1: (1 - min(|v|/v_pref, 1))^2 — full weight when stopped,
 *     zero at preferred speed (corpses and dormant packs count in full).
 *     Density alone saturates at k_density and cannot express a chokepoint
 *     with ZERO throughput (its real cost in time is infinite), so the horde
 *     keeps queueing at a physically blocked gap forever. The jam term makes
 *     stalled crowd — and only stalled crowd — expensive enough that the
 *     Dijkstra reroutes around it; when the plug clears, the EMA decays and
 *     the direct route wins again. k_jam = 0 disables it. Stays far below
 *     WALL_ENTER, so sieges against sealed goals are unaffected.
 *   - CORPSE PILE (CORPSE_DESIGN.md §7-bis, the anti-funnel pivot): two extra
 *     per-cell fields track corpses as accumulated VOLUME, not just a fixed rho
 *     weight. corpse_mass grows by pi*r^2 per corpse and decays slowly;
 *     corpse_pack grows as grounded agents trample a piled cell and decays
 *     faster. The derived height = k_h * mass / (1 + k_pack * pack) (meters)
 *     falls as the pile is packed flat. The edge term k_corpse * min(height /
 *     wall_h, 1) lets a DENSE pile climb to a "wall of corpses" cost (ceiling
 *     k_corpse), comparable to a barricade's per-cell breakthrough toll so the
 *     Dijkstra reroutes the horde to BREAK the barricade rather than queue at a
 *     corpse-clogged open gap. Stays below WALL_ENTER (palazzi/sealed sieges
 *     unaffected). An open gap trampled by through-traffic packs flat (pack up
 *     -> height down): no permanent block; a held wall (kills pile up, no
 *     through-traffic) keeps the pile tall. k_corpse = 0 disables it. (Packing
 *     is wired but largely dormant while corpses are INFINITE mass — an agent
 *     can't crest a sealed pile to trample its cell; it comes alive with the
 *     finite-mass slice. Decay alone already keeps piles non-permanent.)
 *   - THROTTLE: phi/flow are recomputed at most every flow_period seconds
 *     (terrain edits still force an immediate full recompute through the
 *     usual dirty flag). Agents read a slightly stale field in between:
 *     invisible in practice, and it keeps the Dijkstra off the hot path.
 *
 * CORPSES — passive discs in the same collision grid (the living shove around
 * them: emergent barricades at chokepoints). Fixed-size pool, TTL-based; when
 * full, the closest-to-expiry corpse is replaced, so capacity is a cost
 * guarantee, not an error. Corpses block simp_free_at (no spawning into a pile)
 * but are invisible to queries and impulses, and never marked in the nav grid:
 * the flow detour around piles emerges from PBD + the corpse-pile nav cost.
 * Internally they ride along as "ghost" entries appended past the live agents
 * before binning — the PBD kernel needs no special cases beyond skipping
 * corpse-corpse pairs (piles must not blow themselves apart). Mass (§3): with
 * corpse_weight > 0 a corpse is a large-but-FINITE mass, so a pushing crowd
 * slowly SHOVES the pile downstream and breaks through (the shoved positions are
 * written back to the pool each step); corpse_weight <= 0 restores the legacy
 * INFINITE-mass seal (used by idealized tests that need a hard plug).
 *
 * Data layout is SoA; position/velocity arrays are public so the renderer
 * can upload them directly as instance buffers. Agents are removed with
 * swap-and-pop, so indices are NOT stable across simp_kill / drain.
 *
 * Determinism: fully deterministic given the same call sequence (internal
 * xorshift RNG, no libc rand).
 */
#ifndef SIM_PARTICLES_H
#define SIM_PARTICLES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float v_max;      /* desired top speed (m/s)                  default 1.4  */
    float v_jitter;   /* per-agent speed spread, fraction of v_max default 0.25 */
    float a_max;      /* max steering acceleration (m/s^2)        default 8.0  */
    float radius;     /* base agent radius (m)                    default 0.30 */
    float r_jitter;   /* per-agent radius spread, fraction         default 0.15 */
    float noise_ang;  /* steering noise half-angle (rad)          default 0.06 */
    float damping;    /* velocity damping per second [0..1]       default 0.10 */
    float v_clamp;    /* hard cap on recovered speed (m/s)        default 20.0 */
    int   pbd_iters;  /* PBD relaxation iterations per step       default 3    */
    float landing_damp; /* horizontal speed kept on landing [0..1] default 0.30 */
    float wall_h;     /* flight altitude that clears walls (m)    default 2.0  */
    float k_density;  /* density->cost gain (0 = off)             default 2.0  */
    float k_jam;      /* stalled-crowd->cost gain (0 = off)       default 8.0  */
    float flow_period;/* min seconds between flow recomputes      default 0.5  */
    /* corpse pile -> cost (CORPSE_DESIGN.md §7-bis) */
    float k_corpse;     /* corpse-pile->cost ceiling (0 = off)     default 300  */
    float k_h;          /* pile height per unit packed volume (m)  default 1.0  */
    float k_pack;       /* packing attenuation of height/cost      default 0.5  */
    float corpse_mass_hl; /* corpse_mass half-life (s)             default 30.0 */
    float corpse_pack_hl; /* corpse_pack half-life (s)             default 10.0 */
    float pack_inc;     /* pack added per trampling agent per recompute  default 1.0 */
    float corpse_weight;/* corpse mass in walker units (§3): the crowd SHOVES a
                         * large-but-finite pile instead of being sealed out.
                         * <= 0 = infinite (legacy idealized seal). default 40.0 */
} SimPParams;

typedef struct SimP SimP;

/* ---- lifecycle ---------------------------------------------------------- */

/* Nav grid is gw x gh cells of side cell_size (meters). World spans
 * [0, gw*cell_size] x [0, gh*cell_size]. max_agents is a hard capacity. */
SimP *simp_create(int gw, int gh, float cell_size, int max_agents);
void  simp_destroy(SimP *s);

/* Nav-grid dimensions and cell size, as passed to simp_create. Read-only; let
 * game code decode cell indices (e.g. simp_wall_cell = cy*grid_w + cx). */
int   simp_grid_w(const SimP *s);
int   simp_grid_h(const SimP *s);
float simp_cell_size(const SimP *s);

SimPParams *simp_params(SimP *s);          /* live-tweakable               */

/* Worker-thread count for simp_step (M4). Default = online CPUs, or the
 * SIMP_THREADS env var. Results are IDENTICAL for any count (colored-tile PBD
 * writes disjoint data) — purely a speed knob. simp_set_threads(s,1) = serial. */
void  simp_set_threads(SimP *s, int nthreads);
int   simp_threads(const SimP *s);

/* ---- terrain & goals (nav grid coordinates) ----------------------------- */

void  simp_set_wall(SimP *s, int cx, int cy, bool solid);
void  simp_set_goal(SimP *s, int cx, int cy, bool goal);
bool  simp_is_wall(const SimP *s, int cx, int cy);
/* Per-cell wall breakthrough toll (Dijkstra entry cost for a solid cell),
 * default simp_wall_base_cost(). The horde presses the CHEAPEST wall on the
 * path, so a high tier (palazzo, rock) routes it AWAY while a lower tier
 * (player barricade) becomes the preferred siege target — even on a detour.
 * Only consulted when the cell is also solid; collision (SDF) is independent
 * (both tiers physically block). Build tiers relative to simp_wall_base_cost
 * (e.g. palazzo = 10x, barricade = 1x) and keep them >> open path lengths. */
void  simp_set_wall_cost(SimP *s, int cx, int cy, float cost);
float simp_wall_base_cost(void);                 /* the uniform default toll */
const float *simp_wall_cost_arr(const SimP *s);  /* gw*gh, for overlays/editor */
/* Force recompute of phi/flow/SDF now (otherwise done lazily on next step). */
void  simp_terrain_commit(SimP *s);

/* Additive user cost on a nav cell: w > 0 repels (fear, mud), w < 0 attracts
 * (screamer lure). Accumulates; clamped to [-0.8, 100]. Takes effect at the
 * next throttled flow recompute (<= flow_period away), or immediately on
 * simp_terrain_commit. */
void  simp_add_cost(SimP *s, int cx, int cy, float w);
void  simp_clear_cost(SimP *s);
const float *simp_user_cost(const SimP *s);     /* gw*gh, for overlays */
/* Smoothed per-nav-cell crowd density used by the k_density term (agents
 * per cell, EMA + blur; updated every flow_period). For overlays/AI. */
const float *simp_density_arr(const SimP *s);
/* Smoothed per-nav-cell STALLED density used by the k_jam term (stillness-
 * weighted agents per cell, same blur + EMA). For overlays/AI. */
const float *simp_jam_arr(const SimP *s);

/* ---- agents -------------------------------------------------------------- */

/* Returns agent index, or -1 if at capacity or (x,y) inside a wall.        */
int   simp_spawn(SimP *s, float x, float y);
/* Same, but the agent starts dormant (stands still until woken).           */
int   simp_spawn_dormant(SimP *s, float x, float y);

/* Typed spawn (M3.5): explicit per-agent parameters instead of defaults +
 * jitter. radius/v_pref in meters, m/s; mass in walker units (1.0 = default
 * agent). v_pref still gets the sim-level v_jitter spread (anti-lockstep);
 * radius and mass are taken exactly. */
typedef struct {
    float radius;        /* m   */
    float v_pref;        /* m/s */
    float mass;          /* walker units; the core stores 1/mass as invm */
} SimPAgentDesc;
int   simp_spawn_desc(SimP *s, float x, float y, const SimPAgentDesc *d);

/* Put agent i to sleep (counterpart of the wake functions; no-op while
 * flying). Lets game code place dormant packs of typed agents. */
void  simp_sleep(SimP *s, int i);

/* Set the preferred speed of a live agent post-spawn (the value spawn_desc set
 * + jitter). For the maimed-legs crawl transition and speed buffs/debuffs.
 * Index-based like simp_kill/simp_sleep: convert from a handle for persistence.
 * No-op for an out-of-range index; takes effect at the next step's steering. */
void  simp_set_vpref(SimP *s, int i, float v_pref);
/* Stun a live agent for `duration` seconds: steering brakes to rest (no goal
 * pull) and self-momentum is dropped at once, so it stays planted under a hit
 * flinch instead of foot-sliding forward. It still collides and absorbs crowd
 * pushes (PBD) — a shoved stunned agent still moves, which is intended. The
 * timer decays per step and the agent resumes on its own (vpref untouched, so
 * jitter survives). duration<=0 clears it. No-op while flying or out of range.
 * Index-based: convert from a handle for persistence. */
void  simp_stun(SimP *s, int i, float duration);
/* Swap-and-pop removal: the last agent takes index i.                      */
void  simp_kill(SimP *s, int i);
int   simp_count(const SimP *s);

/* True if a disc of radius r at (x,y) overlaps no agent and no wall (SDF).
 * Spawning only where this holds gives burst-free emission: a congested
 * exit throttles itself instead of stacking agents and ejecting them.
 * Uses the collision grid when current, falls back to brute force.         */
bool  simp_free_at(const SimP *s, float x, float y, float r);

/* Wake every dormant agent within radius of (x,y) / in the whole world.    */
void  simp_wake_radius(SimP *s, float x, float y, float radius);
void  simp_wake_all(SimP *s);

/* ---- handles (stable references across swap-and-pop) --------------------- */

/* Dense indices are invalidated by every kill; handles survive. Slot map
 * with generation counter: bits 31..20 = generation, bits 19..0 = slot+1
 * (0 stays invalid). Limits: 2^20-1 concurrent agents, a stale handle can
 * only false-positive after 4096 reuses of the same slot.
 * Game-side per-agent data (HP, type, ...) should be indexed by SLOT
 * (simp_slot_of), which is stable for the agent's whole life. */
typedef uint32_t SimPHandle;
#define SIMP_HANDLE_INVALID 0u

SimPHandle simp_handle_of(const SimP *s, int index);
int        simp_slot_of(const SimP *s, int index);
/* Dense index of a live handle, or -1 if it was killed (even if the slot
 * has been reused since). */
int        simp_index_of(const SimP *s, SimPHandle h);

/* ---- spatial queries (gameplay: turrets, AoE) ----------------------------- */

#define SIMP_QUERY_FLYING 0x1u   /* include airborne agents (forces brute force) */
#define SIMP_RAY_NOWALL   0x2u   /* simp_query_ray: ignore wall occlusion (LoS) */

/* Fill out[] with the indices of agents whose center lies within r of (x,y);
 * returns how many were written (saturating at max_out). Flying agents are
 * excluded unless SIMP_QUERY_FLYING is set; corpses are never returned.
 * Walks the collision grid (cost ~ queried area) when it is current — i.e.
 * when called after simp_step, before any spawn/kill/corpse_add — and falls
 * back to brute force otherwise.
 * USAGE TRAP: returned indices die on the first kill. Either convert them
 * to handles before applying game logic, or kill strictly in decreasing
 * index order (swap-and-pop only moves indices greater than the killed one). */
int simp_query_circle(const SimP *s, float x, float y, float r,
                      int *out, int max_out, uint32_t flags);
/* Index of the closest agent within r_max of (x,y), or -1. Ring scan over
 * the collision grid from the center outward; same staleness rules.        */
int simp_query_nearest(const SimP *s, float x, float y, float r_max);

/* Hitscan along the segment from (ox,oy) in direction (dx,dy) up to maxdist.
 * Fills out[]/out_t[] with the INDICES of the agents whose disc the ray
 * crosses and the ray parameter t (distance) of the entry, ORDERED by t
 * ascending; returns how many were written. If more than max_out agents are
 * hit, the NEAREST max_out are returned. (dx,dy) need not be normalized.
 *   - Non-piercing turret = use out[0] (the first agent hit).
 *   - Piercing turret     = iterate all returned hits.
 * The ray is OCCLUDED by walls (line-of-sight): maxdist is truncated at the
 * first solid nav cell, so a turret cannot shoot through a wall. Pass
 * SIMP_RAY_NOWALL to ignore occlusion. Flying agents are excluded unless
 * SIMP_QUERY_FLYING is set (forces brute force); corpses are never hit.
 * Same staleness rules as the other queries (grid valid after simp_step). */
int simp_query_ray(const SimP *s, float ox, float oy, float dx, float dy,
                   float maxdist, int *out, float *out_t, int max_out,
                   uint32_t flags);

/* ---- behaviour flags ------------------------------------------------------ */

#define SIMP_DORMANT 0x1u    /* stands still until woken                       */
#define SIMP_FLYING  0x2u    /* airborne: no steering, no collisions           */

const uint8_t *simp_flags_arr(const SimP *s);
const float   *simp_z_arr(const SimP *s);   /* altitude (m), 0 = on the ground */

/* Agents that touched down during the last step, reported as handles (safe
 * across the drain that may follow within the same step). For fall damage. */
int               simp_landed_count(const SimP *s);
const SimPHandle *simp_landed(const SimP *s);

/* ---- impulses ------------------------------------------------------------- */

/* Radial impulse (explosion): dv = strength * (1 - r/radius) away from
 * (x,y), applied to every agent within radius. */
void  simp_apply_impulse(SimP *s, float x, float y, float radius, float strength);
/* Same, plus a vertical kick: vz += strength * falloff * up_ratio. Agents
 * whose vz exceeds ~1 m/s take off (SIMP_FLYING). Corpses are unaffected.
 * Note: the gridded fast path skips agents already airborne (they are not
 * binned); brute-force fallback boosts them too. */
void  simp_apply_impulse_ex(SimP *s, float x, float y, float radius,
                            float strength, float up_ratio);

/* ---- corpses (passive obstacles) ------------------------------------------ */

/* Add a static corpse disc (typical: at a kill site, radius ~0.9x the dead
 * agent, ttl a few seconds). Pool is fixed-size; when full the closest-to-
 * expiry corpse is replaced. Returns the pool index (not stable, no handles:
 * gameplay should not point at corpses). */
int   simp_corpse_add(SimP *s, float x, float y, float radius, float ttl);
int   simp_corpse_count(const SimP *s);
const float *simp_corpse_px(const SimP *s);
const float *simp_corpse_py(const SimP *s);
const float *simp_corpse_rad(const SimP *s);

/* Per-nav-cell corpse-pile height (m), derived from accumulated mass and
 * packing (height = k_h * mass / (1 + k_pack * pack)), refreshed at the end of
 * each step. Drives the k_corpse nav cost; game/overlays compare it to wall_h
 * for the future ramp. gw*gh, indexed by cell (no agent-index trap). */
const float *simp_corpse_height(const SimP *s);

/* Fire/acid: remove physical corpses whose center lies within radius of (x,y)
 * AND zero the pile fields (mass/pack/height) on the cells in range, then force
 * a flow recompute. The game-side "no-corpse" defenses (CORPSE_DESIGN.md §6)
 * use this to keep a pile from climbing to wall height. */
void  simp_corpse_clear(SimP *s, float x, float y, float radius);

/* ---- stepping ------------------------------------------------------------ */

/* One fixed step. Agents stepping onto a goal cell are drained (killed);
 * the number drained this step is returned. */
int   simp_step(SimP *s, float dt);

/* ---- read access for rendering / tests ----------------------------------- */

const float *simp_px(const SimP *s);       /* x positions, simp_count() long */
const float *simp_py(const SimP *s);
const float *simp_vx(const SimP *s);
const float *simp_vy(const SimP *s);
const float *simp_radius_arr(const SimP *s);

/* ---- siege sensor (SIEGE_DESIGN.md) -------------------------------------- *
 * Per-grounded-agent wall contact resolved THIS step, for the destructible-
 * wall mechanic: who is pressing against a wall TO REACH ITS GOAL, and where.
 * Both arrays are simp_count() long, indexed by DENSE index (like vx/vy) -
 * convert to slot/handle at once, the index dies on the next kill.
 *
 *   simp_wall_pressure[i] = push * max(into_wall, 0), the penetration the wall
 *      had to resolve at the last projection times how much the agent steers
 *      INTO the wall (-flow . normal). 0 if not in contact, or in contact but
 *      steering tangent/away (that is GRAZING, not siege - the hazard mechanic).
 *   simp_wall_cell[i] = nav cell index being besieged (toward -SDF gradient),
 *      or -1 if simp_wall_pressure[i] == 0.
 *
 * Flyers never register (overflown or ballistic). A fresh snapshot each step;
 * the siege MEMORY (attack timers, structure HP) lives in game code. */
const float *simp_wall_pressure(const SimP *s);
const int   *simp_wall_cell(const SimP *s);

/* Flow-field direction at world point (bilinear, normalized; 0,0 in walls). */
void  simp_sample_flow(const SimP *s, float x, float y, float *dx, float *dy);
/* Signed distance to nearest wall at world point (positive = free space).  */
float simp_sample_sdf(const SimP *s, float x, float y);

/* Diagnostics: mean pairwise overlap depth (m) measured on last step.      */
float simp_mean_overlap(const SimP *s);

#ifdef __cplusplus
}
#endif
#endif /* SIM_PARTICLES_H */

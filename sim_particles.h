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
 * CORPSES — passive infinite-mass discs in the same collision grid (the
 * living shove around them: emergent barricades at chokepoints). Fixed-size
 * pool, TTL-based; when full, the closest-to-expiry corpse is replaced, so
 * capacity is a cost guarantee, not an error. Corpses block simp_free_at
 * (no spawning into a pile) but are invisible to queries and impulses, and
 * never marked in the nav grid: the flow detour around piles emerges from
 * PBD alone. Internally they ride along as "ghost" entries appended past
 * the live agents before binning, with inverse mass 0 — the PBD kernel
 * needs no special cases.
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
} SimPParams;

typedef struct SimP SimP;

/* ---- lifecycle ---------------------------------------------------------- */

/* Nav grid is gw x gh cells of side cell_size (meters). World spans
 * [0, gw*cell_size] x [0, gh*cell_size]. max_agents is a hard capacity. */
SimP *simp_create(int gw, int gh, float cell_size, int max_agents);
void  simp_destroy(SimP *s);

SimPParams *simp_params(SimP *s);          /* live-tweakable               */

/* ---- terrain & goals (nav grid coordinates) ----------------------------- */

void  simp_set_wall(SimP *s, int cx, int cy, bool solid);
void  simp_set_goal(SimP *s, int cx, int cy, bool goal);
bool  simp_is_wall(const SimP *s, int cx, int cy);
/* Force recompute of phi/flow/SDF now (otherwise done lazily on next step). */
void  simp_terrain_commit(SimP *s);

/* ---- agents -------------------------------------------------------------- */

/* Returns agent index, or -1 if at capacity or (x,y) inside a wall.        */
int   simp_spawn(SimP *s, float x, float y);
/* Same, but the agent starts dormant (stands still until woken).           */
int   simp_spawn_dormant(SimP *s, float x, float y);
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

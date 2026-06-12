/* sprite_layer.h — prerendered-sprite rendering layer over the particle core.
 *
 * Implements the renderer-side conventions of GFX_DESIGN.md §4 on top of the
 * SDL3 2D renderer, as the validation step before the real GPU stack:
 *
 *   - FACING: the core stores no heading and instantaneous velocity is
 *     dirtied by PBD. Heading = per-slot EMA of velocity (~0.25 s time
 *     constant), quantized to the sheet's direction count. Sheet row k =
 *     heading k*(360/dirs)deg CLOCKWISE from "facing the camera"
 *     (screen-down), matching gfx/sprite_render.py.
 *   - WALK VARIANTS: up to 8 .zspr sheets; each agent is assigned one by
 *     handle hash, breaking the everyone-marches-the-same uniformity.
 *     Each sheet carries its own measured stride.
 *   - ANIMATION: the walk frame advances with DISTANCE TRAVELLED (not
 *     time), using the variant's stride (meters per cycle, baked into the
 *     .zspr by the pipeline), so feet don't slide and per-agent v_jitter
 *     desynchronizes the horde for free. Phase and initial heading are
 *     seeded from the handle: a fresh agent in a reused slot restarts clean.
 *   - Y-SORT: sprites are drawn in increasing ground-y order.
 *   - ANCHOR: the sheet's baked ground anchor is pinned to the agent (x, y);
 *     sprite size scales with radius/0.30 (the tank is just a big zombie).
 *   - VARIETY: per-agent tint from a curated zombie palette + brightness
 *     jitter, hashed from the handle (dormant agents are dimmed instead,
 *     so sleeping packs read at a glance).
 *   - FLIGHT: SIMP_FLYING draws at y - z*k_z*ppm with an elliptic shadow
 *     left at the ground position (the M3.2 parabola becomes readable).
 *
 * Per-slot state lives here, NOT in the core (see GFX_DESIGN.md §7).
 * Loads the .zspr sheets written by gfx/sheet_pack.py (format ZSP2).
 */
#ifndef SPRITE_LAYER_H
#define SPRITE_LAYER_H

#include "sim_particles.h"
#include <SDL3/SDL.h>

typedef struct SpriteLayer SpriteLayer;

/* Load up to n_paths .zspr walk sheets (missing/bad files are skipped with
 * an SDL_Log) and allocate per-slot state for max_agents slots. Returns
 * NULL if not a single sheet loaded. */
SpriteLayer *sprite_layer_create(SDL_Renderer *ren,
                                 const char *const *zspr_paths, int n_paths,
                                 int max_agents);
void sprite_layer_destroy(SpriteLayer *sl);

/* Advance facing EMA + animation phase. Call once per simp_step with the
 * same dt (skip while paused: sprites freeze with the sim). */
void sprite_layer_update(SpriteLayer *sl, const SimP *s, float dt);

/* Draw every agent as a y-sorted sprite. cam_x/cam_y = world meters of the
 * viewport's top-left corner, ppm = pixels per meter. */
void sprite_layer_draw(SpriteLayer *sl, SDL_Renderer *ren, const SimP *s,
                       float cam_x, float cam_y, float ppm);

#endif /* SPRITE_LAYER_H */

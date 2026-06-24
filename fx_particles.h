/* fx_particles.h — general-purpose 3D particle system (render FX).
 *
 * Renderer-agnostic, zero-dependency C (only libm), like the sim cores: the
 * module SIMULATES particles (spawn, ballistic integration with gravity/drag/
 * wind, life, color/scale tween) and hands out billboard instance data; the
 * actual GL drawing lives in the caller (fxlab / vat_horde), exactly like the
 * gib/decal pools are filled by vat_layer_fill_* and drawn by the loop.
 *
 * Ported from mage-commando's particle.c: cglm types -> plain float[], rand()
 * -> internal xorshift (deterministic), gamedata defs -> FxEmitterDef here,
 * flat Y=0 ground -> a ground-height callback (terrain-aware stopping).
 *
 * Units: meters, seconds. World axes match fxlab's 3D: pos[0]=x (world east),
 * pos[1]=y (UP / height), pos[2]=z (world depth = the sim's "y"). Gravity pulls
 * -y. The ground callback is sampled at (x,z).
 *
 * Versatile by data: an FxEmitterDef is a preset. Blood = a small dark-red cone
 * burst with gravity + ground_stop; fire/smoke/explosions are other presets
 * (additive blend, upward speed, no gravity or negative for buoyancy, etc.).
 */
#ifndef FX_PARTICLES_H
#define FX_PARTICLES_H

#include <stdbool.h>

#define FX_MAX_PARTICLES     4096
#define FX_MAX_EMITTERS        16
#define FX_MAX_COLOR_VARIANTS   8

typedef enum { FX_BLEND_ALPHA = 0, FX_BLEND_ADD = 1 } FxBlend;

typedef enum {
    FX_EMIT_POINT = 0,   /* all from origin                                  */
    FX_EMIT_CIRCLE,      /* random point in a disc of spawn_radius (default) */
    FX_EMIT_BOX,         /* rectangle: +/- spawn_radius (x), +/- spawn_box_z */
    FX_EMIT_LINE         /* segment along x: +/- spawn_radius                */
} FxEmitShape;

/* A particle preset. Burst count + per-particle randomized ranges. */
typedef struct {
    int   count;                 /* particles per emission (burst)           */
    FxEmitShape shape;
    float spawn_radius;          /* disc/box/line half-extent on x           */
    float spawn_box_z;           /* box half-extent on z                     */
    float spawn_offset_y_min, spawn_offset_y_max;  /* vertical spawn jitter  */

    float speed_xy_min, speed_xy_max;   /* horizontal speed magnitude        */
    float speed_y_min,  speed_y_max;    /* vertical (up) speed               */
    float gravity;                       /* downward accel (m/s^2); 9.81 real */
    float drag;                          /* linear velocity damping /s (0=off)*/

    float lifetime_min, lifetime_max;

    float start_scale_min, start_scale_max;   /* billboard size (m) at birth */
    float end_scale_min,   end_scale_max;     /* ...at death (linear tween)  */

    float start_color[4], end_color[4];       /* RGBA, tweened over life     */
    float color_variants[FX_MAX_COLOR_VARIANTS][4];
    int   color_variant_count;   /* >0: pick a random variant as start color */

    int   sprite_first, sprite_last;   /* atlas tile range; <0 = flat/proc.  */
    float wind_scale;            /* per-particle wind influence (0..1)        */
    bool  ground_stop;           /* freeze at ground level instead of sliding */
    FxBlend blend;

    float rate;                  /* emissions/sec for continuous emitters     */
} FxEmitterDef;

typedef struct {
    bool  active;
    float pos[3], vel[3];
    float life, max_life;
    float start_color[4], end_color[4];
    float start_scale, end_scale;
    float gravity, drag, wind_scale;
    int   sprite_index;          /* <0 = flat color / procedural disc         */
    bool  ground_stop;
    FxBlend blend;
} FxParticle;

typedef struct {
    bool  active;
    float pos[3];
    const FxEmitterDef *def;
    float timer;                 /* countdown to next emission                */
    float duration;              /* total (-1 = infinite)                     */
    float elapsed;
} FxEmitter;

/* Ground-height query at a world (x,z). NULL => flat ground at y=0. */
typedef float (*FxGroundFn)(float x, float z, void *ud);

typedef struct {
    FxParticle pool[FX_MAX_PARTICLES];
    FxEmitter  emitters[FX_MAX_EMITTERS];
    unsigned   rng;              /* xorshift state (deterministic)            */
} FxParticles;

/* Reset all pools; seed the deterministic RNG (0 -> a nonzero default). */
void fx_init(FxParticles *fp, unsigned seed);

/* Advance one step. wind = horizontal acceleration {ax, az} (m/s^2) scaled
 * per particle by wind_scale; pass {0,0} for none. ground_fn (with ud) gives
 * the floor height under each particle; NULL = flat y=0. */
void fx_update(FxParticles *fp, float dt, const float wind[2],
               FxGroundFn ground_fn, void *ground_ud);

/* Low-level: spawn one particle. Returns false if the pool is full. */
bool fx_emit_one(FxParticles *fp, const float pos[3], const float vel[3],
                 float life, float gravity, float drag,
                 const float start_col[4], const float end_col[4],
                 float start_scale, float end_scale, int sprite_index,
                 float wind_scale, bool ground_stop, FxBlend blend);

/* Burst from a preset. half_angle < 0 => full 360 deg spread (omnidirectional);
 * otherwise velocities + spawn are constrained to direction +/- half_angle (rad)
 * about the world XZ heading `direction`. */
void fx_emit(FxParticles *fp, const float origin[3], const FxEmitterDef *def,
             float direction, float half_angle);

/* Continuous emitter: emits `def` at `rate`/s for `duration` s (-1 = forever).
 * Returns an emitter index, or -1 if none free. */
int  fx_start_emitter(FxParticles *fp, const float pos[3],
                      const FxEmitterDef *def, float duration);
void fx_stop_emitter(FxParticles *fp, int idx);

/* Build billboard instance data for all active particles of `blend`. Writes,
 * per matching particle: 16 floats (column-major model matrix oriented to the
 * camera via right/up), 4 floats RGBA, 1 float sprite index. right/up are the
 * camera basis (from the view matrix). Returns the instance count (<= max). */
int  fx_collect(const FxParticles *fp, FxBlend blend,
                const float right[3], const float up[3],
                float *out_mat16, float *out_rgba, float *out_sprite, int max);

int  fx_active_count(const FxParticles *fp);

#endif

/* sandbox_particles.c — interactive SDL3 sandbox over the granular core.
 *
 * Paint walls/goals/spawners, blow things up, turn the knobs live and watch
 * the horde flow through the chokepoints. The simulation core
 * (sim_particles.c/.h) has NO SDL dependency; this file is purely the
 * visualization + input layer.
 *
 * Build:
 *     make sandbox        (needs SDL3; see Makefile for PKG_CONFIG_PATH)
 *
 * Scenes:
 *     ./sandbox [scenes/file.txt]   start from an ASCII scene file (see
 *                                   scene.h for the format; must be 160x120)
 *     F2                            save current walls/goals/spawners/packs
 *                                   (+ the main knobs) to the loaded path,
 *                                   or to scene_saved.txt if none
 *     R                             reset = reinstantiate the loaded scene
 *                                   (empty world when started without one)
 *
 * Controls:
 *     LMB            paint the current brush
 *     RMB            erase (clears wall+goal+spawner under the brush)
 *     E / MMB        explosion at cursor (also wakes sleepers around it)
 *     1              brush = WALL
 *     2              brush = SPAWNER (continuous emitter, throttles itself
 *                    when there is no free space: tunnel-mouth flow)
 *     3              brush = GOAL (drains agents)
 *     4              brush = ERASE (same as RMB)
 *     5              brush = PACK (one-shot: places dormant agents under the
 *                    brush; drag/repaint to pack the area denser)
 *     6              brush = KILL (kills agents under the brush; ~30% leave
 *                    a corpse: a static obstacle the crowd shoves around)
 *     G              brush = COST+ (paint user nav cost: repels the flow —
 *                    fear zone / mud; repaint to stack it up)
 *     H              brush = COST- (negative cost: attracts the flow —
 *                    screamer lure; clamped at -0.8)
 *     X              clear all painted user cost
 *     M              cycle spawn type for SPAWNER and PACK brushes:
 *                    WALKER -> RUNNER -> TANK -> MIX (5% tank, 15% runner)
 *     K / L          k_density down / up (density->cost gain, 0 = off:
 *                    the horde fans out around its own jams)
 *     A / S          k_jam down / up (stalled-crowd->cost gain, 0 = off:
 *                    a queue with zero throughput prices itself out and
 *                    the horde reroutes; try a gap sealed by corpses)
 *     O              cycle overlay: off -> crowd density (violet, the rho
 *                    the flow sees) -> stalled crowd (red, the jam term)
 *     W              wake all sleepers ("sunrise")
 *     7 / U          explosion up_ratio down / up (vertical kick: zombies
 *                    above ~1 m/s vz take off, fly over walls above 2 m,
 *                    and land with most horizontal momentum gone)
 *     [ / ]          brush size down / up
 *     - / =          pbd_iters down / up        (crowd stiffness)
 *     , / .          noise_ang down / up        (steering noise)
 *     ; / '          v_max down / up            (desired speed)
 *     9 / 0          impulse radius down / up
 *     8 / I          impulse strength down / up
 *     Z              cycle zoom tier 12/16/32 px/m (anchored at the cursor):
 *                    12 = whole map as quads, 16/32 = sprite tiers
 *     ARROW KEYS     pan the viewport
 *     B              toggle the sprite layer (needs gfx/out/zombie/
 *                    walk_sheet.zspr from gfx/sheet_pack.py; quads otherwise)
 *     D              cycle the floor color (nocturnal / dusk / earthy /
 *                    stone / grass): sprite visibility on different grounds
 *     F3             save a screenshot to sandbox_shot.bmp
 *                    (env SANDBOX_SHOT="frame[,tier,camx,camy]" does it
 *                    unattended: run N frames, shoot, quit — for headless
 *                    visual checks)
 *     SPACE          pause / resume
 *     N              single step (while paused)
 *     T              toggle spawners on/off
 *     V              toggle speed tint on agents
 *     F              toggle flow-field overlay
 *     C              clear agents (keep terrain)
 *     R              reset everything (empty world)
 *     ESC            quit
 */
#include "sim_particles.h"
#include "scene.h"
#include "sprite_layer.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Nav grid: 160x120 cells of 0.5 m -> 80 m x 60 m world. */
#define GW   160
#define GH   120
#define CELL 0.5f
#define WINW ((int)(GW * CELL * 12.0f))  /* 960: whole world at 12 px/m */
#define WINH ((int)(GH * CELL * 12.0f))  /* 720 */

/* Scrolling viewport with discrete zoom tiers (GFX_DESIGN.md §2, adapted to
 * the 80x60 m sandbox): 12 px/m = whole map (quad rendering), 16 and 32 are
 * the sprite tiers. cam_x/cam_y = world meters at the window's top-left. */
static const float TIER_PPM[] = { 12.0f, 16.0f, 32.0f };
static int   cam_tier = 0;
static float cam_x = 0.0f, cam_y = 0.0f;
#define PPM (TIER_PPM[cam_tier])

static void cam_clamp(void) {
    float mx = GW * CELL - (float)WINW / PPM;
    float my = GH * CELL - (float)WINH / PPM;
    if (cam_x > mx) cam_x = mx;
    if (cam_y > my) cam_y = my;
    if (cam_x < 0.0f) cam_x = 0.0f;
    if (cam_y < 0.0f) cam_y = 0.0f;
}

#define MAX_AGENTS      30000
#define SPAWN_RATE      1.5f             /* agents/sec per spawner cell */
#define DT              (1.0f / 60.0f)

enum { B_WALL, B_SPAWN, B_GOAL, B_ERASE, B_PACK, B_KILL, B_COSTP, B_COSTM };
static const char *BRUSH_NAME[] = { "WALL", "SPAWN", "GOAL", "ERASE", "PACK", "KILL",
                                    "COST+", "COST-" };

/* spawn types (M3.5): plain per-agent parameters, the core has no type ids */
enum { T_WALKER, T_RUNNER, T_TANK, T_MIX };
static const char *TYPE_NAME[] = { "WALKER", "RUNNER", "TANK", "MIX" };
static const SimPAgentDesc TYPE_DESC[3] = {
    { 0.30f, 1.4f, 1.0f },     /* walker */
    { 0.27f, 2.8f, 0.9f },     /* runner: thin and fast, threads the gaps */
    { 0.55f, 1.0f, 10.0f },    /* tank: shoves the crowd open */
};
static int spawn_type = T_WALKER;

/* roll the actual descriptor for the current spawn type (MIX is a blend) */
static const SimPAgentDesc *roll_desc(void) {
    int t = spawn_type;
    if (t == T_MIX) {
        int r = rand() % 100;
        t = r < 5 ? T_TANK : (r < 20 ? T_RUNNER : T_WALKER);
    }
    return &TYPE_DESC[t];
}

/* Spawners live in the sandbox, not in the core: the core only knows walls
 * and goals. spawn_acc accumulates fractional spawns per cell. */
static uint8_t spawn_flag[GW * GH];
static float   spawn_acc[GW * GH];

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static void speed_ramp(float t, Uint8 *r, Uint8 *g, Uint8 *b) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    *r = (Uint8)(200 + 55 * t);
    *g = (Uint8)(60 + 160 * t);
    *b = 50;
}

/* floor color palettes (D cycles them): sprite visibility check on
 * different grounds, and a taste of the open palette/mood decision in
 * GFX_DESIGN.md §8 (nocturnal vs earthy daylight) */
static const uint8_t FLOOR_PAL[][3] = {
    { 20, 24, 22 },     /* nocturnal (original) */
    { 52, 58, 48 },     /* dusk grey-green      */
    { 86, 70, 48 },     /* earthy daylight, WC2 */
    { 110, 112, 100 },  /* light stone          */
    { 30, 48, 26 },     /* dark grass           */
};
static int floor_idx = 0;

/* overlay_mode: 0 = off, 1 = crowd density (violet), 2 = stalled crowd (red) */
static void terrain_render(SimP *s, Uint32 *px, int overlay_mode) {
    const float *uc  = simp_user_cost(s);
    const float *rho = overlay_mode == 2 ? simp_jam_arr(s) : simp_density_arr(s);
    const SimPParams *P = simp_params(s);
    /* same packing density the core uses to saturate the k_density term */
    const float inv_rho_max =
        (3.14159265f * P->radius * P->radius) / (CELL * CELL * 0.7f);
    for (int cy = 0; cy < GH; cy++)
    for (int cx = 0; cx < GW; cx++) {
        int i = cy * GW + cx;
        int r = FLOOR_PAL[floor_idx][0], g = FLOOR_PAL[floor_idx][1],
            b = FLOOR_PAL[floor_idx][2];
        if (simp_is_wall(s, cx, cy))      { r = 80; g = 80; b = 95; }
        else if (spawn_flag[i])           { r = 90; g = 40; b = 40; }
        else {
            if (uc[i] > 0.0f) {                             /* fear: orange */
                float t = uc[i] / 5.0f; if (t > 1.0f) t = 1.0f;
                r += (int)(90 * t); g += (int)(45 * t);
            } else if (uc[i] < 0.0f) {                      /* lure: green  */
                float t = -uc[i] / 0.8f; if (t > 1.0f) t = 1.0f;
                g += (int)(90 * t);
            }
            if (overlay_mode == 1) {                        /* rho: violet  */
                float t = rho[i] * inv_rho_max; if (t > 1.0f) t = 1.0f;
                r += (int)(110 * t); b += (int)(140 * t);
            } else if (overlay_mode == 2) {                 /* jam: red     */
                float t = rho[i] * inv_rho_max; if (t > 1.0f) t = 1.0f;
                r += (int)(160 * t); b += (int)(30 * t);
            }
        }
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        px[i] = 0xFF000000u | (Uint32)(r << 16) | (Uint32)(g << 8) | (Uint32)b;
    }
}

/* Goal cells are core-side state with no public read access, so the sandbox
 * mirrors them for rendering. */
static uint8_t goal_flag[GW * GH];

static void paint(SimP *s, int cx, int cy, int brush, int size) {
    if (brush == B_KILL) {
        /* per-event, not per-cell: circle query then kill in DESCENDING
         * index order (indices die on each kill); ~30% leave a corpse */
        static int kbuf[8192];
        float wx = ((float)cx + 0.5f) * CELL, wy = ((float)cy + 0.5f) * CELL;
        int n = simp_query_circle(s, wx, wy, ((float)size + 0.5f) * CELL,
                                  kbuf, 8192, 0);
        const float *px = simp_px(s), *py = simp_py(s);
        const float *rad = simp_radius_arr(s);
        /* query returns grid order: sort ascending, then walk backward */
        for (int a = 1; a < n; a++) {
            int v = kbuf[a], b = a - 1;
            while (b >= 0 && kbuf[b] > v) { kbuf[b + 1] = kbuf[b]; b--; }
            kbuf[b + 1] = v;
        }
        for (int k = n - 1; k >= 0; k--) {
            int i = kbuf[k];
            if (rand() % 100 < 30)
                simp_corpse_add(s, px[i], py[i], rad[i] * 0.95f, 12.0f);
            simp_kill(s, i);
        }
        return;
    }
    for (int dy = -size; dy <= size; dy++)
    for (int dx = -size; dx <= size; dx++) {
        int x = cx + dx, y = cy + dy;
        if (x <= 0 || y <= 0 || x >= GW - 1 || y >= GH - 1) continue;
        if (dx * dx + dy * dy > size * size) continue;
        int i = y * GW + x;
        switch (brush) {
        case B_WALL:
            simp_set_wall(s, x, y, true);
            spawn_flag[i] = 0; goal_flag[i] = 0;
            simp_set_goal(s, x, y, false);
            break;
        case B_SPAWN:
            if (!simp_is_wall(s, x, y)) spawn_flag[i] = 1;
            break;
        case B_GOAL:
            if (!simp_is_wall(s, x, y)) {
                simp_set_goal(s, x, y, true);
                goal_flag[i] = 1;
            }
            break;
        case B_ERASE:
            simp_set_wall(s, x, y, false);
            simp_set_goal(s, x, y, false);
            spawn_flag[i] = 0; goal_flag[i] = 0;
            /* painted user cost goes too (additive API: subtract it out) */
            if (simp_user_cost(s)[i] != 0.0f)
                simp_add_cost(s, x, y, -simp_user_cost(s)[i]);
            break;
        case B_PACK: {
            /* one-shot placement, no emitter state: one jittered attempt per
             * cell per paint event; simp_free_at makes repainting idempotent
             * (it only fills the gaps, up to packing density) */
            const SimPAgentDesc *d = roll_desc();
            float wx = ((float)x + frand()) * CELL;
            float wy = ((float)y + frand()) * CELL;
            if (simp_free_at(s, wx, wy, d->radius)) {
                int a = simp_spawn_desc(s, wx, wy, d);
                if (a >= 0) simp_sleep(s, a);
            }
            break;
        }
        case B_COSTP: simp_add_cost(s, x, y, +0.5f); break;
        case B_COSTM: simp_add_cost(s, x, y, -0.2f); break;
        }
    }
}

static void spawners_step(SimP *s, float dt) {
    /* Emitters ask for clearance beyond the agent radius: in a congested
     * mouth the neighbours are already overlapping each other under crowd
     * pressure, and a newborn admitted into an exactly-fitting pocket
     * becomes the pressure relief valve (it pops out at high speed). The
     * cushion makes the emitter throttle a bit earlier instead. */
    for (int cy = 1; cy < GH - 1; cy++)
    for (int cx = 1; cx < GW - 1; cx++) {
        int i = cy * GW + cx;
        if (!spawn_flag[i]) continue;
        spawn_acc[i] += SPAWN_RATE * dt;
        /* cap the accumulator: a blocked emitter must not bank up spawns
         * and release them as a burst once space frees */
        if (spawn_acc[i] > 1.0f) spawn_acc[i] = 1.0f;
        if (spawn_acc[i] >= 1.0f) {
            spawn_acc[i] -= 1.0f;
            const SimPAgentDesc *d = roll_desc();
            float rchk = d->radius + 0.5f * simp_params(s)->radius;
            float x = ((float)cx + frand()) * CELL;
            float y = ((float)cy + frand()) * CELL;
            /* only emit into free space: no overlap -> no PBD ejection.
             * If the mouth is congested the credit is forfeited, so the
             * emitter self-throttles to the drain rate of the exit. */
            if (!simp_free_at(s, x, y, rchk)) continue;
            if (simp_spawn_desc(s, x, y, d) < 0) return;   /* at capacity */
        }
    }
}

static void boom(SimP *s, float x, float y, float radius, float strength,
                 float up_ratio) {
    simp_apply_impulse_ex(s, x, y, radius, strength, up_ratio);
    /* the blast is heard beyond where it is felt: wake a wider circle */
    simp_wake_radius(s, x, y, radius * 2.0f);
}

/* Optional starting configuration from an ASCII scene file (CLI arg). */
static Scene      start_scene;
static int        start_scene_loaded = 0;
static const char *scene_path = NULL;

/* Start from the loaded scene if any, else from an empty world (paint
 * everything with the brushes, like the first sandbox). */
static SimP *scene_create(void) {
    SDL_memset(spawn_flag, 0, sizeof spawn_flag);
    SDL_memset(spawn_acc, 0, sizeof spawn_acc);
    SDL_memset(goal_flag, 0, sizeof goal_flag);
    if (start_scene_loaded) {
        SimP *s = scene_instantiate(&start_scene, MAX_AGENTS);
        SDL_memcpy(spawn_flag, start_scene.spawn, sizeof spawn_flag);
        SDL_memcpy(goal_flag, start_scene.goal, sizeof goal_flag);
        return s;
    }
    SimP *s = simp_create(GW, GH, CELL, MAX_AGENTS);
    simp_terrain_commit(s);
    return s;
}

/* F2: snapshot the painted world back into a scene file. Dormant agents
 * become pack cells; the main live knobs are saved as set directives.
 * Painted user cost is not saved (see scene.h). */
static void scene_snapshot(SimP *s) {
    Scene out;
    if (scene_alloc(&out, GW, GH, CELL) != 0) return;
    for (int cy = 0; cy < GH; cy++)
        for (int cx = 0; cx < GW; cx++) {
            int i = cy * GW + cx;
            out.wall[i]  = simp_is_wall(s, cx, cy) ? 1 : 0;
            out.goal[i]  = goal_flag[i];
            out.spawn[i] = spawn_flag[i];
        }
    const float *apx = simp_px(s), *apy = simp_py(s);
    const uint8_t *afl = simp_flags_arr(s);
    for (int i = 0; i < simp_count(s); i++) {
        if (!(afl[i] & SIMP_DORMANT)) continue;
        int cx = (int)(apx[i] / CELL), cy = (int)(apy[i] / CELL);
        if (cx >= 0 && cy >= 0 && cx < GW && cy < GH)
            out.pack[cy * GW + cx] = 1;
    }
    const SimPParams *P = simp_params(s);
    out.n_set = 5;
    SDL_strlcpy(out.set[0].name, "pbd_iters", sizeof out.set[0].name);
    out.set[0].value = (float)P->pbd_iters;
    SDL_strlcpy(out.set[1].name, "noise_ang", sizeof out.set[1].name);
    out.set[1].value = P->noise_ang;
    SDL_strlcpy(out.set[2].name, "v_max", sizeof out.set[2].name);
    out.set[2].value = P->v_max;
    SDL_strlcpy(out.set[3].name, "k_density", sizeof out.set[3].name);
    out.set[3].value = P->k_density;
    SDL_strlcpy(out.set[4].name, "k_jam", sizeof out.set[4].name);
    out.set[4].value = P->k_jam;
    const char *path = scene_path ? scene_path : "scene_saved.txt";
    if (scene_save(path, &out) == 0) SDL_Log("scene saved to %s", path);
    else                             SDL_Log("scene save FAILED (%s)", path);
    scene_free(&out);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (scene_load(argv[1], &start_scene) != 0) {
            fprintf(stderr, "cannot load scene '%s'\n", argv[1]);
            return 1;
        }
        if (start_scene.gw != GW || start_scene.gh != GH ||
            fabsf(start_scene.cell - CELL) > 1e-6f) {
            fprintf(stderr, "scene is %dx%d cell %.2f, sandbox wants %dx%d cell %.2f\n",
                    start_scene.gw, start_scene.gh, (double)start_scene.cell,
                    GW, GH, (double)CELL);
            return 1;
        }
        start_scene_loaded = 1;
        scene_path = argv[1];
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) { SDL_Log("init: %s", SDL_GetError()); return 1; }
    SDL_Window   *win = SDL_CreateWindow("Horde particle sandbox", WINW, WINH, 0);
    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    SDL_Texture  *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                          SDL_TEXTUREACCESS_STREAMING, GW, GH);
    if (!win || !ren || !tex) { SDL_Log("setup: %s", SDL_GetError()); return 1; }
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    SimP *s = scene_create();
    static Uint32 terrain_px[GW * GH];
    static SDL_FRect rects[MAX_AGENTS];

    /* sprite layer: optional, the sandbox falls back to quads without it.
     * Every walkN sheet found becomes a per-agent variant (hash-assigned):
     * render more Mixamo walks as --anim walk2=... to de-uniform the horde. */
    static const char *const WALK_SHEETS[] = {
        "gfx/out/zombie/walk_sheet.zspr",
        "gfx/out/zombie/walk2_sheet.zspr",
        "gfx/out/zombie/walk3_sheet.zspr",
        "gfx/out/zombie/walk4_sheet.zspr",
        "gfx/out/zombie/walk5_sheet.zspr",
        "gfx/out/zombie/walk6_sheet.zspr",
        "gfx/out/zombie/walk7_sheet.zspr",
        "gfx/out/zombie/walk8_sheet.zspr",
    };
    SpriteLayer *sprites = sprite_layer_create(ren, WALK_SHEETS, 8, MAX_AGENTS);
    int sprites_on = sprites != NULL;
    if (sprites)   /* stuck/dormant agents idle instead of freezing */
        sprite_layer_set_stuck(sprites, ren, "gfx/out/zombie/idle_sheet.zspr");

    int   brush = B_WALL, size = 3;
    int   paused = 0, running = 1, spawning = 1;
    int   speed_tint = 1, show_flow = 0, show_density = 0;
    int   painting = 0, erasing = 0;
    int   want_shot = 0;
    float boom_radius = 6.0f, boom_strength = 10.0f, boom_up = 0.5f;
    long  drained_total = 0;
    double step_ms = 0.0;

    /* SANDBOX_SHOT="frame[,tier,camx,camy]": unattended visual check —
     * run N frames, optionally move the camera, save sandbox_shot.bmp, quit */
    long shot_at = -1, frame_no = 0;
    int shot_tier = -1; float shot_cx = 0.0f, shot_cy = 0.0f;
    if (getenv("SANDBOX_SHOT"))
        sscanf(getenv("SANDBOX_SHOT"), "%ld,%d,%f,%f",
               &shot_at, &shot_tier, &shot_cx, &shot_cy);

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_MIDDLE) {
                    boom(s, e.button.x / PPM + cam_x, e.button.y / PPM + cam_y,
                         boom_radius, boom_strength, boom_up);
                    break;
                }
                if (e.button.button == SDL_BUTTON_LEFT)  painting = 1;
                if (e.button.button == SDL_BUTTON_RIGHT) erasing = 1;
                paint(s, (int)((e.button.x / PPM + cam_x) / CELL),
                         (int)((e.button.y / PPM + cam_y) / CELL),
                      e.button.button == SDL_BUTTON_RIGHT ? B_ERASE : brush, size);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT)  painting = 0;
                if (e.button.button == SDL_BUTTON_RIGHT) erasing = 0;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (painting || erasing)
                    paint(s, (int)((e.motion.x / PPM + cam_x) / CELL),
                             (int)((e.motion.y / PPM + cam_y) / CELL),
                          erasing ? B_ERASE : brush, size);
                break;
            case SDL_EVENT_KEY_DOWN: {
                SimPParams *P = simp_params(s);
                switch (e.key.key) {
                case SDLK_ESCAPE: running = 0; break;
                case SDLK_1: brush = B_WALL;  break;
                case SDLK_2: brush = B_SPAWN; break;
                case SDLK_3: brush = B_GOAL;  break;
                case SDLK_4: brush = B_ERASE; break;
                case SDLK_5: brush = B_PACK;  break;
                case SDLK_6: brush = B_KILL;  break;
                case SDLK_G: brush = B_COSTP; break;
                case SDLK_H: brush = B_COSTM; break;
                case SDLK_X: simp_clear_cost(s); break;
                case SDLK_M: spawn_type = (spawn_type + 1) % 4; break;
                case SDLK_K:
                    P->k_density = fmaxf(0.0f, P->k_density - 0.5f);
                    simp_terrain_commit(s);   /* apply now, even toward 0 */
                    break;
                case SDLK_L:
                    P->k_density = fminf(6.0f, P->k_density + 0.5f);
                    simp_terrain_commit(s);
                    break;
                case SDLK_A:
                    P->k_jam = fmaxf(0.0f, P->k_jam - 1.0f);
                    simp_terrain_commit(s);   /* apply now, even toward 0 */
                    break;
                case SDLK_S:
                    P->k_jam = fminf(16.0f, P->k_jam + 1.0f);
                    simp_terrain_commit(s);
                    break;
                case SDLK_O: show_density = (show_density + 1) % 3; break;
                case SDLK_W: simp_wake_all(s); break;
                case SDLK_7: boom_up = fmaxf(0.0f, boom_up - 0.1f); break;
                case SDLK_U: boom_up = fminf(2.0f, boom_up + 0.1f); break;
                case SDLK_LEFTBRACKET:  if (size > 0)  size--; break;
                case SDLK_RIGHTBRACKET: if (size < 20) size++; break;
                case SDLK_MINUS:
                    if (P->pbd_iters > 1)  P->pbd_iters--;
                    break;
                case SDLK_EQUALS: case SDLK_PLUS:
                    if (P->pbd_iters < 12) P->pbd_iters++;
                    break;
                case SDLK_COMMA:  P->noise_ang = fmaxf(0.0f,  P->noise_ang - 0.02f); break;
                case SDLK_PERIOD: P->noise_ang = fminf(0.60f, P->noise_ang + 0.02f); break;
                case SDLK_SEMICOLON:  P->v_max = fmaxf(0.2f, P->v_max - 0.2f); break;
                case SDLK_APOSTROPHE: P->v_max = fminf(6.0f, P->v_max + 0.2f); break;
                case SDLK_9: boom_radius = fmaxf(1.0f,  boom_radius - 1.0f); break;
                case SDLK_0: boom_radius = fminf(25.0f, boom_radius + 1.0f); break;
                case SDLK_8: boom_strength = fmaxf(1.0f,  boom_strength - 2.0f); break;
                case SDLK_I: boom_strength = fminf(40.0f, boom_strength + 2.0f); break;
                case SDLK_E: {
                    float mx, my; SDL_GetMouseState(&mx, &my);
                    boom(s, mx / PPM + cam_x, my / PPM + cam_y,
                         boom_radius, boom_strength, boom_up);
                    break;
                }
                case SDLK_Z: {
                    /* next tier, keeping the world point under the cursor
                     * fixed on screen */
                    float mx, my; SDL_GetMouseState(&mx, &my);
                    float wx = mx / PPM + cam_x, wy = my / PPM + cam_y;
                    cam_tier = (cam_tier + 1) % 3;
                    cam_x = wx - mx / PPM;
                    cam_y = wy - my / PPM;
                    cam_clamp();
                    break;
                }
                case SDLK_B: sprites_on = !sprites_on; break;
                case SDLK_D:
                    floor_idx = (floor_idx + 1) %
                                (int)(sizeof FLOOR_PAL / sizeof FLOOR_PAL[0]);
                    break;
                case SDLK_F3: want_shot = 1; break;
                case SDLK_SPACE: paused = !paused; break;
                case SDLK_N:
                    if (paused) {
                        if (spawning) spawners_step(s, DT);
                        drained_total += simp_step(s, DT);
                        if (sprites) sprite_layer_update(sprites, s, DT);
                    }
                    break;
                case SDLK_T: spawning = !spawning; break;
                case SDLK_V: speed_tint = !speed_tint; break;
                case SDLK_F: show_flow = !show_flow; break;
                case SDLK_C:
                    for (int i = simp_count(s) - 1; i >= 0; i--) simp_kill(s, i);
                    break;
                case SDLK_R:
                    simp_destroy(s); s = scene_create();
                    drained_total = 0;
                    break;
                case SDLK_F2: scene_snapshot(s); break;
                default: break;
                }
                break;
            }
            default: break;
            }
        }

        if (!paused) {
            if (spawning) spawners_step(s, DT);
            Uint64 t0 = SDL_GetPerformanceCounter();
            drained_total += simp_step(s, DT);
            Uint64 t1 = SDL_GetPerformanceCounter();
            step_ms = 1000.0 * (double)(t1 - t0) / (double)SDL_GetPerformanceFrequency();
            if (sprites) sprite_layer_update(sprites, s, DT);
        }

        /* viewport pan (held keys, ~10 px per frame regardless of tier) */
        {
            const bool *ks = SDL_GetKeyboardState(NULL);
            float pan = 600.0f * DT / PPM;
            if (ks[SDL_SCANCODE_LEFT])  cam_x -= pan;
            if (ks[SDL_SCANCODE_RIGHT]) cam_x += pan;
            if (ks[SDL_SCANCODE_UP])    cam_y -= pan;
            if (ks[SDL_SCANCODE_DOWN])  cam_y += pan;
            cam_clamp();
        }

        /* unattended screenshot hook (see SANDBOX_SHOT above) */
        if (shot_at >= 0 && frame_no++ == shot_at) {
            if (shot_tier >= 0 && shot_tier < 3) {
                cam_tier = shot_tier;
                cam_x = shot_cx; cam_y = shot_cy;
                cam_clamp();
            }
            want_shot = 2;                      /* 2 = quit after saving */
        }

        /* terrain layer (low-res texture, scaled by the camera transform) */
        terrain_render(s, terrain_px, show_density);
        for (int i = 0; i < GW * GH; i++)
            if (goal_flag[i]) terrain_px[i] = 0xFF3C78FFu;   /* goal: blue */
        SDL_UpdateTexture(tex, NULL, terrain_px, GW * (int)sizeof(Uint32));
        SDL_RenderClear(ren);
        SDL_FRect tdst = { -cam_x * PPM, -cam_y * PPM,
                           GW * CELL * PPM, GH * CELL * PPM };
        SDL_RenderTexture(ren, tex, NULL, &tdst);

        /* flow-field overlay: one segment every 4 nav cells */
        if (show_flow) {
            SDL_SetRenderDrawColor(ren, 70, 160, 170, 255);
            for (int cy = 2; cy < GH - 2; cy += 4)
            for (int cx = 2; cx < GW - 2; cx += 4) {
                float wx = ((float)cx + 0.5f) * CELL;
                float wy = ((float)cy + 0.5f) * CELL;
                float dx, dy;
                simp_sample_flow(s, wx, wy, &dx, &dy);
                if (dx == 0.0f && dy == 0.0f) continue;
                float x0 = (wx - cam_x) * PPM, y0 = (wy - cam_y) * PPM;
                SDL_RenderLine(ren, x0, y0, x0 + dx * 1.4f * PPM, y0 + dy * 1.4f * PPM);
            }
        }

        /* corpses: dark static discs under everything else */
        {
            int nc = simp_corpse_count(s);
            const float *cpx = simp_corpse_px(s), *cpy = simp_corpse_py(s);
            const float *crad = simp_corpse_rad(s);
            for (int j = 0; j < nc; j++) {
                float rp = crad[j] * PPM;
                rects[j].x = (cpx[j] - cam_x) * PPM - rp;
                rects[j].y = (cpy[j] - cam_y) * PPM - rp;
                rects[j].w = 2 * rp; rects[j].h = 2 * rp;
            }
            SDL_SetRenderDrawColor(ren, 70, 60, 58, 255);
            SDL_RenderFillRects(ren, rects, nc);
        }

        /* agents. Sprite tiers (16/32 px/m) draw the sprite layer; the
         * full-map tier and the no-sheet fallback draw batched rects sized
         * by radius (flying ones scale up with altitude). Rect passes:
         * 0 = dormant (always its own color), 1 = awake slow (or all awake
         * when tint is off), 2 = awake fast (speed tint only). */
        int n_dormant = 0;
        {
            const uint8_t *afl = simp_flags_arr(s);
            int n = simp_count(s);
            for (int i = 0; i < n; i++)
                if (afl[i] & SIMP_DORMANT) n_dormant++;
        }
        if (sprites && sprites_on && cam_tier > 0) {
            sprite_layer_draw(sprites, ren, s, cam_x, cam_y, PPM);
        } else {
            const float *apx = simp_px(s), *apy = simp_py(s);
            const float *avx = simp_vx(s), *avy = simp_vy(s);
            const float *ar  = simp_radius_arr(s);
            const float *az  = simp_z_arr(s);
            const uint8_t *afl = simp_flags_arr(s);
            int n = simp_count(s);
            int passes = speed_tint ? 3 : 2;
            for (int pass = 0; pass < passes; pass++) {
                int m = 0;
                for (int i = 0; i < n; i++) {
                    int p;
                    if (afl[i] & SIMP_DORMANT) p = 0;
                    else if (!speed_tint) p = 1;
                    else if (afl[i] & SIMP_FLYING) p = 2;
                    else {
                        float sp = sqrtf(avx[i] * avx[i] + avy[i] * avy[i]);
                        p = sp > 1.6f ? 2 : 1;
                    }
                    if (p != pass) continue;
                    float rp = ar[i] * PPM;
                    if (afl[i] & SIMP_FLYING) rp *= 1.0f + 0.12f * az[i];
                    rects[m].x = (apx[i] - cam_x) * PPM - rp;
                    rects[m].y = (apy[i] - cam_y) * PPM - rp;
                    rects[m].w = 2 * rp; rects[m].h = 2 * rp;
                    m++;
                }
                if (pass == 0) {
                    SDL_SetRenderDrawColor(ren, 110, 125, 160, 255);
                } else if (pass == 1 && speed_tint) {
                    Uint8 r, g, b; speed_ramp(0.25f, &r, &g, &b);
                    SDL_SetRenderDrawColor(ren, r, g, b, 255);
                } else if (pass == 2) {
                    Uint8 r, g, b; speed_ramp(1.0f, &r, &g, &b);
                    SDL_SetRenderDrawColor(ren, r, g, b, 255);
                } else {
                    SDL_SetRenderDrawColor(ren, 210, 80, 50, 255);
                }
                SDL_RenderFillRects(ren, rects, m);
            }
        }

        /* read back BEFORE present: after it the backbuffer is undefined */
        if (want_shot) {
            SDL_Surface *shot = SDL_RenderReadPixels(ren, NULL);
            if (shot) {
                SDL_SaveBMP(shot, "sandbox_shot.bmp");
                SDL_DestroySurface(shot);
                SDL_Log("screenshot: sandbox_shot.bmp");
            } else SDL_Log("screenshot failed: %s", SDL_GetError());
            if (want_shot == 2) running = 0;
            want_shot = 0;
        }
        SDL_RenderPresent(ren);

        SimPParams *P = simp_params(s);
        char title[256];
        snprintf(title, sizeof title,
                 "Horde particles | %.0fppm | brush:%s sz:%d type:%s | n:%d sleep:%d "
                 "corpses:%d drained:%ld | pbd:%d noise:%.2f vmax:%.1f "
                 "kd:%.1f kj:%.0f | boom r:%.0f s:%.0f up:%.1f | %.2f ms %s%s%s",
                 (double)PPM, BRUSH_NAME[brush], size, TYPE_NAME[spawn_type],
                 simp_count(s), n_dormant,
                 simp_corpse_count(s), drained_total,
                 P->pbd_iters, (double)P->noise_ang, (double)P->v_max,
                 (double)P->k_density, (double)P->k_jam,
                 (double)boom_radius, (double)boom_strength, (double)boom_up,
                 step_ms, paused ? "PAUSED" : "running",
                 spawning ? "" : " SPAWN-OFF",
                 sprites && sprites_on ? "" : " QUADS");
        SDL_SetWindowTitle(win, title);
        SDL_Delay(8);
    }

    sprite_layer_destroy(sprites);
    simp_destroy(s);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

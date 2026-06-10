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
 *     W              wake all sleepers ("sunrise")
 *     [ / ]          brush size down / up
 *     - / =          pbd_iters down / up        (crowd stiffness)
 *     , / .          noise_ang down / up        (steering noise)
 *     ; / '          v_max down / up            (desired speed)
 *     9 / 0          impulse radius down / up
 *     8 / I          impulse strength down / up
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
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Nav grid: 160x120 cells of 0.5 m -> 80 m x 60 m world. */
#define GW   160
#define GH   120
#define CELL 0.5f
#define PPM  12.0f                       /* pixels per meter */
#define WINW ((int)(GW * CELL * PPM))    /* 960 */
#define WINH ((int)(GH * CELL * PPM))    /* 720 */

#define MAX_AGENTS      30000
#define SPAWN_RATE      1.5f             /* agents/sec per spawner cell */
#define DT              (1.0f / 60.0f)

enum { B_WALL, B_SPAWN, B_GOAL, B_ERASE, B_PACK };
static const char *BRUSH_NAME[] = { "WALL", "SPAWN", "GOAL", "ERASE", "PACK" };

/* Spawners live in the sandbox, not in the core: the core only knows walls
 * and goals. spawn_acc accumulates fractional spawns per cell. */
static uint8_t spawn_flag[GW * GH];
static float   spawn_acc[GW * GH];

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

/* conservative spawn-admission radius: the largest radius simp_spawn can roll */
static float spawn_rmax(SimP *s) {
    const SimPParams *P = simp_params(s);
    return P->radius * (1.0f + P->r_jitter);
}

static void speed_ramp(float t, Uint8 *r, Uint8 *g, Uint8 *b) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    *r = (Uint8)(200 + 55 * t);
    *g = (Uint8)(60 + 160 * t);
    *b = 50;
}

static void terrain_render(const SimP *s, Uint32 *px) {
    for (int cy = 0; cy < GH; cy++)
    for (int cx = 0; cx < GW; cx++) {
        int i = cy * GW + cx;
        Uint8 r = 20, g = 24, b = 22;                       /* dark floor   */
        if (simp_is_wall(s, cx, cy))      { r = 80; g = 80; b = 95; }
        else if (spawn_flag[i])           { r = 90; g = 40; b = 40; }
        px[i] = 0xFF000000u | (Uint32)(r << 16) | (Uint32)(g << 8) | b;
    }
}

/* Goal cells are core-side state with no public read access, so the sandbox
 * mirrors them for rendering. */
static uint8_t goal_flag[GW * GH];

static void paint(SimP *s, int cx, int cy, int brush, int size) {
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
            break;
        case B_PACK: {
            /* one-shot placement, no emitter state: one jittered attempt per
             * cell per paint event; simp_free_at makes repainting idempotent
             * (it only fills the gaps, up to packing density) */
            float wx = ((float)x + frand()) * CELL;
            float wy = ((float)y + frand()) * CELL;
            if (simp_free_at(s, wx, wy, spawn_rmax(s)))
                simp_spawn_dormant(s, wx, wy);
            break;
        }
        }
    }
}

static void spawners_step(SimP *s, float dt) {
    /* Emitters ask for clearance beyond the agent radius: in a congested
     * mouth the neighbours are already overlapping each other under crowd
     * pressure, and a newborn admitted into an exactly-fitting pocket
     * becomes the pressure relief valve (it pops out at high speed). The
     * cushion makes the emitter throttle a bit earlier instead. */
    float rchk = spawn_rmax(s) + 0.5f * simp_params(s)->radius;
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
            float x = ((float)cx + frand()) * CELL;
            float y = ((float)cy + frand()) * CELL;
            /* only emit into free space: no overlap -> no PBD ejection.
             * If the mouth is congested the credit is forfeited, so the
             * emitter self-throttles to the drain rate of the exit. */
            if (!simp_free_at(s, x, y, rchk)) continue;
            if (simp_spawn(s, x, y) < 0) return;     /* at capacity */
        }
    }
}

static void boom(SimP *s, float x, float y, float radius, float strength) {
    simp_apply_impulse(s, x, y, radius, strength);
    /* the blast is heard beyond where it is felt: wake a wider circle */
    simp_wake_radius(s, x, y, radius * 2.0f);
}

/* Start from an empty world: no walls, no goals, no spawners, no agents.
 * Paint everything with the brushes (like the first sandbox). */
static SimP *scene_create(void) {
    SimP *s = simp_create(GW, GH, CELL, MAX_AGENTS);
    SDL_memset(spawn_flag, 0, sizeof spawn_flag);
    SDL_memset(spawn_acc, 0, sizeof spawn_acc);
    SDL_memset(goal_flag, 0, sizeof goal_flag);
    simp_terrain_commit(s);
    return s;
}

int main(void) {
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

    int   brush = B_WALL, size = 3;
    int   paused = 0, running = 1, spawning = 1;
    int   speed_tint = 1, show_flow = 0;
    int   painting = 0, erasing = 0;
    float boom_radius = 6.0f, boom_strength = 10.0f;
    long  drained_total = 0;
    double step_ms = 0.0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = 0; break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_MIDDLE) {
                    boom(s, e.button.x / PPM, e.button.y / PPM,
                         boom_radius, boom_strength);
                    break;
                }
                if (e.button.button == SDL_BUTTON_LEFT)  painting = 1;
                if (e.button.button == SDL_BUTTON_RIGHT) erasing = 1;
                paint(s, (int)(e.button.x / (CELL * PPM)),
                         (int)(e.button.y / (CELL * PPM)),
                      e.button.button == SDL_BUTTON_RIGHT ? B_ERASE : brush, size);
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT)  painting = 0;
                if (e.button.button == SDL_BUTTON_RIGHT) erasing = 0;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (painting || erasing)
                    paint(s, (int)(e.motion.x / (CELL * PPM)),
                             (int)(e.motion.y / (CELL * PPM)),
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
                case SDLK_W: simp_wake_all(s); break;
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
                    boom(s, mx / PPM, my / PPM, boom_radius, boom_strength);
                    break;
                }
                case SDLK_SPACE: paused = !paused; break;
                case SDLK_N:
                    if (paused) {
                        if (spawning) spawners_step(s, DT);
                        drained_total += simp_step(s, DT);
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
        }

        /* terrain layer (low-res texture, nearest-scaled to window) */
        terrain_render(s, terrain_px);
        for (int i = 0; i < GW * GH; i++)
            if (goal_flag[i]) terrain_px[i] = 0xFF3C78FFu;   /* goal: blue */
        SDL_UpdateTexture(tex, NULL, terrain_px, GW * (int)sizeof(Uint32));
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, NULL, NULL);

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
                float x0 = wx * PPM, y0 = wy * PPM;
                SDL_RenderLine(ren, x0, y0, x0 + dx * 1.4f * PPM, y0 + dy * 1.4f * PPM);
            }
        }

        /* agents: batched rects sized by radius. Pass 0 = dormant (always
         * its own color), pass 1 = awake slow (or all awake when tint is
         * off), pass 2 = awake fast (speed tint only). */
        int n_dormant = 0;
        {
            const float *apx = simp_px(s), *apy = simp_py(s);
            const float *avx = simp_vx(s), *avy = simp_vy(s);
            const float *ar  = simp_radius_arr(s);
            const uint8_t *dor = simp_dormant_arr(s);
            int n = simp_count(s);
            int passes = speed_tint ? 3 : 2;
            for (int pass = 0; pass < passes; pass++) {
                int m = 0;
                for (int i = 0; i < n; i++) {
                    int p;
                    if (dor[i]) p = 0;
                    else if (!speed_tint) p = 1;
                    else {
                        float sp = sqrtf(avx[i] * avx[i] + avy[i] * avy[i]);
                        p = sp > 1.6f ? 2 : 1;
                    }
                    if (p != pass) continue;
                    float rp = ar[i] * PPM;
                    rects[m].x = apx[i] * PPM - rp;
                    rects[m].y = apy[i] * PPM - rp;
                    rects[m].w = 2 * rp; rects[m].h = 2 * rp;
                    m++;
                }
                if (pass == 0) {
                    n_dormant = m;
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

        SDL_RenderPresent(ren);

        SimPParams *P = simp_params(s);
        char title[256];
        snprintf(title, sizeof title,
                 "Horde particles | brush:%s sz:%d | n:%d sleep:%d drained:%ld | "
                 "pbd:%d noise:%.2f vmax:%.1f | boom r:%.0f s:%.0f | "
                 "%.2f ms %s%s",
                 BRUSH_NAME[brush], size, simp_count(s), n_dormant, drained_total,
                 P->pbd_iters, (double)P->noise_ang, (double)P->v_max,
                 (double)boom_radius, (double)boom_strength,
                 step_ms, paused ? "PAUSED" : "running",
                 spawning ? "" : " SPAWN-OFF");
        SDL_SetWindowTitle(win, title);
        SDL_Delay(8);
    }

    simp_destroy(s);
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

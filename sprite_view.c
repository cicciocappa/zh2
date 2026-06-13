/* sprite_view.c — standalone ZSP3 animation previewer (GFX_DESIGN tooling).
 *
 * Plays one or more .zspr sheets at their native duration so you can judge an
 * animation (e.g. "is the idle alive?") that the static PNG contact sheet can't
 * show. No dependency on the sim core: it only parses the ZSP3 header and blits
 * frames, mirroring sprite_layer.c's loader and src-rect math.
 *
 *   ./sprite_view gfx/out/zombie/idle_sheet.zspr [more.zspr ...]
 *
 * Controls:
 *   Space        play / pause
 *   , .          step one frame back / forward (pauses)
 *   Left Right   change facing direction (sheet row)
 *   - = (or +)   slower / faster playback
 *   wheel        zoom; arrows already used, so pan is unneeded (sprite centred)
 *   Tab          next loaded sheet (cycles)
 *   B            cycle background (checker / black / magenta / white / grey)
 *   G            toggle anchor cross + ground line
 *   A            toggle single view <-> all-directions montage
 *   R            reset (dir 0, frame 0, speed 1, zoom default)
 *   Esc / Q      quit
 *
 * Headless filmstrip (for review without a display):
 *   SPRITE_VIEW_SHOT="[dir]" ./sprite_view sheet.zspr
 *     renders every frame of <dir> (default 0) in a row to
 *     sprite_view_shot.bmp on a checker background, then exits.
 */
#define _POSIX_C_SOURCE 199309L
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char  name[256];
    SDL_Texture *tex;
    int   sheet_w, sheet_h, frame_px, dirs, frames;
    float anchor_x, anchor_y;     /* px from frame cell top-left */
    float px_per_m, k_z, stride_m, duration_s;
} Sheet;

/* ZSP3 = "ZSP3" + 5 u32 (w,h,frame_px,dirs,frames) + 6 f32
 * (anchor_x,anchor_y,px_per_m,k_z,stride_m,duration_s) + RGBA raw.
 * Same layout sprite_layer.c reads. */
static int load_sheet(SDL_Renderer *ren, const char *path, Sheet *sh) {
    FILE *f = fopen(path, "rb");
    if (!f) { SDL_Log("no %s", path); return -1; }
    char magic[4]; uint32_t u[5]; float fl[6];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "ZSP3", 4) != 0 ||
        fread(u, 4, 5, f) != 5 || fread(fl, 4, 6, f) != 6) {
        SDL_Log("%s is not a ZSP3 sheet", path); fclose(f); return -1;
    }
    sh->sheet_w = (int)u[0]; sh->sheet_h = (int)u[1];
    sh->frame_px = (int)u[2]; sh->dirs = (int)u[3]; sh->frames = (int)u[4];
    sh->anchor_x = fl[0]; sh->anchor_y = fl[1]; sh->px_per_m = fl[2];
    sh->k_z = fl[3]; sh->stride_m = fl[4]; sh->duration_s = fl[5];

    size_t npx = (size_t)sh->sheet_w * sh->sheet_h;
    void *pixels = malloc(npx * 4);
    if (!pixels || fread(pixels, 4, npx, f) != npx) {
        SDL_Log("%s truncated", path); fclose(f); free(pixels); return -1;
    }
    fclose(f);
    sh->tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STATIC, sh->sheet_w, sh->sheet_h);
    if (!sh->tex) { SDL_Log("texture: %s", SDL_GetError()); free(pixels); return -1; }
    SDL_UpdateTexture(sh->tex, NULL, pixels, sh->sheet_w * 4);
    SDL_SetTextureBlendMode(sh->tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(sh->tex, SDL_SCALEMODE_LINEAR);
    free(pixels);
    const char *base = strrchr(path, '/');
    snprintf(sh->name, sizeof sh->name, "%s", base ? base + 1 : path);
    return 0;
}

/* src rect for (dir row, frame col), as in sprite_layer.c */
static SDL_FRect cell_src(const Sheet *sh, int dir, int frame) {
    SDL_FRect s = { (float)(frame * sh->frame_px), (float)(dir * sh->frame_px),
                    (float)sh->frame_px, (float)sh->frame_px };
    return s;
}

static const SDL_Color BGS[] = {
    {  40,  40,  48, 255 },   /* checker base (light cells lighter) */
    {   0,   0,   0, 255 },   /* black            */
    { 255,   0, 255, 255 },   /* magenta (alpha)  */
    { 235, 235, 235, 255 },   /* white            */
    { 120, 120, 120, 255 },   /* mid grey         */
};
#define NBG ((int)(sizeof BGS / sizeof BGS[0]))

static void draw_background(SDL_Renderer *ren, int bg, int vw, int vh) {
    SDL_Color c = BGS[bg];
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
    SDL_RenderClear(ren);
    if (bg == 0) {                 /* checker */
        const int cell = 24;
        SDL_SetRenderDrawColor(ren, 60, 60, 70, 255);
        for (int y = 0; y < vh; y += cell)
            for (int x = ((y / cell) & 1) * cell; x < vw; x += 2 * cell) {
                SDL_FRect r = { (float)x, (float)y, (float)cell, (float)cell };
                SDL_RenderFillRect(ren, &r);
            }
    }
}

/* draw one frame so the sheet anchor lands at (ax, ay) on screen, scaled. */
static void draw_frame(SDL_Renderer *ren, const Sheet *sh, int dir, int frame,
                       float ax, float ay, float side, int show_anchor) {
    SDL_FRect src = cell_src(sh, dir, frame);
    SDL_FRect dst = { ax - sh->anchor_x / (float)sh->frame_px * side,
                      ay - sh->anchor_y / (float)sh->frame_px * side,
                      side, side };
    SDL_RenderTexture(ren, sh->tex, &src, &dst);
    if (show_anchor) {
        SDL_SetRenderDrawColor(ren, 0, 220, 120, 200);
        SDL_RenderLine(ren, dst.x, ay, dst.x + side, ay);          /* ground */
        SDL_RenderLine(ren, ax, ay - 8, ax, ay + 8);               /* anchor */
        SDL_FRect cell = { dst.x, dst.y, side, side };
        SDL_SetRenderDrawColor(ren, 90, 90, 110, 120);
        SDL_RenderRect(ren, &cell);                                /* frame box */
    }
}

/* headless: filmstrip of every frame of `dir` -> sprite_view_shot.bmp */
static int shot_filmstrip(SDL_Renderer *ren, const Sheet *sh, int dir) {
    int cell = sh->frame_px * 2;            /* 2x source for a readable strip */
    if (cell > 256) cell = 256;
    int vw = cell * sh->frames, vh = cell;
    SDL_Texture *rt = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
                                        SDL_TEXTUREACCESS_TARGET, vw, vh);
    if (!rt) { SDL_Log("rt: %s", SDL_GetError()); return 1; }
    SDL_SetRenderTarget(ren, rt);
    draw_background(ren, 0, vw, vh);
    for (int fr = 0; fr < sh->frames; fr++) {
        float ax = fr * cell + cell * 0.5f;
        float side = (float)cell / sh->frame_px * sh->frame_px; /* = cell-ish */
        side = (float)cell;
        draw_frame(ren, sh, dir, fr, ax, vh * 0.92f, side, 1);
    }
    SDL_RenderPresent(ren);
    SDL_Surface *s = SDL_RenderReadPixels(ren, NULL);
    SDL_SetRenderTarget(ren, NULL);
    if (!s) { SDL_Log("readpixels: %s", SDL_GetError()); return 1; }
    int ok = SDL_SaveBMP(s, "sprite_view_shot.bmp");
    SDL_DestroySurface(s);
    SDL_Log("filmstrip dir %d, %d frames -> sprite_view_shot.bmp", dir, sh->frames);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.zspr [more.zspr ...]\n", argv[0]);
        return 2;
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init: %s", SDL_GetError()); return 1;
    }
    SDL_Window *win;
    SDL_Renderer *ren;
    if (!SDL_CreateWindowAndRenderer("sprite_view", 900, 720, 0, &win, &ren)) {
        SDL_Log("window: %s", SDL_GetError()); return 1;
    }

    Sheet sheets[32];
    int n = 0;
    for (int i = 1; i < argc && n < 32; i++)
        if (load_sheet(ren, argv[i], &sheets[n]) == 0) n++;
    if (n == 0) { SDL_Log("no valid sheets"); return 1; }

    const char *shot = getenv("SPRITE_VIEW_SHOT");
    if (shot) {
        int dir = atoi(shot);                       /* "" -> 0 */
        if (dir < 0 || dir >= sheets[0].dirs) dir = 0;
        int rc = shot_filmstrip(ren, &sheets[0], dir);
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
        return rc;
    }

    int cur = 0, dir = 0, bg = 0, anchor = 1, montage = 0, paused = 0;
    float phase = 0.0f, speed = 1.0f, zoom = 0.0f;  /* zoom 0 = auto-fit */
    Uint64 prev = SDL_GetTicks();

    for (int running = 1; running; ) {
        Sheet *sh = &sheets[cur];
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = 0;
            else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                if (zoom == 0.0f) zoom = 6.0f;       /* leave auto-fit */
                zoom *= (e.wheel.y > 0) ? 1.1f : 1.0f / 1.1f;
                if (zoom < 0.5f) zoom = 0.5f;
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.key) {
                case SDLK_ESCAPE: case SDLK_Q: running = 0; break;
                case SDLK_SPACE: paused = !paused; break;
                case SDLK_PERIOD:
                    paused = 1; phase += 1.0f / sh->frames; break;
                case SDLK_COMMA:
                    paused = 1; phase -= 1.0f / sh->frames; break;
                case SDLK_RIGHT: dir = (dir + 1) % sh->dirs; break;
                case SDLK_LEFT:  dir = (dir + sh->dirs - 1) % sh->dirs; break;
                case SDLK_MINUS: speed *= 0.8f; break;
                case SDLK_EQUALS: case SDLK_PLUS: speed *= 1.25f; break;
                case SDLK_TAB:
                    cur = (cur + 1) % n; dir %= sheets[cur].dirs; phase = 0; break;
                case SDLK_B: bg = (bg + 1) % NBG; break;
                case SDLK_G: anchor = !anchor; break;
                case SDLK_A: montage = !montage; break;
                case SDLK_R:
                    dir = 0; phase = 0; speed = 1.0f; zoom = 0.0f; break;
                default: break;
                }
            }
        }

        Uint64 now = SDL_GetTicks();
        float dt = (now - prev) / 1000.0f;
        prev = now;
        float dur = sh->duration_s > 0.05f ? sh->duration_s : sh->frames / 12.0f;
        if (!paused) phase += dt * speed / dur;
        phase -= floorf(phase);
        int frame = (int)(phase * sh->frames) % sh->frames;
        if (frame < 0) frame += sh->frames;

        int vw, vh; SDL_GetRenderOutputSize(ren, &vw, &vh);
        draw_background(ren, bg, vw, vh);

        if (montage) {
            int cols = (int)ceilf(sqrtf((float)sh->dirs));
            int rows = (sh->dirs + cols - 1) / cols;
            float cw = (float)vw / cols, ch = (float)vh / rows;
            float side = (cw < ch ? cw : ch) * 0.82f;
            for (int d = 0; d < sh->dirs; d++) {
                float cx = (d % cols + 0.5f) * cw;
                float cy = (d / cols + 0.85f) * ch;
                draw_frame(ren, sh, d, frame, cx, cy, side, anchor);
            }
        } else {
            float side = zoom > 0.0f ? sh->frame_px * zoom
                                     : (vh * 0.8f);       /* auto-fit height */
            float ax = vw * 0.5f, ay = vh * 0.9f;
            draw_frame(ren, sh, dir, frame, ax, ay, side, anchor);
        }

        SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
        char hud[256];
        snprintf(hud, sizeof hud,
                 "%s  [%d/%d]  dir %d/%d  frame %d/%d  %.2fs  speed %.2fx%s%s",
                 sh->name, cur + 1, n, dir, sh->dirs, frame, sh->frames,
                 (double)dur, (double)speed, paused ? "  PAUSED" : "",
                 montage ? "  MONTAGE" : "");
        SDL_RenderDebugText(ren, 8, 8, hud);
        SDL_RenderDebugText(ren, 8, 22,
            "space play  ,/. step  </> dir  -/= speed  Tab sheet  B bg  G anchor  A montage  R reset");
        SDL_RenderPresent(ren);
        SDL_Delay(8);
    }

    for (int i = 0; i < n; i++) SDL_DestroyTexture(sheets[i].tex);
    SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}

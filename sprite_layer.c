/* sprite_layer.c — see sprite_layer.h. */
#include "sprite_layer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Heading EMA time constant (s): GFX_DESIGN.md suggests 0.2-0.3. */
#define HEADING_TAU     0.25f
/* Mean seconds between random gait-variant switches per WALKING agent
 * (dormant/standing agents never switch: a pose pop on a still zombie
 * reads as a glitch, in the moving crowd it disappears). Feel knob. */
#define VARIANT_SWITCH_T 15.0f
/* Stuck detection on the speed EMA, with hysteresis: under ENTER the agent
 * plays the stuck/idle sheet, back over EXIT it walks again. The gap and
 * the EMA keep the boundary from flickering under PBD jitter. */
#define SPEED_TAU        0.5f
#define STUCK_ENTER_MS   0.15f
#define STUCK_EXIT_MS    0.35f
/* Radius the sheets are authored for: scale = agent radius / this. */
#define SHEET_RADIUS_M  0.30f
#define MAX_VARIANTS    8
/* Cap on how fast the facing may rotate (rad/s). The EMA already smooths, but
 * a shove (or the EMA vector passing through the origin on a velocity reversal)
 * can snap the angle around; this clamp turns that into a slew. Generous: a
 * full turn in ~1 s, so legit corner turns are unaffected, only spins capped. */
#define MAX_TURN_RATE   6.0f

typedef struct { float key; int idx; } SprOrd;

typedef struct {
    SDL_Texture *tex;
    int   sheet_w, sheet_h, frame_px, dirs, frames;
    float anchor_x, anchor_y;     /* px from frame cell top-left */
    float px_per_m, k_z;
    float stride_m;               /* meters per animation cycle  */
    float duration_s;             /* native clip length          */
} SprSheet;

struct SpriteLayer {
    SprSheet v[MAX_VARIANTS];
    int n_var;
    SprSheet stuck;               /* optional idle/struggle sheet  */
    int has_stuck;
    SDL_Texture *shadow;
    int max_slots;
    /* per-slot renderer state, reset when the slot's handle changes */
    SimPHandle *seen;
    float  *hx, *hy;              /* heading EMA (velocity units)  */
    float  *spd;                  /* speed EMA (stuck detection)   */
    float  *phase;                /* anim cycles, fractional       */
    uint8_t *stuckf;              /* in stuck state (hysteresis)   */
    uint8_t *var;                 /* walk variant index            */
    uint8_t *tr, *tg, *tb;        /* per-agent tint                */
    SprOrd *order;                /* y-sort scratch                */
    uint32_t rng;                 /* xorshift for variant switches */
};

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static SDL_Texture *make_shadow_tex(SDL_Renderer *ren) {
    enum { N = 64 };
    static Uint32 px[N * N];
    for (int y = 0; y < N; y++)
    for (int x = 0; x < N; x++) {
        float dx = (x + 0.5f) / (N * 0.5f) - 1.0f;
        float dy = (y + 0.5f) / (N * 0.5f) - 1.0f;
        float d = sqrtf(dx * dx + dy * dy);
        float a = d >= 1.0f ? 0.0f : (1.0f - d) * (1.0f - d);
        px[y * N + x] = ((Uint32)(a * 160.0f) << 24);     /* black, alpha */
    }
    SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STATIC, N, N);
    if (!t) return NULL;
    SDL_UpdateTexture(t, NULL, px, N * (int)sizeof(Uint32));
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

static int load_sheet(SDL_Renderer *ren, const char *path, SprSheet *sh) {
    FILE *f = fopen(path, "rb");
    if (!f) { SDL_Log("sprite layer: no %s (skipped)", path); return -1; }
    char magic[4];
    uint32_t u[5];
    float fl[6];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "ZSP3", 4) != 0 ||
        fread(u, 4, 5, f) != 5 || fread(fl, 4, 6, f) != 6) {
        SDL_Log("sprite layer: %s is not a ZSP3 sheet (re-run sheet_pack.py)",
                path);
        fclose(f);
        return -1;
    }
    sh->sheet_w = (int)u[0]; sh->sheet_h = (int)u[1];
    sh->frame_px = (int)u[2]; sh->dirs = (int)u[3]; sh->frames = (int)u[4];
    sh->anchor_x = fl[0]; sh->anchor_y = fl[1];
    sh->px_per_m = fl[2]; sh->k_z = fl[3]; sh->stride_m = fl[4];
    sh->duration_s = fl[5];

    size_t npx = (size_t)sh->sheet_w * sh->sheet_h;
    void *pixels = malloc(npx * 4);
    if (!pixels || fread(pixels, 4, npx, f) != npx) {
        SDL_Log("sprite layer: %s truncated", path);
        fclose(f); free(pixels);
        return -1;
    }
    fclose(f);

    /* RGBA32 = byte-order RGBA, matching PIL's tobytes() */
    sh->tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STATIC,
                                sh->sheet_w, sh->sheet_h);
    if (!sh->tex) {
        SDL_Log("sprite layer: texture: %s", SDL_GetError());
        free(pixels);
        return -1;
    }
    SDL_UpdateTexture(sh->tex, NULL, pixels, sh->sheet_w * 4);
    SDL_SetTextureBlendMode(sh->tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(sh->tex, SDL_SCALEMODE_LINEAR);
    free(pixels);
    SDL_Log("sprite layer: %s (%d dirs x %d frames @ %d px, stride %.2f m)",
            path, sh->dirs, sh->frames, sh->frame_px, (double)sh->stride_m);
    return 0;
}

SpriteLayer *sprite_layer_create(SDL_Renderer *ren,
                                 const char *const *paths, int n_paths,
                                 int max_agents) {
    SpriteLayer *sl = calloc(1, sizeof *sl);
    for (int i = 0; i < n_paths && sl->n_var < MAX_VARIANTS; i++)
        if (load_sheet(ren, paths[i], &sl->v[sl->n_var]) == 0)
            sl->n_var++;
    if (sl->n_var == 0) { free(sl); return NULL; }

    sl->shadow = make_shadow_tex(ren);
    sl->rng = 0x9e3779b9u;
    sl->max_slots = max_agents;
    sl->seen  = calloc((size_t)max_agents, sizeof *sl->seen);
    sl->hx    = calloc((size_t)max_agents, sizeof *sl->hx);
    sl->hy    = calloc((size_t)max_agents, sizeof *sl->hy);
    sl->spd   = calloc((size_t)max_agents, sizeof *sl->spd);
    sl->phase = calloc((size_t)max_agents, sizeof *sl->phase);
    sl->stuckf = calloc((size_t)max_agents, 1);
    sl->var   = calloc((size_t)max_agents, 1);
    sl->tr    = calloc((size_t)max_agents, 1);
    sl->tg    = calloc((size_t)max_agents, 1);
    sl->tb    = calloc((size_t)max_agents, 1);
    sl->order = calloc((size_t)max_agents, sizeof *sl->order);
    return sl;
}

int sprite_layer_set_stuck(SpriteLayer *sl, SDL_Renderer *ren,
                           const char *path) {
    if (load_sheet(ren, path, &sl->stuck) != 0) return -1;
    sl->has_stuck = 1;
    return 0;
}

void sprite_layer_destroy(SpriteLayer *sl) {
    if (!sl) return;
    for (int i = 0; i < sl->n_var; i++)
        if (sl->v[i].tex) SDL_DestroyTexture(sl->v[i].tex);
    if (sl->has_stuck && sl->stuck.tex) SDL_DestroyTexture(sl->stuck.tex);
    if (sl->shadow) SDL_DestroyTexture(sl->shadow);
    free(sl->seen); free(sl->hx); free(sl->hy); free(sl->spd);
    free(sl->phase); free(sl->stuckf);
    free(sl->var); free(sl->tr); free(sl->tg); free(sl->tb); free(sl->order);
    free(sl);
}

void sprite_layer_update(SpriteLayer *sl, const SimP *s, float dt) {
    const float *vx = simp_vx(s), *vy = simp_vy(s);
    const uint8_t *fl = simp_flags_arr(s);
    int n = simp_count(s);
    float alpha = 1.0f - expf(-dt / HEADING_TAU);
    float beta  = 1.0f - expf(-dt / SPEED_TAU);
    /* max facing rotation this step; constant per frame, so the rotation that
     * enforces it is precomputed once (no per-agent trig on the hot path) */
    float maxturn = MAX_TURN_RATE * dt;
    float ct = cosf(maxturn), st = sinf(maxturn);
    for (int i = 0; i < n; i++) {
        int slot = simp_slot_of(s, i);
        SimPHandle h = simp_handle_of(s, i);
        if (sl->seen[slot] != h) {
            /* new agent in this slot: seed variant, heading, phase and tint
             * from the handle so respawns are clean and the horde varied */
            uint32_t r = hash32(h);
            float ang = (float)(r & 0xffff) * (6.2831853f / 65536.0f);
            sl->seen[slot] = h;
            sl->hx[slot] = sinf(ang) * 0.01f;   /* weak: first real velocity */
            sl->hy[slot] = cosf(ang) * 0.01f;   /* sample takes over fast    */
            sl->spd[slot] = 1.0f;               /* fresh spawn: not stuck    */
            sl->stuckf[slot] = 0;
            sl->phase[slot] = (float)((r >> 16) & 0xff) / 256.0f;
            sl->var[slot] = (uint8_t)((r >> 24) % (uint32_t)sl->n_var);
            /* variety from a curated zombie palette + brightness jitter:
             * free random RGB reads as purple/green zombies in the crowd,
             * a palette stays plausible while still telling agents apart */
            static const uint8_t PAL[][3] = {
                { 255, 255, 255 },     /* as authored          */
                { 215, 255, 215 },     /* sickly green         */
                { 255, 225, 210 },     /* raw / bloodshot      */
                { 225, 225, 240 },     /* ashen                */
                { 255, 245, 205 },     /* jaundiced            */
                { 235, 215, 215 },     /* bruised              */
                { 225, 240, 225 },     /* pale green           */
                { 240, 240, 215 },     /* dusty                */
            };
            const uint8_t *p = PAL[(r >> 8) & 7];
            int jit = (int)((r >> 11) & 0x1f) - 16;       /* +-16 brightness */
            int tr = p[0] + jit, tg = p[1] + jit, tb = p[2] + jit;
            sl->tr[slot] = (uint8_t)(tr > 255 ? 255 : (tr < 160 ? 160 : tr));
            sl->tg[slot] = (uint8_t)(tg > 255 ? 255 : (tg < 160 ? 160 : tg));
            sl->tb[slot] = (uint8_t)(tb > 255 ? 255 : (tb < 160 ? 160 : tb));
        }
        if (fl[i] & SIMP_FLYING) continue;      /* keep launch heading      */
        float sp = sqrtf(vx[i] * vx[i] + vy[i] * vy[i]);

        /* stuck detection FIRST: the facing freezes while stuck, so it must be
         * known before the heading update. Speed EMA + hysteresis (dormant
         * agents are "stuck" by flag, not by speed: they are still on purpose) */
        sl->spd[slot] += beta * (sp - sl->spd[slot]);
        if (!sl->stuckf[slot] && sl->spd[slot] < STUCK_ENTER_MS)
            sl->stuckf[slot] = 1;
        else if (sl->stuckf[slot] && sl->spd[slot] > STUCK_EXIT_MS)
            sl->stuckf[slot] = 0;

        /* heading = EMA of velocity toward the move dir, but: (1) frozen while
         * stuck or standing — in a jam the PBD micro-jitter would spin the
         * facing (pirouette); (2) rate-limited so a shove or the EMA vector
         * passing through zero on a reversal slews instead of snapping. */
        if (sp > 0.05f && !sl->stuckf[slot]) {
            float ox = sl->hx[slot], oy = sl->hy[slot];
            float nx = ox + alpha * (vx[i] - ox);
            float ny = oy + alpha * (vy[i] - oy);
            float cross = ox * ny - oy * nx;    /* sign = turn direction   */
            float dot   = ox * nx + oy * ny;
            float mo2 = ox * ox + oy * oy, mn2 = nx * nx + ny * ny;
            /* turn exceeds maxturn iff angle>90 (dot<0) or sin>sin(maxturn) */
            if ((dot < 0.0f || cross * cross > st * st * mo2 * mn2)
                && mo2 > 1e-12f) {
                float mag = sqrtf(mn2);
                float imo = 1.0f / sqrtf(mo2);
                float ux = ox * imo, uy = oy * imo;       /* old unit dir   */
                float s = cross >= 0.0f ? st : -st;       /* rotate toward n */
                sl->hx[slot] = (ux * ct - uy * s) * mag;
                sl->hy[slot] = (ux * s  + uy * ct) * mag;
            } else {
                sl->hx[slot] = nx;
                sl->hy[slot] = ny;
            }
        }

        if (sl->has_stuck && (sl->stuckf[slot] || (fl[i] & SIMP_DORMANT))) {
            /* time-based playback at the clip's native speed */
            float d = sl->stuck.duration_s > 0.2f ? sl->stuck.duration_s
                                                  : 2.0f;
            sl->phase[slot] += dt / d;
        } else {
            float stride = sl->v[sl->var[slot]].stride_m;
            if (stride > 0.1f)
                sl->phase[slot] += sp * dt / stride;
            else
                sl->phase[slot] += dt;          /* non-locomotion: 1 cycle/s */
        }

        /* occasional random gait change, walking agents only */
        if (sl->n_var > 1 && sp > 0.3f) {
            uint32_t x = sl->rng;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            sl->rng = x;
            if ((float)(x >> 8) * (1.0f / 16777216.0f) < dt / VARIANT_SWITCH_T)
                sl->var[slot] = (uint8_t)((sl->var[slot] + 1 +
                                 (x % (uint32_t)(sl->n_var - 1)))
                                % (uint32_t)sl->n_var);   /* always different */
        }
    }
}

static int order_cmp(const void *a, const void *b) {
    float ka = ((const SprOrd *)a)->key;
    float kb = ((const SprOrd *)b)->key;
    return (ka > kb) - (ka < kb);
}

void sprite_layer_draw(SpriteLayer *sl, SDL_Renderer *ren, const SimP *s,
                       float cam_x, float cam_y, float ppm) {
    const float *px = simp_px(s), *py = simp_py(s);
    const float *rad = simp_radius_arr(s);
    const float *z = simp_z_arr(s);
    const uint8_t *fl = simp_flags_arr(s);
    int n = simp_count(s);

    int vw, vh;
    SDL_GetRenderOutputSize(ren, &vw, &vh);

    /* y-sort on ground y: lower on screen draws on top (GFX_DESIGN.md §1).
     * Cull with a generous margin (frames are mostly transparent border). */
    int m = 0;
    float margin = 2.0f * (float)sl->v[0].frame_px / sl->v[0].px_per_m;
    for (int i = 0; i < n; i++) {
        if (px[i] < cam_x - margin || px[i] > cam_x + (float)vw / ppm + margin ||
            py[i] < cam_y - margin || py[i] > cam_y + (float)vh / ppm + margin)
            continue;
        sl->order[m].key = py[i];
        sl->order[m].idx = i;
        m++;
    }
    qsort(sl->order, (size_t)m, sizeof *sl->order, order_cmp);

    for (int k = 0; k < m; k++) {
        int i = sl->order[k].idx;
        int slot = simp_slot_of(s, i);
        const SprSheet *sh =
            sl->has_stuck && (sl->stuckf[slot] || (fl[i] & SIMP_DORMANT))
                ? &sl->stuck : &sl->v[sl->var[slot]];

        /* heading -> sheet row: row r faces r*(360/dirs)deg clockwise from
         * screen-down; world y points down on screen, so r = atan2(-hx, hy) */
        float sector = 6.2831853f / (float)sh->dirs;
        int row = (int)lroundf(atan2f(-sl->hx[slot], sl->hy[slot]) / sector);
        row = ((row % sh->dirs) + sh->dirs) % sh->dirs;
        int frame = (int)(sl->phase[slot] * (float)sh->frames) % sh->frames;

        float scale = rad[i] / SHEET_RADIUS_M;
        float side = (float)sh->frame_px / sh->px_per_m * ppm * scale;
        float sx = (px[i] - cam_x) * ppm;
        float sy = (py[i] - cam_y) * ppm;

        if ((fl[i] & SIMP_FLYING) && sl->shadow) {
            SDL_FRect shd = { sx - rad[i] * 2.0f * ppm,
                              sy - rad[i] * 1.0f * ppm,
                              rad[i] * 4.0f * ppm, rad[i] * 2.0f * ppm };
            SDL_RenderTexture(ren, sl->shadow, NULL, &shd);
        }

        SDL_FRect src = { (float)(frame * sh->frame_px),
                          (float)(row * sh->frame_px),
                          (float)sh->frame_px, (float)sh->frame_px };
        SDL_FRect dst = { sx - (sh->anchor_x / (float)sh->frame_px) * side,
                          sy - z[i] * sh->k_z * ppm
                             - (sh->anchor_y / (float)sh->frame_px) * side,
                          side, side };
        if (fl[i] & SIMP_DORMANT)
            SDL_SetTextureColorMod(sh->tex, (Uint8)(sl->tr[slot] / 2),
                                   (Uint8)(sl->tg[slot] / 2),
                                   (Uint8)(sl->tb[slot] / 2));
        else
            SDL_SetTextureColorMod(sh->tex, sl->tr[slot], sl->tg[slot],
                                   sl->tb[slot]);
        SDL_RenderTexture(ren, sh->tex, &src, &dst);
    }
    for (int i = 0; i < sl->n_var; i++)
        SDL_SetTextureColorMod(sl->v[i].tex, 255, 255, 255);
    if (sl->has_stuck)
        SDL_SetTextureColorMod(sl->stuck.tex, 255, 255, 255);
}

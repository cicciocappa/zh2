/* scene.c — vector scene loader/rasterizer. See scene.h for the file format. */

#define _POSIX_C_SOURCE 200809L   /* strtok_r under -std=c11 */
#include "scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>

#if defined(_WIN32)
#define strtok_r strtok_s        /* MS CRT: identical 3-arg signature */
#endif

/* ---- SimPParams fields addressable from "set <name> <value>" ------------ */

typedef struct { const char *name; size_t off; int is_int; } ParamEntry;

static const ParamEntry PTAB[] = {
    { "v_max",        offsetof(SimPParams, v_max),        0 },
    { "v_jitter",     offsetof(SimPParams, v_jitter),     0 },
    { "a_max",        offsetof(SimPParams, a_max),        0 },
    { "radius",       offsetof(SimPParams, radius),       0 },
    { "r_jitter",     offsetof(SimPParams, r_jitter),     0 },
    { "noise_ang",    offsetof(SimPParams, noise_ang),    0 },
    { "damping",      offsetof(SimPParams, damping),      0 },
    { "v_clamp",      offsetof(SimPParams, v_clamp),      0 },
    { "pbd_iters",    offsetof(SimPParams, pbd_iters),    1 },
    { "landing_damp", offsetof(SimPParams, landing_damp), 0 },
    { "wall_h",       offsetof(SimPParams, wall_h),       0 },
    { "k_density",    offsetof(SimPParams, k_density),    0 },
    { "k_jam",        offsetof(SimPParams, k_jam),        0 },
    { "k_danger",     offsetof(SimPParams, k_danger),     0 },
    { "danger_ref",   offsetof(SimPParams, danger_ref),   0 },
    { "danger_hl",    offsetof(SimPParams, danger_hl),    0 },
    { "k_corpse",     offsetof(SimPParams, k_corpse),     0 },
    { "flow_period",  offsetof(SimPParams, flow_period),  0 },
};
#define PTAB_N ((int)(sizeof PTAB / sizeof PTAB[0]))

static const ParamEntry *param_find(const char *name) {
    for (int i = 0; i < PTAB_N; i++)
        if (strcmp(PTAB[i].name, name) == 0) return &PTAB[i];
    return NULL;
}

/* ---- lifecycle ----------------------------------------------------------- */

void scene_free(Scene *sc) { memset(sc, 0, sizeof *sc); }

static void scene_derive_grid(Scene *sc) {
    sc->gw = (int)lroundf(sc->world_w / sc->cell);
    sc->gh = (int)lroundf(sc->world_h / sc->cell);
    if (sc->gw < 1) sc->gw = 1;
    if (sc->gh < 1) sc->gh = 1;
}

/* ---- load ---------------------------------------------------------------- */

#define LINE_MAX_LEN 4096

/* parse a run of "x y" pairs from the rest of a tokenized line into a poly */
static int parse_poly_verts(char **save, ScenePoly *p) {
    char *tx, *ty;
    p->nverts = 0;
    while ((tx = strtok_r(NULL, " \t", save)) != NULL) {
        ty = strtok_r(NULL, " \t", save);
        if (!ty) return -1;                       /* odd vertex count */
        if (p->nverts >= SCENE_POLY_MAX_VERTS) return -1;
        p->vx[p->nverts] = (float)atof(tx);
        p->vy[p->nverts] = (float)atof(ty);
        p->nverts++;
    }
    return p->nverts >= 3 ? 0 : -1;
}

int scene_load(const char *path, Scene *sc) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    memset(sc, 0, sizeof *sc);
    sc->cell = 0.5f;
    sc->world_w = sc->world_h = 0.0f;

    char line[LINE_MAX_LEN];
    int err = 0;

    while (!err && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *save = NULL;
        char *key = strtok_r(line, " \t", &save);
        if (!key || key[0] == '#') continue;

        if (!strcmp(key, "cell")) {
            char *v = strtok_r(NULL, " \t", &save);
            if (!v || (sc->cell = (float)atof(v)) <= 0.0f) err = -2;
        } else if (!strcmp(key, "world")) {
            char *w = strtok_r(NULL, " \t", &save), *h = strtok_r(NULL, " \t", &save);
            if (!w || !h) err = -2;
            else { sc->world_w = (float)atof(w); sc->world_h = (float)atof(h); }
        } else if (!strcmp(key, "terrain")) {
            char *v = strtok_r(NULL, " \t", &save);
            if (!v) err = -2;
            else { strncpy(sc->terrain, v, sizeof sc->terrain - 1);
                   sc->terrain[sizeof sc->terrain - 1] = '\0'; }
        } else if (!strcmp(key, "statics")) {
            char *v = strtok_r(NULL, " \t", &save);
            if (!v) err = -2;
            else { strncpy(sc->statics, v, sizeof sc->statics - 1);
                   sc->statics[sizeof sc->statics - 1] = '\0'; }
        } else if (!strcmp(key, "set")) {
            char *n = strtok_r(NULL, " \t", &save), *v = strtok_r(NULL, " \t", &save);
            if (n && v && param_find(n) && sc->n_set < SCENE_MAX_SET) {
                strncpy(sc->set[sc->n_set].name, n, 23);
                sc->set[sc->n_set].value = (float)atof(v);
                sc->n_set++;
            } else err = -2;
        } else if (!strcmp(key, "goal") || !strcmp(key, "spawn") ||
                   !strcmp(key, "pack") || !strcmp(key, "cost")) {
            int is_cost = !strcmp(key, "cost");
            char *t[5]; int nt = 0;
            for (; nt < (is_cost ? 5 : 4); nt++) {
                t[nt] = strtok_r(NULL, " \t", &save);
                if (!t[nt]) { err = -2; break; }
            }
            if (err) break;
            SceneRect r = { (float)atof(t[0]), (float)atof(t[1]),
                            (float)atof(t[2]), (float)atof(t[3]),
                            is_cost ? (float)atof(t[4]) : 0.0f };
            SceneRect *dst; int *cnt;
            if (!strcmp(key, "goal"))       { dst = sc->goal;  cnt = &sc->n_goal; }
            else if (!strcmp(key, "spawn")) { dst = sc->spawn; cnt = &sc->n_spawn; }
            else if (!strcmp(key, "pack"))  { dst = sc->pack;  cnt = &sc->n_pack; }
            else                            { dst = sc->cost;  cnt = &sc->n_cost; }
            if (*cnt < SCENE_MAX_RECT) dst[(*cnt)++] = r; else err = -2;
        } else if (!strcmp(key, "poly")) {
            if (sc->n_poly >= SCENE_MAX_POLY) { err = -2; break; }
            ScenePoly *p = &sc->poly[sc->n_poly];
            memset(p, 0, sizeof *p);
            char *hs = strtok_r(NULL, " \t", &save);
            char *kind = strtok_r(NULL, " \t", &save);
            if (!hs || !kind) { err = -2; break; }
            p->height = (float)atof(hs);
            if (!strcmp(kind, "solid")) { p->solid = true; p->cost = 0.0f; }
            else if (!strcmp(kind, "cost")) {
                char *w = strtok_r(NULL, " \t", &save);
                if (!w) { err = -2; break; }
                p->solid = false; p->cost = (float)atof(w);
            } else { err = -2; break; }
            if (parse_poly_verts(&save, p) != 0) { err = -2; break; }
            sc->n_poly++;
        } else if (!strcmp(key, "wall")) {
            if (sc->n_wall >= SCENE_MAX_RECT) { err = -2; break; }
            char *t[6]; int ok = 1;
            for (int i = 0; i < 6; i++) { t[i] = strtok_r(NULL, " \t", &save); if (!t[i]) ok = 0; }
            if (!ok) { err = -2; break; }
            SceneWall w = { (float)atof(t[2]), (float)atof(t[3]), (float)atof(t[4]),
                            (float)atof(t[5]), (float)atof(t[0]), (float)atof(t[1]) };
            sc->wall[sc->n_wall++] = w;
        } else if (!strcmp(key, "turret")) {
            if (sc->n_turret >= SCENE_MAX_RECT) { err = -2; break; }
            char *x = strtok_r(NULL, " \t", &save), *y = strtok_r(NULL, " \t", &save);
            char *rg = strtok_r(NULL, " \t", &save), *hv = strtok_r(NULL, " \t", &save);
            char *hp = strtok_r(NULL, " \t", &save);
            if (!x || !y) { err = -2; break; }
            SceneTurret t = { (float)atof(x), (float)atof(y),
                              rg ? (float)atof(rg) : 30.0f, hv ? atoi(hv) : 0,
                              hp ? (float)atof(hp) : 0.0f };
            sc->turret[sc->n_turret++] = t;
        } else if (!strcmp(key, "prop")) {
            if (sc->n_prop >= SCENE_MAX_PROP) { err = -2; break; }
            char *k = strtok_r(NULL, " \t", &save);
            char *x = strtok_r(NULL, " \t", &save);
            char *y = strtok_r(NULL, " \t", &save);
            char *r = strtok_r(NULL, " \t", &save);   /* rotation optional, default 0 */
            if (!k || !x || !y) { err = -2; break; }
            SceneProp *pr = &sc->prop[sc->n_prop];
            strncpy(pr->key, k, SCENE_PROP_KEY_LEN - 1);
            pr->key[SCENE_PROP_KEY_LEN - 1] = '\0';
            pr->x = (float)atof(x); pr->y = (float)atof(y);
            pr->rot = r ? (float)atof(r) : 0.0f;
            sc->n_prop++;
        } else {
            err = -2;                              /* unknown directive */
        }
    }
    fclose(f);

    if (!err && (sc->world_w <= 0.0f || sc->world_h <= 0.0f)) err = -2;
    if (err) { scene_free(sc); return err; }
    scene_derive_grid(sc);
    return 0;
}

/* ---- save ---------------------------------------------------------------- */

int scene_save(const char *path, const Scene *sc) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# horde scene (vector, meters)\n");
    fprintf(f, "cell %g\n", (double)sc->cell);
    fprintf(f, "world %g %g\n", (double)sc->world_w, (double)sc->world_h);
    if (sc->terrain[0]) fprintf(f, "terrain %s\n", sc->terrain);
    if (sc->statics[0]) fprintf(f, "statics %s\n", sc->statics);
    for (int k = 0; k < sc->n_set; k++)
        fprintf(f, "set %s %g\n", sc->set[k].name, (double)sc->set[k].value);
    for (int k = 0; k < sc->n_goal; k++)
        fprintf(f, "goal %g %g %g %g\n", (double)sc->goal[k].x, (double)sc->goal[k].y,
                (double)sc->goal[k].w, (double)sc->goal[k].h);
    for (int k = 0; k < sc->n_spawn; k++)
        fprintf(f, "spawn %g %g %g %g\n", (double)sc->spawn[k].x, (double)sc->spawn[k].y,
                (double)sc->spawn[k].w, (double)sc->spawn[k].h);
    for (int k = 0; k < sc->n_pack; k++)
        fprintf(f, "pack %g %g %g %g\n", (double)sc->pack[k].x, (double)sc->pack[k].y,
                (double)sc->pack[k].w, (double)sc->pack[k].h);
    for (int k = 0; k < sc->n_cost; k++)
        fprintf(f, "cost %g %g %g %g %g\n", (double)sc->cost[k].x, (double)sc->cost[k].y,
                (double)sc->cost[k].w, (double)sc->cost[k].h, (double)sc->cost[k].weight);
    for (int k = 0; k < sc->n_poly; k++) {
        const ScenePoly *p = &sc->poly[k];
        if (p->solid) fprintf(f, "poly %g solid", (double)p->height);
        else          fprintf(f, "poly %g cost %g", (double)p->height, (double)p->cost);
        for (int v = 0; v < p->nverts; v++)
            fprintf(f, " %g %g", (double)p->vx[v], (double)p->vy[v]);
        fputc('\n', f);
    }
    for (int k = 0; k < sc->n_wall; k++)
        fprintf(f, "wall %g %g %g %g %g %g\n", (double)sc->wall[k].hp, (double)sc->wall[k].cost_mult,
                (double)sc->wall[k].x, (double)sc->wall[k].y,
                (double)sc->wall[k].w, (double)sc->wall[k].h);
    for (int k = 0; k < sc->n_turret; k++)
        fprintf(f, "turret %g %g %g %d %g\n", (double)sc->turret[k].x, (double)sc->turret[k].y,
                (double)sc->turret[k].range, sc->turret[k].heavy, (double)sc->turret[k].hp);
    for (int k = 0; k < sc->n_prop; k++)
        fprintf(f, "prop %s %g %g %g\n", sc->prop[k].key, (double)sc->prop[k].x,
                (double)sc->prop[k].y, (double)sc->prop[k].rot);
    int bad = ferror(f);
    fclose(f);
    return bad ? -1 : 0;
}

/* ---- rasterization ------------------------------------------------------- */

/* Scanline fill of a simple polygon (convex or concave) into the grid. For
 * each cell whose CENTER falls inside the polygon, calls cb(ud, cx, cy).
 * Even-odd rule on horizontal lines through cell-row centers. Public: also
 * the kernel for host-side prop footprints (prop_world.c, §8.5). */
void scene_raster_cells(const float *pvx, const float *pvy, int nverts,
                        float cell, int gw, int gh,
                        void (*cb)(void *ud, int cx, int cy), void *ud) {
    float xs[SCENE_POLY_MAX_VERTS];
    for (int cy = 0; cy < gh; cy++) {
        float yc = ((float)cy + 0.5f) * cell;
        int n = 0;
        for (int i = 0, j = nverts - 1; i < nverts; j = i++) {
            float yi = pvy[i], yj = pvy[j];
            if ((yi <= yc && yc < yj) || (yj <= yc && yc < yi)) {
                float t = (yc - yi) / (yj - yi);
                xs[n++] = pvx[i] + t * (pvx[j] - pvx[i]);
            }
        }
        /* insertion sort the crossings */
        for (int a = 1; a < n; a++) {
            float key = xs[a]; int b = a - 1;
            while (b >= 0 && xs[b] > key) { xs[b + 1] = xs[b]; b--; }
            xs[b + 1] = key;
        }
        for (int a = 0; a + 1 < n; a += 2) {
            int cx0 = (int)ceilf(xs[a]     / cell - 0.5f);
            int cx1 = (int)floorf(xs[a + 1] / cell - 0.5f);
            if (cx0 < 0) cx0 = 0;
            if (cx1 >= gw) cx1 = gw - 1;
            for (int cx = cx0; cx <= cx1; cx++) cb(ud, cx, cy);
        }
    }
}

typedef struct { SimP *s; const ScenePoly *p; } PolyRasterCtx;
static void cb_poly(void *ud, int cx, int cy) {
    PolyRasterCtx *c = (PolyRasterCtx *)ud;
    if (c->p->solid) simp_set_wall(c->s, cx, cy, true);
    else             simp_add_cost(c->s, cx, cy, c->p->cost);
}

/* rect in meters -> inclusive cell range; cb per cell */
static void raster_rect(SimP *s, const SceneRect *r, float cell, int gw, int gh,
                        void (*cb)(SimP *, int, int, const SceneRect *)) {
    int cx0 = (int)floorf(r->x / cell), cy0 = (int)floorf(r->y / cell);
    int cx1 = (int)floorf((r->x + r->w) / cell - 1e-4f);
    int cy1 = (int)floorf((r->y + r->h) / cell - 1e-4f);
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= gw) cx1 = gw - 1;
    if (cy1 >= gh) cy1 = gh - 1;
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++) cb(s, cx, cy, r);
}
static void cb_goal(SimP *s, int cx, int cy, const SceneRect *r) {
    (void)r; if (!simp_is_wall(s, cx, cy)) simp_set_goal(s, cx, cy, true);
}
static void cb_cost(SimP *s, int cx, int cy, const SceneRect *r) {
    simp_add_cost(s, cx, cy, r->weight);
}

/* ---- instantiate --------------------------------------------------------- */

static inline uint32_t srng_next(uint32_t *st) {
    uint32_t x = *st; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *st = x;
}
static inline float srng_f01(uint32_t *st) {
    return (float)(srng_next(st) >> 8) * (1.0f / 16777216.0f);
}

SimP *scene_instantiate(const Scene *sc, int max_agents) {
    SimP *s = simp_create(sc->gw, sc->gh, sc->cell, max_agents);
    if (!s) return NULL;

    SimPParams *P = simp_params(s);
    for (int k = 0; k < sc->n_set; k++) {
        const ParamEntry *e = param_find(sc->set[k].name);
        if (!e) continue;
        if (e->is_int) *(int *)((char *)P + e->off)   = (int)sc->set[k].value;
        else           *(float *)((char *)P + e->off) = sc->set[k].value;
    }

    for (int k = 0; k < sc->n_poly; k++) {
        PolyRasterCtx ctx = { s, &sc->poly[k] };
        scene_raster_cells(sc->poly[k].vx, sc->poly[k].vy, sc->poly[k].nverts,
                           sc->cell, sc->gw, sc->gh, cb_poly, &ctx);
    }
    for (int k = 0; k < sc->n_cost; k++)
        raster_rect(s, &sc->cost[k], sc->cell, sc->gw, sc->gh, cb_cost);
    for (int k = 0; k < sc->n_goal; k++)
        raster_rect(s, &sc->goal[k], sc->cell, sc->gw, sc->gh, cb_goal);
    simp_terrain_commit(s);

    /* dormant packs: jittered attempts admitted by simp_free_at (no overlap) */
    uint32_t rng = 0x5CE9E5EEu;
    float r_adm = P->radius * (1.0f + P->r_jitter);
    for (int k = 0; k < sc->n_pack; k++) {
        const SceneRect *r = &sc->pack[k];
        int tries = (int)((r->w * r->h) / (r_adm * r_adm) * 2.0f) + 1;
        for (int t = 0; t < tries; t++) {
            float x = r->x + srng_f01(&rng) * r->w;
            float y = r->y + srng_f01(&rng) * r->h;
            if (simp_free_at(s, x, y, r_adm)) simp_spawn_dormant(s, x, y);
        }
    }
    return s;
}

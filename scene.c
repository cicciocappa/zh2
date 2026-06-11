/* scene.c — implementation. See scene.h for the file format. */

#include "scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

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
    { "flow_period",  offsetof(SimPParams, flow_period),  0 },
};
#define PTAB_N ((int)(sizeof PTAB / sizeof PTAB[0]))

static const ParamEntry *param_find(const char *name) {
    for (int i = 0; i < PTAB_N; i++)
        if (strcmp(PTAB[i].name, name) == 0) return &PTAB[i];
    return NULL;
}

/* ---- lifecycle ----------------------------------------------------------- */

int scene_alloc(Scene *sc, int gw, int gh, float cell) {
    memset(sc, 0, sizeof *sc);
    if (gw <= 0 || gh <= 0 || cell <= 0.0f) return -1;
    sc->gw = gw; sc->gh = gh; sc->cell = cell;
    size_t n = (size_t)gw * gh;
    sc->wall  = (uint8_t *)calloc(n, 1);
    sc->goal  = (uint8_t *)calloc(n, 1);
    sc->spawn = (uint8_t *)calloc(n, 1);
    sc->pack  = (uint8_t *)calloc(n, 1);
    if (!sc->wall || !sc->goal || !sc->spawn || !sc->pack) {
        scene_free(sc);
        return -1;
    }
    return 0;
}

void scene_free(Scene *sc) {
    free(sc->wall); free(sc->goal); free(sc->spawn); free(sc->pack);
    memset(sc, 0, sizeof *sc);
}

/* ---- load ----------------------------------------------------------------- */

#define LINE_MAX_LEN 1024

int scene_load(const char *path, Scene *sc) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    memset(sc, 0, sizeof *sc);
    sc->cell = 0.5f;

    /* two passes over the map block would need rewinding; instead buffer the
     * row lines first, then size the grid from them */
    char  line[LINE_MAX_LEN];
    char **rows = NULL;
    int   n_rows = 0, cap_rows = 0, in_map = 0, err = 0;

    while (fgets(line, sizeof line, f)) {
        size_t len = strcspn(line, "\r\n");
        line[len] = '\0';
        if (!in_map) {
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '\0' || *p == '#') continue;
            char key[32];
            if (sscanf(p, "%31s", key) != 1) continue;
            if (strcmp(key, "map") == 0) { in_map = 1; continue; }
            if (strcmp(key, "cell") == 0) {
                if (sscanf(p, "%*s %f", &sc->cell) != 1 || sc->cell <= 0.0f) err = -2;
            } else if (strcmp(key, "set") == 0) {
                char name[24]; float v;
                if (sscanf(p, "%*s %23s %f", name, &v) == 2 && param_find(name) &&
                    sc->n_set < SCENE_MAX_SET) {
                    strcpy(sc->set[sc->n_set].name, name);
                    sc->set[sc->n_set].value = v;
                    sc->n_set++;
                } else err = -2;
            } else if (strcmp(key, "cost") == 0) {
                if (sc->n_cost < SCENE_MAX_COST &&
                    sscanf(p, "%*s %d %d %d %d %f",
                           &sc->cost[sc->n_cost].x0, &sc->cost[sc->n_cost].y0,
                           &sc->cost[sc->n_cost].x1, &sc->cost[sc->n_cost].y1,
                           &sc->cost[sc->n_cost].w) == 5)
                    sc->n_cost++;
                else err = -2;
            } else err = -2;                     /* unknown directive */
            if (err) break;
        } else {
            if (n_rows == cap_rows) {
                cap_rows = cap_rows ? cap_rows * 2 : 64;
                char **nr = (char **)realloc(rows, (size_t)cap_rows * sizeof(char *));
                if (!nr) { err = -1; break; }
                rows = nr;
            }
            rows[n_rows] = (char *)malloc(len + 1);
            if (!rows[n_rows]) { err = -1; break; }
            memcpy(rows[n_rows], line, len + 1);
            n_rows++;
        }
    }
    fclose(f);

    int gw = 0;
    for (int r = 0; r < n_rows; r++) {
        int w = (int)strlen(rows[r]);
        if (w > gw) gw = w;
    }
    if (!err && (n_rows == 0 || gw == 0)) err = -2;
    if (!err) {
        Scene hdr = *sc;     /* scene_alloc zeroes the struct: keep the header */
        if (scene_alloc(sc, gw, n_rows, hdr.cell) != 0) err = -1;
        else {
            memcpy(sc->set, hdr.set, sizeof sc->set);
            memcpy(sc->cost, hdr.cost, sizeof sc->cost);
            sc->n_set = hdr.n_set;
            sc->n_cost = hdr.n_cost;
        }
    }

    if (!err) {
        for (int cy = 0; cy < sc->gh; cy++) {
            const char *row = rows[cy];
            int w = (int)strlen(row);
            for (int cx = 0; cx < w; cx++) {
                int i = cy * sc->gw + cx;
                switch (row[cx]) {
                case '#': sc->wall[i]  = 1; break;
                case 'G': case 'g': sc->goal[i]  = 1; break;
                case 'S': case 's': sc->spawn[i] = 1; break;
                case 'P': case 'p': sc->pack[i]  = 1; break;
                case '.': case ' ': break;
                default: break;                  /* unknown chars = floor */
                }
            }
        }
    }

    for (int r = 0; r < n_rows; r++) free(rows[r]);
    free(rows);
    if (err) { scene_free(sc); return err; }
    return 0;
}

/* ---- save ----------------------------------------------------------------- */

int scene_save(const char *path, const Scene *sc) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# horde scene %dx%d\n", sc->gw, sc->gh);
    fprintf(f, "cell %g\n", (double)sc->cell);
    for (int k = 0; k < sc->n_set; k++)
        fprintf(f, "set %s %g\n", sc->set[k].name, (double)sc->set[k].value);
    for (int k = 0; k < sc->n_cost; k++)
        fprintf(f, "cost %d %d %d %d %g\n",
                sc->cost[k].x0, sc->cost[k].y0, sc->cost[k].x1, sc->cost[k].y1,
                (double)sc->cost[k].w);
    fprintf(f, "map\n");
    for (int cy = 0; cy < sc->gh; cy++) {
        for (int cx = 0; cx < sc->gw; cx++) {
            int i = cy * sc->gw + cx;
            char c = '.';
            if (sc->wall[i])       c = '#';
            else if (sc->goal[i])  c = 'G';
            else if (sc->spawn[i]) c = 'S';
            else if (sc->pack[i])  c = 'P';
            fputc(c, f);
        }
        fputc('\n', f);
    }
    int bad = ferror(f);
    fclose(f);
    return bad ? -1 : 0;
}

/* ---- instantiate ----------------------------------------------------------- */

/* deterministic jitter for pack placement: same scene -> same agents */
static inline uint32_t srng_next(uint32_t *st) {
    uint32_t x = *st;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *st = x;
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

    for (int cy = 0; cy < sc->gh; cy++)
        for (int cx = 0; cx < sc->gw; cx++) {
            int i = cy * sc->gw + cx;
            if (sc->wall[i]) simp_set_wall(s, cx, cy, true);
            if (sc->goal[i] && !sc->wall[i]) simp_set_goal(s, cx, cy, true);
        }
    for (int k = 0; k < sc->n_cost; k++)
        for (int cy = sc->cost[k].y0; cy <= sc->cost[k].y1; cy++)
            for (int cx = sc->cost[k].x0; cx <= sc->cost[k].x1; cx++)
                simp_add_cost(s, cx, cy, sc->cost[k].w);
    simp_terrain_commit(s);

    /* dormant packs: a few jittered attempts per cell, admitted by
     * simp_free_at like the sandbox PACK brush (no initial overlap) */
    uint32_t rng = 0x5CE9E5EEu;
    float r_adm = P->radius * (1.0f + P->r_jitter);
    for (int cy = 0; cy < sc->gh; cy++)
        for (int cx = 0; cx < sc->gw; cx++) {
            if (!sc->pack[cy * sc->gw + cx]) continue;
            for (int t = 0; t < 3; t++) {
                float x = ((float)cx + srng_f01(&rng)) * sc->cell;
                float y = ((float)cy + srng_f01(&rng)) * sc->cell;
                if (simp_free_at(s, x, y, r_adm)) simp_spawn_dormant(s, x, y);
            }
        }
    return s;
}

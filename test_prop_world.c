/* test_prop_world.c — host-side application of catalog entity axes
 * (ENTITY_DESIGN.md §6 + §8.5, prop_world.c).
 *
 *   1. footprint raster: axis-aligned fence (2x0.5), rotated (90 deg) fence
 *      and a 31-deg bus — wall cells match a brute-force point-in-rotated-
 *      rect test on every cell center; decor prop raises nothing.
 *   2. tiers: siegeable fence cells cost cost_mult*base; indestructible bus
 *      cells cost palazzo-tier (>= PROP_WORLD_PALAZZO_MULT*base); the fence
 *      is registered as a defense structure with its hp, the bus is not.
 *   3. axis C: transmit through the fence is partial (opac 0.3), through the
 *      bus zero-ish (opac 1.0 default), in the open 1.
 *   4. WxD omitted -> exactly the one cell under the prop center.
 *   5. determinism: two fresh worlds -> identical wall grids.
 */
#include "prop_world.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TMP "test_prop_world_tmp.txt"

static Scene *make_scene(void) {
    static Scene sc;
    memset(&sc, 0, sizeof sc);
    sc.cell = 0.5f; sc.world_w = 40.0f; sc.world_h = 30.0f;
    sc.gw = 80; sc.gh = 60;
    sc.goal[0] = (SceneRect){ 38.0f, 13.0f, 1.5f, 4.0f, 0 }; sc.n_goal = 1;
    /* props: two fences (0 / 90 deg), a rotated bus, a hut (no WxD), decor */
    strcpy(sc.prop[0].key, "fence"); sc.prop[0].x = 10.13f; sc.prop[0].y = 10.21f; sc.prop[0].rot = 0.0f;
    strcpy(sc.prop[1].key, "fence"); sc.prop[1].x = 20.13f; sc.prop[1].y = 10.21f; sc.prop[1].rot = 90.0f;
    strcpy(sc.prop[2].key, "bus");   sc.prop[2].x = 15.31f; sc.prop[2].y = 20.17f; sc.prop[2].rot = 31.0f;
    strcpy(sc.prop[3].key, "hut");   sc.prop[3].x = 30.26f; sc.prop[3].y = 24.26f; sc.prop[3].rot = 0.0f;
    strcpy(sc.prop[4].key, "bench"); sc.prop[4].x = 5.13f;  sc.prop[4].y = 5.21f;  sc.prop[4].rot = 45.0f;
    sc.n_prop = 5;
    return &sc;
}

/* brute-force: is cell center inside the W×D rect of prop i? */
static int in_rect(const Scene *sc, int i, float W, float D, float px, float py) {
    float a = sc->prop[i].rot * 0.01745329252f, ca = cosf(a), sa = sinf(a);
    float dx = px - sc->prop[i].x, dy = py - sc->prop[i].y;
    float u =  dx * ca + dy * sa;      /* into prop-local frame (inverse rot) */
    float v = -dx * sa + dy * ca;
    return fabsf(u) <= 0.5f * W && fabsf(v) <= 0.5f * D;
}

int main(void) {
    FILE *f = fopen(TMP, "w");
    if (!f) { printf("FAIL (tmp)\n"); return 1; }
    fputs("fence  -  1.0  Fence  -  -  -  solid 1.8 2x0.5  350 0.6  0.3  -\n"
          "bus    -  1.0  Bus    -  -  -  solid 2.6 11x2.5 inf -    -    -\n"
          "hut    -  1.0  Hut    -  -  -  solid 4          inf -    1.0  -\n"
          "bench  -  1.0  Bench\n", f);
    fclose(f);
    PropCatalog cat;
    if (prop_catalog_load(TMP, &cat) != 4) { printf("FAIL (catalog)\n"); return 1; }
    remove(TMP);

    Scene *sc = make_scene();
    SimP *s = scene_instantiate(sc, 64);
    if (!s) { printf("FAIL (instantiate)\n"); return 1; }
    DefGame *g = def_create(s, 64);
    PropWorld pw;
    int nsolid = prop_world_apply(sc, &cat, s, g, &pw);
    int ok = 1;

    /* 1) footprint raster vs brute force (fences + bus + hut; bench = none) */
    float cell = sc->cell;
    int mism = 0, cells_fence0 = 0, cells_bus = 0, cells_hut = 0;
    for (int cy = 0; cy < sc->gh; cy++)
        for (int cx = 0; cx < sc->gw; cx++) {
            float px = (cx + 0.5f) * cell, py = (cy + 0.5f) * cell;
            int want = in_rect(sc, 0, 2.0f, 0.5f, px, py)
                     | in_rect(sc, 1, 2.0f, 0.5f, px, py)
                     | in_rect(sc, 2, 11.0f, 2.5f, px, py)
                     | in_rect(sc, 3, cell, cell, px, py);   /* hut: one cell */
            int got = simp_is_wall(s, cx, cy);
            if (want != got) mism++;
            if (got && in_rect(sc, 0, 2.0f, 0.5f, px, py)) cells_fence0++;
            if (got && in_rect(sc, 2, 11.0f, 2.5f, px, py)) cells_bus++;
            if (got && in_rect(sc, 3, cell, cell, px, py)) cells_hut++;
        }
    ok = ok && nsolid == 4 && mism == 0 && cells_fence0 > 0 && cells_bus > 0;
    printf("raster: solid=%d mismatch=%d fence0=%d bus=%d hut=%d | %s\n",
           nsolid, mism, cells_fence0, cells_bus, cells_hut, ok ? "ok" : "BAD");

    /* 4) hut without WxD = exactly the cell under its center */
    ok = ok && cells_hut == 1;

    /* 2) tiers + structure registration */
    float base = simp_wall_base_cost();
    const float *wc = simp_wall_cost_arr(s);
    int fcx = (int)(sc->prop[0].x / cell), fcy = (int)(sc->prop[0].y / cell);
    int bcx = (int)(sc->prop[2].x / cell), bcy = (int)(sc->prop[2].y / cell);
    float fcost = wc[fcy * sc->gw + fcx], bcost = wc[bcy * sc->gw + bcx];
    int fsid = pw.struct_id[0];
    int ok2 = fsid >= 0 && pw.struct_is_prop[fsid] &&
              fabsf(def_struct_hp(g, fsid) - 350.0f) < 1e-3f &&
              pw.struct_id[2] < 0 && pw.struct_id[4] < 0 && !pw.solid[4] &&
              fabsf(fcost - 0.6f * base) < 1e-3f * base &&
              bcost >= PROP_WORLD_PALAZZO_MULT * base * 0.999f &&
              pw.n_siege == 2;   /* both fences */
    printf("tiers: fence sid=%d hp=%.0f cost=%.2fxbase, bus cost=%.1fxbase | %s\n",
           fsid, (double)def_struct_hp(g, fsid), (double)(fcost / base),
           (double)(bcost / base), ok2 ? "ok" : "BAD");
    ok = ok && ok2;

    /* 3) axis C: ray across each prop's center, fired along +y through the
     * thin side; open path = 1, fence partial, bus ~0 (opac defaults to 1) */
    float t_open  = simp_ray_transmit(s, 2.0f, 2.0f, 0.0f, 1.0f, 10.0f);
    float t_fence = simp_ray_transmit(s, sc->prop[0].x, sc->prop[0].y - 3.0f,
                                      0.0f, 1.0f, 6.0f);
    float t_bus   = simp_ray_transmit(s, sc->prop[2].x, sc->prop[2].y - 4.0f,
                                      0.0f, 1.0f, 8.0f);
    int ok3 = fabsf(t_open - 1.0f) < 1e-6f &&
              t_fence > 0.05f && t_fence < 0.95f && t_bus < 1e-3f;
    printf("axis C: open=%.3f fence=%.3f bus=%.6f | %s\n",
           (double)t_open, (double)t_fence, (double)t_bus, ok3 ? "ok" : "BAD");
    ok = ok && ok3;

    /* 5) determinism: fresh world -> identical wall grid + counters */
    SimP *s2 = scene_instantiate(sc, 64);
    DefGame *g2 = def_create(s2, 64);
    PropWorld pw2;
    prop_world_apply(sc, &cat, s2, g2, &pw2);
    int det = pw2.n_solid == pw.n_solid && pw2.n_siege == pw.n_siege;
    for (int cy = 0; cy < sc->gh && det; cy++)
        for (int cx = 0; cx < sc->gw; cx++)
            if (simp_is_wall(s, cx, cy) != simp_is_wall(s2, cx, cy)) { det = 0; break; }
    printf("determinism: %s\n", det ? "ok" : "BAD");
    ok = ok && det;

    def_destroy(g2); simp_destroy(s2);
    def_destroy(g);  simp_destroy(s);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

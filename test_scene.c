/* test_scene.c — vector scene loader/rasterizer verification.
 *
 *   1. ROUNDTRIP: build a scene in code, save, load, compare every field.
 *   2. PARSE: a hand-written file (cell/world/set/rects/poly + comments)
 *      instantiates with the right params, rasterized walls (from a polygon),
 *      goals, cost, dormant packs, and reports spawner rects back.
 *   3. DETERMINISM: two instantiates of the same scene produce identical
 *      agents (count and positions).
 */
#include "scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TMP "test_scene_tmp.txt"

static int roundtrip(void) {
    Scene a;
    memset(&a, 0, sizeof a);
    a.cell = 0.5f; a.world_w = 30.0f; a.world_h = 20.0f;
    strcpy(a.terrain, "assets/terrain/level1.glb");
    a.n_set = 2;
    strcpy(a.set[0].name, "k_density"); a.set[0].value = 3.0f;
    strcpy(a.set[1].name, "pbd_iters"); a.set[1].value = 5.0f;
    a.n_goal = 1;  a.goal[0]  = (SceneRect){ 24, 8, 3, 2, 0 };
    a.n_spawn = 1; a.spawn[0] = (SceneRect){ 1, 1, 28, 2, 0 };
    a.n_pack = 1;  a.pack[0]  = (SceneRect){ 10, 9, 2, 2, 0 };
    a.n_cost = 1;  a.cost[0]  = (SceneRect){ 5, 5, 4, 2, 2.5f };
    a.n_poly = 2;
    a.poly[0] = (ScenePoly){ .nverts = 4, .height = 4.0f, .solid = true,
        .vx = { 12, 18, 18, 12 }, .vy = { 6, 6, 12, 12 } };
    a.poly[1] = (ScenePoly){ .nverts = 3, .height = 0.4f, .solid = false, .cost = 6.0f,
        .vx = { 20, 26, 23 }, .vy = { 4, 4, 9 } };
    a.n_prop = 2;
    strcpy(a.prop[0].key, "bench"); a.prop[0].x = 7.5f; a.prop[0].y = 12.0f; a.prop[0].rot = 90.0f;
    strcpy(a.prop[1].key, "sign");  a.prop[1].x = 3.0f; a.prop[1].y = 4.5f;  a.prop[1].rot = 0.0f;
    a.n_turret = 2;
    a.turret[0] = (SceneTurret){ 12, 8, 25.0f, 0, 400.0f };  /* tough emplacement */
    a.turret[1] = (SceneTurret){ 20, 6, 30.0f, 1, 0.0f };    /* hp omitted -> default */

    int ok = scene_save(TMP, &a) == 0;
    Scene b;
    ok = ok && scene_load(TMP, &b) == 0;
    ok = ok && b.gw == 60 && b.gh == 40 && fabsf(b.cell - 0.5f) < 1e-6f;
    ok = ok && b.n_set == 2 && b.n_goal == 1 && b.n_spawn == 1 &&
         b.n_pack == 1 && b.n_cost == 1 && b.n_poly == 2;
    ok = ok && strcmp(b.set[0].name, "k_density") == 0 &&
         fabsf(b.set[0].value - 3.0f) < 1e-6f;
    ok = ok && fabsf(b.cost[0].weight - 2.5f) < 1e-6f && b.cost[0].x == 5;
    ok = ok && b.poly[0].solid && b.poly[0].nverts == 4 &&
         fabsf(b.poly[0].height - 4.0f) < 1e-6f &&
         fabsf(b.poly[0].vx[1] - 18.0f) < 1e-6f;
    ok = ok && !b.poly[1].solid && fabsf(b.poly[1].cost - 6.0f) < 1e-6f &&
         b.poly[1].nverts == 3;
    ok = ok && strcmp(b.terrain, "assets/terrain/level1.glb") == 0;
    ok = ok && b.n_turret == 2 &&
         fabsf(b.turret[0].range - 25.0f) < 1e-6f && b.turret[0].heavy == 0 &&
         fabsf(b.turret[0].hp - 400.0f) < 1e-6f &&
         b.turret[1].heavy == 1 && fabsf(b.turret[1].hp - 0.0f) < 1e-6f;
    ok = ok && b.n_prop == 2 &&
         strcmp(b.prop[0].key, "bench") == 0 && fabsf(b.prop[0].x - 7.5f) < 1e-6f &&
         fabsf(b.prop[0].rot - 90.0f) < 1e-6f &&
         strcmp(b.prop[1].key, "sign") == 0 && fabsf(b.prop[1].y - 4.5f) < 1e-6f;
    printf("roundtrip: %dx%d set=%d poly=%d goal=%d prop=%d turret=%d | %s\n",
           b.gw, b.gh, b.n_set, b.n_poly, b.n_goal, b.n_prop, b.n_turret, ok ? "ok" : "BAD");
    scene_free(&a); scene_free(&b);
    return ok;
}

static int parse_and_instantiate(void) {
    FILE *f = fopen(TMP, "w");
    if (!f) return 0;
    fputs("# hand-written vector test scene\n"
          "cell 1.0\n"
          "world 10 6\n"
          "set v_max 2.0\n"
          "goal 8 1 1 2\n"
          "spawn 1 2 1 1\n"
          "cost 2 2 2 2 5.0\n"
          "pack 4 1 2 2\n"
          "poly 4.0 solid  3 0 5 0 5 1 3 1\n"     /* wall band: cells (3,0),(4,0) */
          "prop cart 6 3 45\n",                   /* pure decor: ignored by sim */
          f);
    fclose(f);

    Scene sc;
    int ok = scene_load(TMP, &sc) == 0;
    ok = ok && sc.gw == 10 && sc.gh == 6 && fabsf(sc.cell - 1.0f) < 1e-6f;
    ok = ok && sc.n_spawn == 1 && fabsf(sc.spawn[0].x - 1.0f) < 1e-6f;
    ok = ok && sc.n_prop == 1 && strcmp(sc.prop[0].key, "cart") == 0;
    SimP *s = ok ? scene_instantiate(&sc, 256) : NULL;
    ok = ok && s != NULL;
    if (ok) {
        ok = ok && fabsf(simp_params(s)->v_max - 2.0f) < 1e-6f;
        ok = ok && simp_is_wall(s, 3, 0) && simp_is_wall(s, 4, 0) &&
             !simp_is_wall(s, 0, 0) && !simp_is_wall(s, 6, 0);   /* poly raster */
        ok = ok && simp_user_cost(s)[2 * 10 + 2] > 4.9f;          /* cost rect */
        ok = ok && simp_count(s) > 0;                            /* packs spawned */
        const uint8_t *fl = simp_flags_arr(s);
        for (int i = 0; i < simp_count(s); i++)
            if (!(fl[i] & SIMP_DORMANT)) ok = 0;                  /* all dormant */
        printf("parse: %dx%d walls+cost+packs=%d | %s\n",
               sc.gw, sc.gh, simp_count(s), ok ? "ok" : "BAD");
        simp_destroy(s);
    } else {
        printf("parse: load/instantiate failed | BAD\n");
    }
    scene_free(&sc);
    return ok;
}

static int determinism(void) {
    Scene sc;
    if (scene_load(TMP, &sc) != 0) return 0;
    SimP *s1 = scene_instantiate(&sc, 256);
    SimP *s2 = scene_instantiate(&sc, 256);
    int ok = s1 && s2 && simp_count(s1) == simp_count(s2);
    if (ok) {
        const float *x1 = simp_px(s1), *y1 = simp_py(s1);
        const float *x2 = simp_px(s2), *y2 = simp_py(s2);
        for (int i = 0; i < simp_count(s1); i++)
            if (x1[i] != x2[i] || y1[i] != y2[i]) { ok = 0; break; }
    }
    printf("determinism: %d agents twice | %s\n",
           s1 ? simp_count(s1) : -1, ok ? "ok" : "BAD");
    if (s1) simp_destroy(s1);
    if (s2) simp_destroy(s2);
    scene_free(&sc);
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= roundtrip();
    ok &= parse_and_instantiate();
    ok &= determinism();
    remove(TMP);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

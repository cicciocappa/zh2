/* test_scene.c — scene file loader verification.
 *
 *   1. ROUNDTRIP: build a scene in code, save, load, compare every layer.
 *   2. PARSE: a hand-written file with directives (cell, set, cost, short
 *      rows, comments) instantiates with the right params, walls, goals,
 *      user cost and dormant packs.
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
    if (scene_alloc(&a, 60, 40, 0.5f) != 0) return 0;
    for (int x = 0; x < 60; x++) { a.wall[x] = 1; a.wall[39 * 60 + x] = 1; }
    for (int y = 10; y < 20; y++) a.goal[y * 60 + 55] = 1;
    for (int y = 15; y < 25; y++) a.spawn[y * 60 + 3] = 1;
    for (int y = 18; y < 22; y++) for (int x = 20; x < 26; x++)
        a.pack[y * 60 + x] = 1;
    a.n_set = 2;
    strcpy(a.set[0].name, "k_density");  a.set[0].value = 3.0f;
    strcpy(a.set[1].name, "pbd_iters");  a.set[1].value = 5.0f;
    a.n_cost = 1;
    a.cost[0].x0 = 10; a.cost[0].y0 = 5; a.cost[0].x1 = 30; a.cost[0].y1 = 8;
    a.cost[0].w = 2.5f;

    int ok = scene_save(TMP, &a) == 0;
    Scene b;
    ok = ok && scene_load(TMP, &b) == 0;
    ok = ok && b.gw == a.gw && b.gh == a.gh && fabsf(b.cell - a.cell) < 1e-6f;
    if (ok) {
        for (int i = 0; i < 60 * 40; i++) {
            if (a.wall[i] != b.wall[i] || a.goal[i] != b.goal[i] ||
                a.spawn[i] != b.spawn[i] || a.pack[i] != b.pack[i]) { ok = 0; break; }
        }
    }
    ok = ok && b.n_set == 2 && strcmp(b.set[0].name, "k_density") == 0 &&
         fabsf(b.set[0].value - 3.0f) < 1e-6f;
    ok = ok && b.n_cost == 1 && b.cost[0].x1 == 30 &&
         fabsf(b.cost[0].w - 2.5f) < 1e-6f;
    printf("roundtrip: %dx%d set=%d cost=%d | %s\n",
           b.gw, b.gh, b.n_set, b.n_cost, ok ? "ok" : "BAD");
    scene_free(&a);
    scene_free(&b);
    return ok;
}

static int parse_and_instantiate(void) {
    FILE *f = fopen(TMP, "w");
    if (!f) return 0;
    fputs("# hand-written test scene\n"
          "cell 1.0\n"
          "set k_density 0\n"
          "set v_max 2.0\n"
          "cost 2 2 4 4 5.0\n"
          "map\n"
          "##########\n"
          "#...PP..G#\n"
          "#S..PP\n"                     /* short row: padded with floor */
          "##########\n", f);
    fclose(f);

    Scene sc;
    int ok = scene_load(TMP, &sc) == 0;
    ok = ok && sc.gw == 10 && sc.gh == 4 && fabsf(sc.cell - 1.0f) < 1e-6f;
    SimP *s = ok ? scene_instantiate(&sc, 256) : NULL;
    ok = ok && s != NULL;
    if (ok) {
        ok = ok && fabsf(simp_params(s)->v_max - 2.0f) < 1e-6f;
        ok = ok && simp_is_wall(s, 0, 0) && simp_is_wall(s, 9, 3) &&
             !simp_is_wall(s, 1, 1) && !simp_is_wall(s, 9, 2);  /* padding */
        ok = ok && simp_user_cost(s)[2 * 10 + 3] > 4.9f;
        ok = ok && simp_count(s) > 0;            /* packs spawned something */
        const uint8_t *fl = simp_flags_arr(s);
        for (int i = 0; i < simp_count(s); i++)
            if (!(fl[i] & SIMP_DORMANT)) ok = 0; /* all of them dormant */
        ok = ok && sc.spawn[2 * 10 + 1];         /* S cell reported back */
        printf("parse: %dx%d packs=%d agents | %s\n",
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

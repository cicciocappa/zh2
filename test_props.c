/* test_props.c — prop catalog loader verification (EDITOR_DESIGN §10 stadio 5b).
 *
 *   1. parse a hand-written catalog: count, fields, "-" -> placeholder mesh.
 *   2. find by key (hit + miss).
 *   3. missing file -> negative return.
 *   4. unified entity axes (ENTITY_DESIGN.md §6): the fence/bus/wreck rows of
 *      the design doc parse into solid/hp/opacity/mass; "-" placeholders skip
 *      the destructible trio; old-format lines stay inert (retro-compat).
 */
#include "props.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#define TMP "test_props_tmp.txt"

int main(void) {
    int ok = 1;

    FILE *f = fopen(TMP, "w");
    if (!f) { printf("FAIL (tmp)\n"); return 1; }
    fputs("# comment line\n"
          "bench  assets/models/props/bench.glb  1.0  Bench\n"
          "sign   -                       2.5  Sign\n"
          "cart   assets/models/props/cart.glb   1.2\n"     /* label omitted -> key */
          "bad_line_only_one_token\n",               /* malformed -> skipped */
          f);
    fclose(f);

    PropCatalog c;
    int n = prop_catalog_load(TMP, &c);
    ok = ok && n == 3 && c.n == 3;

    const PropDef *b = prop_catalog_find(&c, "bench");
    ok = ok && b && strcmp(b->mesh, "assets/models/props/bench.glb") == 0 &&
         fabsf(b->scale - 1.0f) < 1e-6f && strcmp(b->label, "Bench") == 0;

    const PropDef *s = prop_catalog_find(&c, "sign");
    ok = ok && s && s->mesh[0] == '\0' &&                 /* "-" -> placeholder */
         fabsf(s->scale - 2.5f) < 1e-6f;

    const PropDef *ca = prop_catalog_find(&c, "cart");
    ok = ok && ca && strcmp(ca->label, "cart") == 0;       /* defaulted to key */

    ok = ok && prop_catalog_find(&c, "nope") == NULL;      /* miss */

    PropCatalog c2;
    ok = ok && prop_catalog_load("does_not_exist.txt", &c2) < 0;

    printf("catalog: n=%d | %s\n", n, ok ? "ok" : "BAD");
    remove(TMP);

    /* ---- 4) unified entity axes (ENTITY_DESIGN.md §6) ------------------- */
    f = fopen(TMP, "w");
    if (!f) { printf("FAIL (tmp)\n"); return 1; }
    fputs("# key  mesh scale label [trig debris topple] [solid H] [hp mult] [opac] [mass]\n"
          "fence  -    1.0   Fence  -    -      -        solid 1.8  350 0.6   0.3    -\n"
          "bus    -    3.0   Bus    -    -      -        solid 2.6  inf -     1.0    inf\n"
          "wreck  -    1.4   Wreck  -    -      -        -          -   -     0.5    120\n"
          "table  -    1.0   Table  0.8  wood   0.4      -          -   -     -      -\n"
          "bench  -    1.0   Bench\n",                     /* old format: inert */
          f);
    fclose(f);
    int n4 = prop_catalog_load(TMP, &c);
    int ok4 = (n4 == 5);

    const PropDef *fe = prop_catalog_find(&c, "fence");
    ok4 = ok4 && fe && fe->solid && fabsf(fe->height - 1.8f) < 1e-6f &&
          fabsf(fe->hp - 350.0f) < 1e-3f && fabsf(fe->cost_mult - 0.6f) < 1e-6f &&
          fabsf(fe->opacity - 0.3f) < 1e-6f && fe->mass == 0.0f &&
          !fe->destructible;

    const PropDef *bu = prop_catalog_find(&c, "bus");
    ok4 = ok4 && bu && bu->solid && fabsf(bu->height - 2.6f) < 1e-6f &&
          isinf(bu->hp) && fabsf(bu->opacity - 1.0f) < 1e-6f && isinf(bu->mass);

    const PropDef *wr = prop_catalog_find(&c, "wreck");
    ok4 = ok4 && wr && !wr->solid && wr->hp == 0.0f &&
          fabsf(wr->opacity - 0.5f) < 1e-6f && fabsf(wr->mass - 120.0f) < 1e-3f;

    const PropDef *ta = prop_catalog_find(&c, "table");  /* destructible, no axes */
    ok4 = ok4 && ta && ta->destructible && fabsf(ta->trigger_radius - 0.8f) < 1e-6f &&
          strcmp(ta->debris, "wood") == 0 && fabsf(ta->topple - 0.4f) < 1e-6f &&
          !ta->solid && ta->hp == 0.0f && ta->opacity == 0.0f && ta->mass == 0.0f;

    const PropDef *be = prop_catalog_find(&c, "bench");  /* old format */
    ok4 = ok4 && be && !be->destructible && !be->solid && be->hp == 0.0f &&
          be->opacity == 0.0f && be->mass == 0.0f;

    printf("entity axes: n=%d fence/bus/wreck/table/bench %s\n",
           n4, ok4 ? "ok" : "BAD");
    ok = ok && ok4;
    remove(TMP);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

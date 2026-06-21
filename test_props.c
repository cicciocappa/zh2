/* test_props.c — prop catalog loader verification (EDITOR_DESIGN §10 stadio 5b).
 *
 *   1. parse a hand-written catalog: count, fields, "-" -> placeholder mesh.
 *   2. find by key (hit + miss).
 *   3. missing file -> negative return.
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
          "bench  meshes/props/bench.glb  1.0  Bench\n"
          "sign   -                       2.5  Sign\n"
          "cart   meshes/props/cart.glb   1.2\n"     /* label omitted -> key */
          "bad_line_only_one_token\n",               /* malformed -> skipped */
          f);
    fclose(f);

    PropCatalog c;
    int n = prop_catalog_load(TMP, &c);
    ok = ok && n == 3 && c.n == 3;

    const PropDef *b = prop_catalog_find(&c, "bench");
    ok = ok && b && strcmp(b->mesh, "meshes/props/bench.glb") == 0 &&
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
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

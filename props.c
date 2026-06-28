/* props.c — pure-decor prop catalog loader. See props.h. */

#define _POSIX_C_SOURCE 200809L   /* strtok_r under -std=c11 */
#include "props.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define strtok_r strtok_s
#endif

int prop_catalog_load(const char *path, PropCatalog *c) {
    memset(c, 0, sizeof *c);
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *save = NULL;
        char *key = strtok_r(line, " \t", &save);
        if (!key || key[0] == '#') continue;
        char *mesh  = strtok_r(NULL, " \t", &save);
        char *scale = strtok_r(NULL, " \t", &save);
        char *label = strtok_r(NULL, " \t", &save);
        if (!mesh || !scale) continue;                 /* malformed: skip */
        if (c->n >= PROP_MAX_DEFS) break;

        PropDef *d = &c->defs[c->n];
        strncpy(d->key, key, PROP_KEY_LEN - 1);
        d->key[PROP_KEY_LEN - 1] = '\0';
        if (strcmp(mesh, "-") != 0) {                  /* "-" -> placeholder */
            strncpy(d->mesh, mesh, PROP_PATH_LEN - 1);
            d->mesh[PROP_PATH_LEN - 1] = '\0';
        }
        d->scale = (float)atof(scale);
        if (d->scale <= 0.0f) d->scale = 1.0f;
        strncpy(d->label, label ? label : key, PROP_LABEL_LEN - 1);
        d->label[PROP_LABEL_LEN - 1] = '\0';
        /* optional destructible config (DESTRUCT_DESIGN.md §3): trig debris topple.
         * Present => destructible; absent => inert decor (defaults stay zero). */
        char *trig   = strtok_r(NULL, " \t", &save);
        char *debris = strtok_r(NULL, " \t", &save);
        char *topple = strtok_r(NULL, " \t", &save);
        if (trig && debris && topple) {
            d->destructible = 1;
            d->trigger_radius = (float)atof(trig);
            if (d->trigger_radius <= 0.0f) d->trigger_radius = 1.0f;
            strncpy(d->debris, debris, PROP_DEBRIS_LEN - 1);
            d->debris[PROP_DEBRIS_LEN - 1] = '\0';
            d->topple = (float)atof(topple);
            if (d->topple < 0.0f) d->topple = 0.0f;
        }
        c->n++;
    }
    fclose(f);
    return c->n;
}

const PropDef *prop_catalog_find(const PropCatalog *c, const char *key) {
    for (int i = 0; i < c->n; i++)
        if (strcmp(c->defs[i].key, key) == 0) return &c->defs[i];
    return NULL;
}

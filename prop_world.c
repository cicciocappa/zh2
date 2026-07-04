/* prop_world.c — apply catalog entity axes at instantiation. See prop_world.h. */

#include "prop_world.h"
#include <math.h>
#include <stdio.h>

typedef struct {
    SimP *s;
    DefGame *g;
    int sid;            /* defense structure id, -1 = permanent wall */
    float cost;         /* per-cell breakthrough cost (absolute)     */
    float opacity;
    int cells;
} FootCtx;

static void cb_foot(void *ud, int cx, int cy) {
    FootCtx *c = (FootCtx *)ud;
    if (c->sid >= 0) def_struct_cell(c->g, c->sid, cx, cy);  /* wall + tier    */
    else             simp_set_wall(c->s, cx, cy, true);
    simp_set_wall_cost(c->s, cx, cy, c->cost);               /* override tier  */
    simp_set_opacity(c->s, cx, cy, c->opacity);              /* after set_wall */
    c->cells++;
}

int prop_world_apply(const Scene *sc, const PropCatalog *cat,
                     SimP *s, DefGame *g, PropWorld *pw) {
    for (int i = 0; i < PROP_WORLD_MAX_STRUCT; i++) pw->struct_is_prop[i] = 0;
    pw->n = sc->n_prop;
    pw->n_solid = pw->n_siege = 0;

    float cell = sc->cell, base = simp_wall_base_cost();
    int gw = simp_grid_w(s), gh = simp_grid_h(s);
    int touched = 0;

    for (int i = 0; i < sc->n_prop; i++) {
        pw->struct_id[i] = -1;
        pw->solid[i] = 0;
        const PropDef *d = prop_catalog_find(cat, sc->prop[i].key);
        if (!d || !d->solid) continue;                /* decor/draggable: no-op */
        if (d->destructible)
            fprintf(stderr, "prop_world: '%s' solid AND destructible — the "
                    "shatter would orphan its wall cells, fix the catalog\n",
                    d->key);

        /* §8.5 footprint: W×D rect yawed by rot, min one nav cell per side */
        float W = d->fw > 0.0f ? d->fw : cell;
        float D = d->fd > 0.0f ? d->fd : cell;
        if (W < cell) W = cell;
        if (D < cell) D = cell;
        float a = sc->prop[i].rot * 0.01745329252f;
        float ca = cosf(a), sa = sinf(a), hw = 0.5f * W, hd = 0.5f * D;
        float lx[4] = { -hw, hw, hw, -hw }, ly[4] = { -hd, -hd, hd, hd };
        float vx[4], vy[4];
        for (int k = 0; k < 4; k++) {
            vx[k] = sc->prop[i].x + lx[k] * ca - ly[k] * sa;
            vy[k] = sc->prop[i].y + lx[k] * sa + ly[k] * ca;
        }

        int siege = g && d->hp > 0.0f && !isinf(d->hp);
        FootCtx ctx = { s, g, -1, 0.0f, 0.0f, 0 };
        if (siege) {
            ctx.sid = def_add_structure(g, d->hp, 0);
            if (ctx.sid < 0) {                        /* STRUCT_CAP hit */
                fprintf(stderr, "prop_world: '%s' out of defense structures, "
                        "degraded to permanent wall\n", d->key);
                siege = 0;
            }
        }
        /* tier: siegeable = author's mult; unbreakable must stay expensive */
        float mult = siege ? d->cost_mult
                   : (d->cost_mult > PROP_WORLD_PALAZZO_MULT
                          ? d->cost_mult : PROP_WORLD_PALAZZO_MULT);
        ctx.cost = mult * base;
        ctx.opacity = d->opacity > 0.0f ? d->opacity : 1.0f; /* solid default */

        scene_raster_cells(vx, vy, 4, cell, gw, gh, cb_foot, &ctx);

        pw->solid[i] = 1;
        pw->n_solid++;
        if (ctx.sid >= 0) {
            pw->struct_id[i] = (int16_t)ctx.sid;
            if (ctx.sid < PROP_WORLD_MAX_STRUCT) pw->struct_is_prop[ctx.sid] = 1;
            pw->n_siege++;
        }
        touched += ctx.cells;
    }

    if (touched) simp_terrain_commit(s);
    return pw->n_solid;
}

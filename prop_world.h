/* prop_world.h — host-side application of the catalog entity axes
 * (ENTITY_DESIGN.md §6 + §8.5) at world instantiation.
 *
 * scene_instantiate rasterizes scene geometry but ignores props (render-only
 * for the sim core); this module walks the Scene props, looks each key up in
 * the PropCatalog and applies the STATIC axes to the live world:
 *
 *  - axis A+G (`solid H WxD`): the prop footprint — a W×D rect (meters)
 *    centered on the prop, yawed by its rot — is rasterized as nav wall
 *    cells via scene_raster_cells. W/D are clamped up to one nav cell
 *    (§8.5: minimum useful thickness); WxD omitted = one cell.
 *  - axis B (`hp mult`): finite hp > 0 -> def_add_structure + def_struct_cell
 *    per footprint cell (siegeable: collapse frees the cells and reroutes),
 *    per-cell breakthrough tier = cost_mult × base. hp inf or 0 -> permanent
 *    wall with tier = max(cost_mult, PROP_WORLD_PALAZZO_MULT): an unbreakable
 *    target must never out-price a breakable one, or the horde sieges forever.
 *  - axis C (`opac`): per-cell bullet opacity AFTER the wall is raised; for
 *    solid props an omitted column (parsed 0) means the solid default 1.0.
 *
 * NOT applied here (v1): axis D — a finite-mass prop is a draggable PBD body,
 * not nav cells (§8.5); it stays pure decor until the drag-body spawn path is
 * wired. Axis E (destruct.c one-shot shatter) keeps its own machinery: a row
 * that is BOTH solid and destructible is a design error (the shatter would
 * leave orphan wall cells) — warned and the solid axis wins for nav, fix the
 * catalog row.
 *
 * Call AFTER scene_instantiate + def_create + wall building, BEFORE any
 * prefill/spawning (agents must not spawn inside a bus). Commits the nav
 * once if anything was raised. Deterministic (pure function of scene+catalog).
 */
#ifndef PROP_WORLD_H
#define PROP_WORLD_H

#include "scene.h"
#include "props.h"
#include "sim_particles.h"
#include "defense.h"

/* wall-cost tier for indestructible solid props; mirrors the 'palazzo' tier
 * of the terrain-hole statics in the host (PALAZZO_WALL_MULT). */
#define PROP_WORLD_PALAZZO_MULT 10.0f

/* upper bound on defense structure ids we track (>= defense.c STRUCT_CAP) */
#define PROP_WORLD_MAX_STRUCT 256

typedef struct {
    int     n;                              /* = sc->n_prop at apply time     */
    int16_t struct_id[SCENE_MAX_PROP];      /* defense struct id, -1 = none   */
    unsigned char solid[SCENE_MAX_PROP];    /* 1 = footprint raised           */
    unsigned char struct_is_prop[PROP_WORLD_MAX_STRUCT]; /* render: the grey
                                     struct-cell boxes skip prop structures
                                     (the prop draws its own mesh)            */
    int     n_solid;                        /* props whose footprint applied  */
    int     n_siege;                        /* of which siegeable (finite hp) */
} PropWorld;

/* Apply the catalog axes to the world. Returns n_solid (>= 0). g may be NULL
 * (no defense game, e.g. plain sim tests): siegeable props degrade to
 * permanent walls with their own cost_mult tier. */
int prop_world_apply(const Scene *sc, const PropCatalog *cat,
                     SimP *s, DefGame *g, PropWorld *pw);

#endif /* PROP_WORLD_H */

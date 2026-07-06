/* props.h — pure-decor prop CATALOG (EDITOR_DESIGN §10 stadio 5b).
 *
 * A text, data-driven catalog mapping a prop KEY (as stored in SceneProp.key)
 * to a render mesh + editor metadata. Pure decor: NO sim/nav effect — the sim
 * core never sees props. Host-side only (renderer + editor), zero deps.
 *
 * FILE FORMAT (props/catalog.txt; '#' comments; whitespace-separated):
 *
 *     # key   mesh                     scale  label
 *     bench   assets/models/props/bench.glb   1.0    Bench
 *     sign    assets/models/props/sign.glb    1.0    Sign
 *     cart    -                        1.2    Cart      # '-' mesh = placeholder
 *
 * `mesh` is a .glb path relative to the working dir, or "-" for none (the host
 * draws a procedural placeholder). `scale` multiplies the render size; `label`
 * is shown in the editor HUD. Missing mesh FILES also fall back to placeholder
 * at load time (host decides) so a level authored before the art exists still
 * shows where the props are.
 *
 * UNIFIED ENTITY AXES (ENTITY_DESIGN.md §6) — optional trailing columns, all
 * "-" = today's inert decor (retro-compatible; the destructible trio accepts
 * "-" placeholders so later columns can be reached):
 *
 *     # key  mesh scale label [trig debris topple] [solid H [WxD]] [hp mult] [opac] [mass] [resist burn]
 *     fence  -    1.0   Fence  -    -      -        solid 1.8 2x0.5   350 0.6   0.3    -    -   -
 *     bus    -    3.0   Bus    -    -      -        solid 2.6 11x2.5  inf -     1.0    inf  -   6
 *     wreck  -    1.4   Wreck  -    -      -        -                 -   -     0.5    120  -   -
 *
 * `solid H` = the prop's footprint is raised as nav wall (height H meters,
 * render extrusion + flyer clearance); the optional `WxD` token right after H
 * (ENTITY_DESIGN §8.5, e.g. "2x0.5") is the footprint rectangle in meters,
 * W along the prop's local x and D along local y, yawed by the instance rot;
 * omitted = one nav cell. `hp mult` = siegeable structure with
 * that HP pool ("inf" = indestructible) and wall-cost tier multiplier;
 * `opac` = bullet opacity 0..1 (axis C, simp_set_opacity); `mass` = draggable
 * mass in walker units ("inf" or "-" with solid = not draggable). `resist` =
 * blast damage threshold (EXPLOSION_DESIGN §5) and `burn` = seconds of fire on
 * ignition. The host applies these at instantiation via prop_world_apply
 * (prop_world.h); the catalog only stores them.
 */
#ifndef PROPS_H
#define PROPS_H

#define PROP_MAX_DEFS  64
#define PROP_KEY_LEN   24
#define PROP_PATH_LEN  128
#define PROP_LABEL_LEN 24
#define PROP_DEBRIS_LEN 16

typedef struct {
    char  key[PROP_KEY_LEN];
    char  mesh[PROP_PATH_LEN];   /* "" -> procedural placeholder */
    float scale;
    char  label[PROP_LABEL_LEN];
    /* destructible decor (DESTRUCT_DESIGN.md): the horde shatters it on contact.
     * Default = inert decor (today's behavior). Set via extra catalog tokens. */
    int   destructible;          /* 0 = pure decor, 1 = shatters on contact   */
    float trigger_radius;        /* m: an agent within this of the prop fires it */
    char  debris[PROP_DEBRIS_LEN]; /* FX style ("wood"/"metal"/...), host->preset */
    float topple;                /* s of topple-tilt before bursting (0=instant) */
    /* unified entity axes (ENTITY_DESIGN.md §6); zeros = inert decor. */
    int   solid;                 /* 1 = footprint raised as nav wall (axis A)  */
    float height;                /* m, solid extrusion (axis G); 0 if !solid   */
    float fw, fd;                /* §8.5 footprint W×D (m); 0 = one nav cell   */
    float hp;                    /* >0 = siegeable (axis B); INFINITY = indestructible */
    float cost_mult;             /* wall-cost tier multiplier (1 = barricade)  */
    float opacity;               /* bullet opacity 0..1 (axis C)               */
    float mass;                  /* draggable mass, walker units (axis D);
                                    0 = not draggable, INFINITY = immovable    */
    /* explosion response (EXPLOSION_DESIGN.md §5); zeros = default. */
    float resist;                /* blast damage threshold: D(d) <= resist =>
                                    ignore the blast (0 = light decor always
                                    bursts). Ignored for hp props (pool decides) */
    float burn;                  /* seconds of fire on ignition (0 = doesn't burn) */
} PropDef;

typedef struct {
    PropDef defs[PROP_MAX_DEFS];
    int     n;
} PropCatalog;

/* Load the catalog. Returns the number of defs parsed (>=0) on success, or a
 * negative value if the file can't be opened. Lines that are malformed or
 * overflow PROP_MAX_DEFS are skipped (the catalog is best-effort). */
int prop_catalog_load(const char *path, PropCatalog *c);

/* Find a def by key, or NULL. */
const PropDef *prop_catalog_find(const PropCatalog *c, const char *key);

#endif /* PROPS_H */

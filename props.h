/* props.h — pure-decor prop CATALOG (EDITOR_DESIGN §10 stadio 5b).
 *
 * A text, data-driven catalog mapping a prop KEY (as stored in SceneProp.key)
 * to a render mesh + editor metadata. Pure decor: NO sim/nav effect — the sim
 * core never sees props. Host-side only (renderer + editor), zero deps.
 *
 * FILE FORMAT (props/catalog.txt; '#' comments; whitespace-separated):
 *
 *     # key   mesh                     scale  label
 *     bench   meshes/props/bench.glb   1.0    Bench
 *     sign    meshes/props/sign.glb    1.0    Sign
 *     cart    -                        1.2    Cart      # '-' mesh = placeholder
 *
 * `mesh` is a .glb path relative to the working dir, or "-" for none (the host
 * draws a procedural placeholder). `scale` multiplies the render size; `label`
 * is shown in the editor HUD. Missing mesh FILES also fall back to placeholder
 * at load time (host decides) so a level authored before the art exists still
 * shows where the props are.
 */
#ifndef PROPS_H
#define PROPS_H

#define PROP_MAX_DEFS  64
#define PROP_KEY_LEN   24
#define PROP_PATH_LEN  128
#define PROP_LABEL_LEN 24

typedef struct {
    char  key[PROP_KEY_LEN];
    char  mesh[PROP_PATH_LEN];   /* "" -> procedural placeholder */
    float scale;
    char  label[PROP_LABEL_LEN];
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

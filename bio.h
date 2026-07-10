/* bio.h — biomass economy (BIOMASS_DESIGN.md).
 *
 * Game-side module, zero-dep (no sim, no defense, no libm): the state of the
 * ONE base converter. Every kill yields raw biomass (the HOST maps DefBody ->
 * yield and calls bio_add; this module doesn't know what a zombie or a turret
 * is — it talks in abstract items). The converter turns the raw tank into the
 * SELECTED output item the instant the cost threshold is crossed; each item
 * store is capped, and biomass arriving while the selected store is full is
 * WASTED (the design's pressure point — the HUD must surface bio_full()).
 *
 * Two currencies, two phases: the placement budget ($, defense.c) buys the
 * PREP; biomass is the ASSAULT currency. They never convert.
 *
 * Determinism: pure arithmetic on floats, no RNG, no allocation. Same call
 * sequence => same state.
 */
#ifndef BIO_H
#define BIO_H

#ifdef __cplusplus
extern "C" {
#endif

/* The items (BIOMASS_DESIGN §6). The four ammo entries map 1:1 onto
 * DefTurretKind (light/heavy/flame/acid) — the mapping lives in the host. */
typedef enum {
    BIO_AMMO_LIGHT = 0,   /* instant reload of a light turret          */
    BIO_AMMO_HEAVY,       /* idem, heavy                               */
    BIO_AMMO_FUEL,        /* idem, flame                               */
    BIO_AMMO_ACID,        /* idem, acid                                */
    BIO_MORTAR,           /* one mortar strike                         */
    BIO_REPAIR,           /* repair kit: +HP to one structure          */
    BIO_UPGRADE,          /* upgrade kit: Blocco 3 panel currency      */
    BIO_ITEM_COUNT
} BioItem;

/* Fired the instant an item pops out of the converter (sound/flash hook,
 * same pattern as the defense events). */
typedef void (*BioProducedFn)(void *user, int item);

/* Public struct (like DefTurret): the host owns one instance, tests poke it
 * directly. All tuning is runtime state — balance.cfg (Blocco 3) will feed
 * the setters below. */
typedef struct {
    float tank;                     /* raw biomass buffer (persists across
                                       output switches — §12.Q1)          */
    int   output;                   /* BioItem currently produced         */
    int   store[BIO_ITEM_COUNT];    /* items in stock                     */
    float cost[BIO_ITEM_COUNT];     /* current biomass cost per item      */
    float cost_base[BIO_ITEM_COUNT];/* pre-upgrade cost (floor anchor)    */
    int   cap[BIO_ITEM_COUNT];      /* current store cap per item         */
    int   cap_base[BIO_ITEM_COUNT]; /* pre-upgrade cap (ceiling anchor)   */
    float wasted;                   /* total biomass thrown away (HUD)    */
    float k_conv;                   /* cost multiplier per converter
                                       upgrade level (§7, default 0.9)    */
    int   lv_cost, lv_cap;          /* upgrade levels taken per branch    */
    BioProducedFn produced; void *produced_user;
} Bio;

/* Defaults from BIOMASS_DESIGN §6: costs 25/35/30/30/40/60/100, caps all 3,
 * k_conv 0.9, stores empty, output = BIO_AMMO_LIGHT. */
void  bio_init(Bio *b);

void  bio_set_output(Bio *b, int item);        /* clamped to a valid item   */
int   bio_output(const Bio *b);

/* Credit `biomass` from a kill. If the SELECTED store is at cap the whole
 * amount is wasted (tank untouched — the player is told via bio_full);
 * otherwise it accrues and converts greedily while the threshold is met. */
void  bio_add(Bio *b, float biomass);

/* Consume one item from stock. 1 = consumed, 0 = store empty. */
int   bio_take(Bio *b, int item);

int   bio_count(const Bio *b, int item);
float bio_cost(const Bio *b, int item);
int   bio_cap(const Bio *b, int item);
int   bio_full(const Bio *b);       /* 1 = selected output at cap ("STOCCAGGIO
                                       PIENO": incoming biomass is wasted)  */
float bio_tank(const Bio *b);
float bio_wasted(const Bio *b);

/* Balance knobs (balance.cfg / scene `biostock`). set_cost re-anchors the
 * base too (upgrades apply on top); set_store clamps to [0, cap]. */
void  bio_set_cost(Bio *b, int item, float cost);
void  bio_set_cap(Bio *b, int item, int cap);
void  bio_set_store(Bio *b, int item, int n);
void  bio_set_k_conv(Bio *b, float k);
void  bio_set_produced_cb(Bio *b, BioProducedFn fn, void *user);

/* Converter self-upgrade (§7): ONE call = one level — the host consumes a
 * BIO_UPGRADE kit first (bio_take), then picks a branch. Costs: every item
 * cost *= k_conv, floored at 0.5*cost_base. Caps: every item cap += 1,
 * ceiling cap_base+3. Return 1 = applied, 0 = branch saturated (the host
 * should NOT have consumed the kit — check bio_can_upgrade_* first). */
int   bio_upgrade_costs(Bio *b);
int   bio_upgrade_caps(Bio *b);
int   bio_can_upgrade_costs(const Bio *b);
int   bio_can_upgrade_caps(const Bio *b);

#ifdef __cplusplus
}
#endif
#endif /* BIO_H */

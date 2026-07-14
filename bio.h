/* bio.h — biomass economy, v2 (BIOMASS_DESIGN.md).
 *
 * Game-side module, zero-dep (no sim, no defense, no libm): ONE tank of ONE
 * currency. Every kill yields biomass (the HOST maps DefBody -> yield and calls
 * bio_add); the tank is capped, and biomass arriving on a full tank is WASTED
 * (the design's pressure point — the HUD must surface it). Actions (mortar
 * shell, repair, instant reload) are paid straight out of the tank at their
 * host-side cost: no converter, no per-item store, nothing to select.
 *
 * Two currencies, two phases: the placement budget ($, defense.c) buys the
 * PREP; biomass is the ASSAULT currency (and, later, the debrief's). They
 * never convert.
 *
 * Determinism: pure arithmetic on floats, no RNG, no allocation. Same call
 * sequence => same state.
 */
#ifndef BIO_H
#define BIO_H

#ifdef __cplusplus
extern "C" {
#endif

#define BIO_CAP_DEFAULT 500.0f

/* Public struct (like DefTurret): the host owns one instance, tests poke it
 * directly. */
typedef struct {
    float tank;     /* biomass held, always within [0, cap]                */
    float cap;      /* capacity (§3; raised by the debrief upgrade)        */
    float wasted;   /* total biomass thrown away on a full tank (HUD)      */
} Bio;

/* start = initial biomass (scene `biotank`), cap <= 0 -> BIO_CAP_DEFAULT. */
void  bio_init(Bio *b, float start, float cap);

/* Credit a kill. Returns the amount WASTED (tank was at cap): > 0 drives the
 * HUD flash. Non-positive amounts are ignored. */
float bio_add(Bio *b, float biomass);

/* Pay for an action. 1 = paid (tank debited), 0 = not enough biomass (tank
 * untouched — the host plays the "no" sound). cost <= 0 is a free action: paid
 * without touching the tank. */
int   bio_take(Bio *b, float cost);

float bio_tank(const Bio *b);
float bio_cap(const Bio *b);
float bio_wasted(const Bio *b);
int   bio_full(const Bio *b);      /* 1 = at cap: incoming kills are wasted */

/* Debrief upgrade (§7) / balance: raise the capacity. Lowering it clamps the
 * tank down (the excess is NOT counted as waste: it never was a kill). */
void  bio_set_cap(Bio *b, float cap);

#ifdef __cplusplus
}
#endif
#endif /* BIO_H */

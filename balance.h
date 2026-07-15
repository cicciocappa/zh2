/* balance.h — runtime game-balance table (Blocco 3; the "balance.cfg" the
 * project memory has been promising).
 *
 * ONE struct holding every tunable that shapes attack vs defense, filled with
 * the historical compiled defaults by balance_defaults() and optionally
 * overridden from a flat `key = value` text file (assets/balance.cfg) by
 * balance_load(). Game-side aggregator: it embeds defense's DefTuning (so the
 * enemy table and combat knobs are not duplicated) plus the host-side knobs
 * that used to be #defines in vat_horde.c / catalog rows in PL_CAT_GAME.
 *
 * File format: one `key = value` per line, `#` starts a comment (full line or
 * trailing), blank lines ok, keys are the dotted names in the KEYS table of
 * balance.c. A missing file means "all defaults"; an unknown key or malformed
 * line is warned on stderr and skipped (never fatal — the game must boot).
 * Missing keys keep their default: a partial file is legal.
 *
 * Zero-dep (libc only), deterministic: same file bytes => same struct.
 */
#ifndef BALANCE_H
#define BALANCE_H

#include "defense.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-kind turret row, indexed by DefTurretKind (LIGHT/HEAVY/FLAME/ACID).
 * Applies to BOTH the PREP placement catalog and scene-authored turrets. */
typedef struct {
    float range;         /* m                                                */
    float fire_period;   /* s between shots (FLAME/ACID: activation tick)    */
    float damage;        /* HP per shot (heavy ignores it: gibs)             */
    float arc_deg;       /* placement aim-arc TOTAL width (scene may author) */
    float hp;            /* destructible emplacement HP                      */
    int   cost;          /* PREP budget ($)                                  */
    int   mag;           /* magazine size, 0 = infinite                      */
    float reload_cost;   /* instant-reload biomass price                     */
} BalTurret;

typedef struct {
    DefTuning def;       /* defense-side: enemy table, DoT, siege, director mix */

    BalTurret tur[4];    /* per DefTurretKind                                */
    float turret_reload_s;   /* silent auto-reload window (s, all kinds)     */

    struct { int cost; float hp, mass; } barricade;          /* full solid   */
    struct { int cost; float hp, mass, opacity; } fence;     /* cancellata   */
    struct { int cost; float trigger_r, blast_r, damage,
             strength, up_ratio, arm_s; } mine;

    struct { float cost, delay, min_range, max_range, cooldown,
             blast_r, blast_damage; } mortar;    /* cost in biomass          */

    struct { float cap, repair_rate, adjust_cost;
             float yield[BT_COUNT]; } bio;       /* yield per DefBody kill   */

    struct { float weight, radius, linger; } lure;   /* fire lure (w>=0 off) */
    struct { float turret_dps, turret_reach; } contact; /* turret mob siege  */
    struct { float base_rate, rate_ramp, wave_period; } director; /* legacy demo */

    int   budget;        /* default PREP budget (scene `budget` overrides)   */
    float lz_hp;         /* LZ core HP                                       */
    float fall_damage;   /* light HP on landing after a loft                 */
} Balance;

/* Fill with the historical compiled defaults (memset first: the struct is
 * memcmp-comparable afterwards). */
void balance_defaults(Balance *b);

/* Apply `key = value` overrides from path on top of the current contents.
 * Returns the number of keys applied, or -1 if the file can't be opened
 * (b untouched). If bad is non-NULL it receives the count of malformed or
 * unknown lines (each also warned on stderr with its line number). */
int  balance_load(Balance *b, const char *path, int *bad);

/* Write every key with its current value (no comments — template/debug dump).
 * Returns 0, or -1 if the file can't be written. */
int  balance_save(const Balance *b, const char *path);

#ifdef __cplusplus
}
#endif
#endif /* BALANCE_H */

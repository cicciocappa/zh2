/* place.h — runtime player placement (PLACEMENT_DESIGN.md).
 *
 * Game-side, core-agnostic like defense.c: it talks to the core through simp_*
 * and to the defensive layer through def_* (budget, structures, turrets). It
 * holds ONLY the placement-session state (selected catalog item, cursor, whether
 * the spot is valid); the ghost rendering and mouse reads live in the caller
 * (vat_horde). Reused by test_place.c (headless) and the VAT sandbox.
 *
 * Unified system: barricades / turrets / draggables (/ traps, future) are
 * catalog ENTRIES, not separate mechanics. Adding a kind = one row + one commit
 * case, no state-machine change.
 *
 * Per-frame in PLAY: pl_set_cursor(pick_y0) → pl_validate (green/red ghost) →
 * pl_commit on click. Deterministic: no RNG, reads world state only.
 */
#ifndef PLACE_H
#define PLACE_H

#include "defense.h"
#include "sim_particles.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PL_BARRICADE, PL_TURRET, PL_BIN, PL_CAR, PL_TRAP, PL_NKINDS } PlKind;
/* PL_TRAP: reserved (PREP_UI_DESIGN §2) — commits are a no-op until def_blast
 * (EXPLOSION_DESIGN) lands; no v1 catalog entry uses it. */

/* Result of pl_validate, drives the ghost colour + HUD message. */
typedef enum { PL_OK, PL_NOFUNDS, PL_BLOCKED, PL_OVERLAP, PL_BADITEM } PlReason;

/* One catalog entry (data-driven, PREP_UI_DESIGN §2): one new item = one row.
 * The trailing combat params use 0 = "standard default" so positional
 * initializers that predate them stay valid (the sandbox catalog). */
typedef struct {
    PlKind      kind;
    const char *name;     /* "Barricata", "Torretta"…                        */
    int         cost;     /* budget units                                    */
    float       w, h;     /* footprint (m), axis-aligned; CAR: w = rod length */
    float       radius;   /* disc radius for bin/car free-space probe        */
    float       hp;       /* barricade HP                                    */
    float       mass;     /* draggable mass (bin/car) / barricade debris mass */
    /* turret combat params (0 = standard: range 40, light 0.12s/40HP,
     * heavy 0.5s/gib). heavy uses defense's slow gibbing shot. */
    int         heavy;
    float       range, fire_period, damage;
    /* barricade cell opacity (axis C): 0 or >=1 = full solid (default);
     * in (0,1) = see-through cover, turrets shoot across (cancellata 0.3).
     * Collapse restores it automatically (simp_set_wall mirrors opacity). */
    float       opacity;
} PlItem;

/* Host veto: return 1 if (wx,wy) sits on an immovable static (palazzo/roccia).
 * NULL = nothing blocked by statics. vat_horde binds this to ter_blocked. */
typedef int (*PlBlockedFn)(void *user, float wx, float wy);

typedef struct {
    const PlItem *cat; int ncat;     /* catalog (borrowed, not owned)        */
    int    active;                   /* placement mode on/off                */
    int    sel;                      /* selected catalog index               */
    float  cx, cy;                   /* cursor in world meters               */
    int    rot90;                    /* 0..3 orientation (barricade/car)     */
    int    valid;                    /* commit allowed here? (pl_validate)   */
    int    reason;                   /* PlReason of the last validate        */
    PlBlockedFn blocked; void *blocked_user;
} Placement;

void          pl_init(Placement *p, const PlItem *catalog, int n);
void          pl_set_blocked_cb(Placement *p, PlBlockedFn fn, void *user);
void          pl_set_cursor(Placement *p, float wx, float wy);
void          pl_cycle(Placement *p, int dir);   /* change selected item     */
void          pl_select(Placement *p, int idx);  /* direct pick (UI tabs);
                                                    out-of-range = ignored   */
void          pl_rotate(Placement *p, int dir);  /* rot90 += dir (wraps)     */
const PlItem *pl_selected(const Placement *p);

/* Recompute validity WITHOUT mutating the world: budget? footprint free? not on
 * a static? Sets p->valid / p->reason, returns p->valid. Call every frame while
 * active. */
int  pl_validate(Placement *p, DefGame *g, SimP *s);

/* Place the selected item: re-runs pl_validate (cheap, no mutation) and refuses
 * if not affordable/valid here, else deducts budget (def_spend) and materializes
 * (simp_drag_add / def_add_turret / def_add_structure+cells+commit). Spends ONLY
 * on a successful create. Returns 1 if placed. Self-contained — safe to call on a
 * click without trusting the cached p->valid. */
int  pl_commit(Placement *p, DefGame *g, SimP *s);

#ifdef __cplusplus
}
#endif
#endif /* PLACE_H */

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
#include "traps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PL_BARRICADE, PL_TURRET, PL_BIN, PL_CAR, PL_TRAP, PL_NKINDS } PlKind;
/* PL_TRAP (PREP_UI_DESIGN §2 / GAME_PLAN fase D): the MINE. Commit registers a
 * TrapDef into the Traps table attached via pl_set_traps (NULL = no-op, e.g.
 * sandbox/tests without traps). The trap params live in the trailing PlItem
 * fields (trig_r/blast_r/blast_dmg/strength/up_ratio/arm_delay). */

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
    float       hp;       /* barricade HP; turret emplacement HP (0 = 250)   */
    float       mass;     /* draggable mass (bin/car) / barricade debris mass */
    /* turret combat params (0 = standard: range 40, light 0.12s/40HP,
     * heavy 0.5s/gib). heavy uses defense's slow gibbing shot. */
    int         heavy;
    float       range, fire_period, damage;
    /* barricade cell opacity (axis C): 0 or >=1 = full solid (default);
     * in (0,1) = see-through cover, turrets shoot across (cancellata 0.3).
     * Collapse restores it automatically (simp_set_wall mirrors opacity). */
    float       opacity;
    /* trap (PL_TRAP) params — see traps.h TrapDef. 0 = commit-side default, so
     * rows that predate these fields (sandbox catalog) stay valid. */
    float       trig_r;      /* proximity trigger radius (m)                  */
    float       blast_r;     /* explosion radius (m)                          */
    float       blast_dmg;   /* explosion peak damage                         */
    float       strength;    /* blast impulse strength                        */
    float       up_ratio;    /* blast impulse loft                            */
    float       arm_delay;   /* s before the mine can fire                    */
    /* turret aim arc: TOTAL width in degrees, centered on the facing the
     * player picks at placement (Placement.facing). 0 = default (90°). */
    float       arc_deg;
} PlItem;

/* Host veto: return 1 if (wx,wy) sits on an immovable static (palazzo/roccia).
 * NULL = nothing blocked by statics. vat_horde binds this to ter_blocked. */
typedef int (*PlBlockedFn)(void *user, float wx, float wy);

/* ---- currency hook (LOOP_DESIGN C) ----------------------------------------
 * Placement charges the defense budget by default. During the ASSAULT the
 * host swaps the wallet to BIOMASS: price() maps a catalog $ cost to the
 * active currency (e.g. ceil(1.5·$)), avail()/spend() read and deduct it.
 * PlLinePlan.cost and validate/commit all go through the wallet, so the HUD
 * shows the active currency for free. Undo refunds do NOT go through the
 * wallet (they restore the budget): swapping wallets invalidates outstanding
 * undo records — clear the stack at the swap (the shell already clears it
 * when the assault starts). NULL wallet = legacy budget behavior. */
typedef struct {
    int  (*price)(void *user, int cost);  /* catalog $ -> active currency    */
    int  (*avail)(void *user);            /* spendable units right now       */
    void (*spend)(void *user, int amount);
    void *user;
} PlWallet;

/* ---- PREP undo (PREP_UI_DESIGN §6) ----------------------------------------
 * Optional LIFO stack of this-session placements. Attach one with
 * pl_set_undo(): successful pl_commit / pl_line_commit push a record; a full
 * stack drops the OLDEST record (undo depth, not a placement cap).
 * pl_undo_pop removes the newest placement: full refund, clean teardown
 * (defense's remove-LAST primitives — valid while placement is the only
 * mutator, i.e. PREP), one nav recommit. Draggables (BIN/CAR, sandbox-only
 * kinds) are NOT recorded. Clear the stack when the assault starts
 * (committing to battle = the point of no return). */
enum { PL_UNDO_MAX = 128 };
typedef struct {
    int nstructs;                    /* structures this placement created    */
    int nturrets;                    /* turrets this placement created       */
    int ntraps;                      /* traps (mines) this placement created */
    int cost;                        /* refunded in full on pop              */
} PlUndoRec;
typedef struct { PlUndoRec rec[PL_UNDO_MAX]; int n; } PlUndo;

typedef struct {
    const PlItem *cat; int ncat;     /* catalog (borrowed, not owned)        */
    int    active;                   /* placement mode on/off                */
    int    sel;                      /* selected catalog index               */
    float  cx, cy;                   /* cursor in world meters               */
    int    rot90;                    /* 0..3 orientation (barricade/car)     */
    float  facing;                   /* turret aim-arc centre (rad, world +x
                                        = 0, CCW); host sets it via the
                                        place-then-drag aiming gesture       */
    int    valid;                    /* commit allowed here? (pl_validate)   */
    int    reason;                   /* PlReason of the last validate        */
    PlBlockedFn blocked; void *blocked_user;
    PlUndo *undo;                    /* optional (NULL = no recording)       */
    Traps  *traps;                   /* PL_TRAP target (NULL = trap commit no-op) */
    const PlWallet *wallet;          /* optional (NULL = defense budget)     */
} Placement;

void pl_set_traps(Placement *p, Traps *t);      /* attach the mine table     */
void pl_set_wallet(Placement *p, const PlWallet *w);  /* NULL = budget      */
void pl_set_undo(Placement *p, PlUndo *u);      /* attach/detach; resets it  */
int  pl_undo_pop(Placement *p, DefGame *g, SimP *s);   /* 1 = one undone     */
void pl_undo_clear(Placement *p);               /* assault begun: keep spent */

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

/* ---- barriers drawn as a LINE (PREP_UI_DESIGN §5) -------------------------
 *
 * The player drags a world-space segment; the line snaps to 0.5 m length
 * granularity (the nav cell — chosen so a residual gap is narrower than the
 * agent disc), keeps its FREE angle, and fills greedily with modules of
 * 2.5 / 1.0 / 0.5 m (e.g. 11 m = 4×2.5 + 1×1). Each module is its own
 * def_add_structure (the horde opens a LOCAL breach), thickness = it->h.
 *
 * Catalog contract for line placement: it->cost is PER METER (module cost =
 * lroundf(len·cost)); it->hp is per 2.5 m module, shorter modules scale
 * proportionally. Same opacity/debris semantics as pl_commit.
 *
 * pl_line_validate fills the plan (modules, snapped geometry, total cost) and
 * p->valid/p->reason WITHOUT mutating; pl_line_commit re-validates and places
 * ALL-OR-NOTHING (any vetoed cell, missing budget or missing structure slots
 * = nothing placed, nothing spent). Selected item must be a PL_BARRICADE. */
enum { PL_LINE_MAX_MODULES = 32 };   /* 80 m of wall per gesture: plenty */
typedef struct {
    int   nmod;
    float mx[PL_LINE_MAX_MODULES];   /* module centers (world m)           */
    float my[PL_LINE_MAX_MODULES];
    float mlen[PL_LINE_MAX_MODULES]; /* module lengths (2.5 / 1.0 / 0.5)   */
    float ang;                       /* line angle (rad, from +x)          */
    float len;                       /* snapped total length (m)           */
    int   cost;                      /* total budget units                 */
} PlLinePlan;

int  pl_line_validate(Placement *p, DefGame *g, SimP *s,
                      float x0, float y0, float x1, float y1, PlLinePlan *out);
int  pl_line_commit(Placement *p, DefGame *g, SimP *s,
                    float x0, float y0, float x1, float y1);

/* Place the selected item: re-runs pl_validate (cheap, no mutation) and refuses
 * if not affordable/valid here, else deducts budget (def_spend) and materializes
 * (simp_drag_add / def_add_turret+make_destructible / def_add_structure+cells+
 * commit). Turrets come out DESTRUCTIBLE like scene turrets (the horde mobs
 * them via the contact siege); emplacement HP from the row (0 = standard 250).
 * Spends ONLY on a successful create. Returns 1 if placed. Self-contained —
 * safe to call on a click without trusting the cached p->valid. */
int  pl_commit(Placement *p, DefGame *g, SimP *s);

#ifdef __cplusplus
}
#endif
#endif /* PLACE_H */

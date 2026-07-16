/* place.c — runtime player placement (PLACEMENT_DESIGN.md). See place.h. */
#include "place.h"
#include <math.h>
#include <string.h>

#define PL_MAX_CELLS 256   /* barricade footprint cap (huge: ~8×1 m is 16) */

/* ---- footprint helpers -------------------------------------------------- */

/* Effective W×H after the 90° rotation (odd rot swaps the axes). */
static void pl_dims(const PlItem *it, int rot90, float *W, float *H) {
    if (rot90 & 1) { *W = it->h; *H = it->w; }
    else           { *W = it->w; *H = it->h; }
}

/* Nav cells a barricade of W×H meters centered on (cx,cy) covers. Returns the
 * count (clamped to PL_MAX_CELLS), filling gx/gy (clamped to the grid). The
 * SAME enumeration is used by validate (free check) and commit (raise walls). */
static int pl_cells(const PlItem *it, float cx, float cy, int rot90,
                    const SimP *s, int *gx, int *gy, int maxn) {
    float cs = simp_cell_size(s);
    int gw = simp_grid_w(s), gh = simp_grid_h(s);
    float W, H; pl_dims(it, rot90, &W, &H);
    int ncx = (int)lroundf(W / cs); if (ncx < 1) ncx = 1;
    int ncy = (int)lroundf(H / cs); if (ncy < 1) ncy = 1;
    float x0 = cx - 0.5f * ncx * cs, y0 = cy - 0.5f * ncy * cs;
    int n = 0;
    for (int j = 0; j < ncy; j++)
        for (int i = 0; i < ncx; i++) {
            if (n >= maxn) return n;
            int cgx = (int)floorf((x0 + (i + 0.5f) * cs) / cs);
            int cgy = (int)floorf((y0 + (j + 0.5f) * cs) / cs);
            if (cgx < 0) cgx = 0; else if (cgx >= gw) cgx = gw - 1;
            if (cgy < 0) cgy = 0; else if (cgy >= gh) cgy = gh - 1;
            gx[n] = cgx; gy[n] = cgy; n++;
        }
    return n;
}

/* Two world-space disc centers of a CAR (rod along the long axis). */
static void pl_car_discs(const PlItem *it, float cx, float cy, int rot90,
                         float *ax, float *ay, float *bx, float *by) {
    float half = 0.5f * it->w;
    if (rot90 & 1) { *ax = cx; *ay = cy - half; *bx = cx; *by = cy + half; }
    else           { *ax = cx - half; *ay = cy; *bx = cx + half; *by = cy; }
}

/* ---- session state ------------------------------------------------------ */

void pl_init(Placement *p, const PlItem *catalog, int n) {
    p->cat = catalog; p->ncat = n;
    p->active = 0; p->sel = 0; p->cx = p->cy = 0.0f; p->rot90 = 0;
    p->facing = 0.0f;
    p->valid = 0; p->reason = PL_BADITEM;
    p->blocked = 0; p->blocked_user = 0;
    p->undo = 0;
    p->traps = 0;
    p->wallet = 0;
}

void pl_set_traps(Placement *p, Traps *t) { p->traps = t; }

void pl_set_wallet(Placement *p, const PlWallet *w) { p->wallet = w; }

/* ---- currency (place.h PlWallet): default = defense budget -------------- */

static int wprice(const Placement *p, int cost) {
    return (p->wallet && p->wallet->price)
         ? p->wallet->price(p->wallet->user, cost) : cost;
}
static int wavail(const Placement *p, DefGame *g) {
    return p->wallet ? p->wallet->avail(p->wallet->user) : (int)def_budget(g);
}
static void wspend(Placement *p, DefGame *g, int amount) {
    if (p->wallet) p->wallet->spend(p->wallet->user, amount);
    else def_spend(g, amount);
}

/* ---- PREP undo (place.h) ------------------------------------------------ */

void pl_set_undo(Placement *p, PlUndo *u) {
    p->undo = u;
    if (u) u->n = 0;
}

void pl_undo_clear(Placement *p) { if (p->undo) p->undo->n = 0; }

static void undo_push(Placement *p, int nstructs, int nturrets, int ntraps, int cost) {
    PlUndo *u = p->undo;
    if (!u) return;
    if (u->n == PL_UNDO_MAX) {                   /* drop the OLDEST record */
        memmove(u->rec, u->rec + 1, (PL_UNDO_MAX - 1) * sizeof u->rec[0]);
        u->n--;
    }
    u->rec[u->n].nstructs = nstructs;
    u->rec[u->n].nturrets = nturrets;
    u->rec[u->n].ntraps = ntraps;
    u->rec[u->n].cost = cost;
    u->n++;
}

int pl_undo_pop(Placement *p, DefGame *g, SimP *s) {
    PlUndo *u = p->undo;
    if (!u || u->n <= 0) return 0;
    PlUndoRec r = u->rec[u->n - 1];
    /* remove-LAST can only fail if someone else mutated defense after our
     * record (LIFO broken, e.g. assault started without clearing): drop the
     * whole stack rather than refund entities we did not remove. */
    for (int k = 0; k < r.ntraps; k++)
        if (p->traps && !traps_remove_last(p->traps)) { u->n = 0; return 0; }
    int walls = 0;
    for (int k = 0; k < r.nturrets; k++) {
        /* placed turrets are DESTRUCTIBLE (backing structure not counted in
         * nstructs): the coupled remove tears down both and frees the wall
         * cell. Unbound turrets (legacy callers) take the plain path. */
        int tid = def_turret_count(g) - 1;
        if (def_remove_turret_bound(g, tid)) { walls = 1; continue; }
        if (!def_remove_turret(g, tid)) { u->n = 0; return 0; }
    }
    for (int k = 0; k < r.nstructs; k++) {
        if (!def_remove_structure(g, def_struct_count(g) - 1)) { u->n = 0; return 0; }
        walls = 1;
    }
    if (walls) simp_terrain_commit(s);           /* one reroute per pop */
    def_set_budget(g, def_budget(g) + r.cost);   /* full refund */
    u->n--;
    return 1;
}

void pl_set_blocked_cb(Placement *p, PlBlockedFn fn, void *user) {
    p->blocked = fn; p->blocked_user = user;
}

void pl_set_cursor(Placement *p, float wx, float wy) { p->cx = wx; p->cy = wy; }

void pl_cycle(Placement *p, int dir) {
    if (p->ncat <= 0) return;
    p->sel = (p->sel + dir) % p->ncat;
    if (p->sel < 0) p->sel += p->ncat;
}

void pl_select(Placement *p, int idx) {
    if (idx >= 0 && idx < p->ncat) p->sel = idx;
}

void pl_rotate(Placement *p, int dir) { p->rot90 = ((p->rot90 + dir) % 4 + 4) % 4; }

const PlItem *pl_selected(const Placement *p) {
    if (p->sel < 0 || p->sel >= p->ncat) return 0;
    return &p->cat[p->sel];
}

/* ---- validity ----------------------------------------------------------- */

int pl_validate(Placement *p, DefGame *g, SimP *s) {
    const PlItem *it = pl_selected(p);
    if (!it) { p->valid = 0; p->reason = PL_BADITEM; return 0; }

    /* 1. funds (wallet: budget in PREP, biomass in ASSAULT) */
    if (wavail(p, g) < wprice(p, it->cost)) {
        p->valid = 0; p->reason = PL_NOFUNDS; return 0; }

    /* 2. on a static (host veto) — checked at the cursor */
    if (p->blocked && p->blocked(p->blocked_user, p->cx, p->cy)) {
        p->valid = 0; p->reason = PL_BLOCKED; return 0;
    }

    /* 3. physical space free (agents / walls / corpses) */
    int free = 1;
    if (it->kind == PL_BARRICADE) {
        float cs = simp_cell_size(s);
        int gx[PL_MAX_CELLS], gy[PL_MAX_CELLS];
        int n = pl_cells(it, p->cx, p->cy, p->rot90, s, gx, gy, PL_MAX_CELLS);
        for (int k = 0; k < n && free; k++)
            if (!simp_free_at(s, (gx[k] + 0.5f) * cs, (gy[k] + 0.5f) * cs, cs * 0.45f))
                free = 0;
    } else if (it->kind == PL_CAR) {
        float ax, ay, bx, by; pl_car_discs(it, p->cx, p->cy, p->rot90, &ax, &ay, &bx, &by);
        free = simp_free_at(s, ax, ay, it->radius) && simp_free_at(s, bx, by, it->radius);
    } else { /* PL_TURRET (point) / PL_BIN (disc) / PL_TRAP (mine, small disc) */
        free = simp_free_at(s, p->cx, p->cy, it->radius);
    }
    if (!free) { p->valid = 0; p->reason = PL_OVERLAP; return 0; }

    p->valid = 1; p->reason = PL_OK; return 1;
}

/* ---- commit ------------------------------------------------------------- */

/* Player turrets are DESTRUCTIBLE like scene turrets (they were not, so the
 * horde never attacked them — 2026-07-08 diagnosis): standard emplacement HP,
 * same default as vat_horde's VAT_HORDE_TURRET_HP for scene turrets. A catalog
 * row with hp > 0 overrides it. */
#define PL_TURRET_HP_STD 250.0f

/* Turret built at the cursor, combat params from the catalog entry
 * (PREP_UI_DESIGN §2). 0 = standard default, so bare rows keep behaving
 * like the old fixed light turret. The aim arc is CENTERED on the facing
 * the player picked with the aiming drag (Placement.facing); width from
 * the catalog (it->arc_deg, 0 = 90° total). arc_min/max stay UNWRAPPED
 * (facing ± half may leave ±pi — defense.c compares bearings via wrap_pi). */
static int commit_turret(DefGame *g, SimP *s, const PlItem *it, float cx, float cy,
                         float facing) {
    DefTurret t = {0};
    float half = (it->arc_deg > 0.0f ? it->arc_deg : 90.0f)
                 * (3.14159265f / 360.0f);
    t.x = cx; t.y = cy;
    t.ang = facing;
    t.arc_min = facing - half; t.arc_max = facing + half;
    t.sweep_dir = 1; t.sweep_speed = 2.5f;
    t.aim_tol = DEF_AIM_TOL_STD;          /* turn-then-shoot (game turret) */
    /* PlItem.heavy carries the full DefTurretKind (0/1 = the legacy pair,
     * 2 = flame, 3 = acid); def_add_turret keeps kind/heavy coherent. */
    t.kind = it->heavy; t.piercing = 0;
    /* per-kind standard defaults (0 in the catalog = use these; the numbers
     * move into the balance table in Blocco 3): */
    float d_range = 40.0f, d_fp = 0.12f, d_dmg = 40.0f;
    if      (t.kind == TUR_HEAVY) { d_fp = 0.5f;  d_dmg = 0.0f;  } /* gibs   */
    else if (t.kind == TUR_FLAME) { d_range = 12.0f; d_fp = 0.15f; d_dmg = 6.0f; }
    else if (t.kind == TUR_ACID)  { d_range = 18.0f; d_fp = 0.25f; d_dmg = 4.0f; }
    t.range       = it->range       > 0.0f ? it->range       : d_range;
    t.fire_period = it->fire_period > 0.0f ? it->fire_period : d_fp;
    t.damage      = it->damage      > 0.0f ? it->damage      : d_dmg;
    int tid = def_add_turret(g, &t);
    if (tid < 0) return 0;
    /* destructible emplacement: the cell becomes a barricade-tier wall the
     * horde can mob (contact siege) — all-or-nothing with the turret. */
    float hp = it->hp > 0.0f ? it->hp : PL_TURRET_HP_STD;
    if (def_turret_make_destructible(g, tid, hp) < 0) {
        def_remove_turret(g, tid);               /* struct table full: roll back */
        return 0;
    }
    simp_terrain_commit(s);                      /* reroute around the emplacement */
    return 1;
}

static int commit_barricade(DefGame *g, SimP *s, const PlItem *it,
                            float cx, float cy, int rot90) {
    int gx[PL_MAX_CELLS], gy[PL_MAX_CELLS];
    int n = pl_cells(it, cx, cy, rot90, s, gx, gy, PL_MAX_CELLS);
    if (n <= 0) return 0;
    int sid = def_add_structure(g, it->hp, 0);
    if (sid < 0) return 0;
    int semi = (it->opacity > 0.0f && it->opacity < 1.0f);   /* cancellata */
    for (int k = 0; k < n; k++) {
        def_struct_cell(g, sid, gx[k], gy[k]);
        /* axis C override AFTER set_wall (prop_world pattern): see-through
         * cover, turrets shoot across; collapse's set_wall(false) mirrors
         * opacity back to 0 on its own. */
        if (semi) simp_set_opacity(s, gx[k], gy[k], it->opacity);
    }
    if (it->mass > 0.0f) def_struct_set_debris(g, sid, it->mass);  /* hybrid */
    simp_terrain_commit(s);                                        /* reroute */
    return 1;
}

/* Register a MINE into the attached Traps table (GAME_PLAN fase D). Trap params
 * come from the trailing PlItem fields; 0 = a sensible commit-side default so a
 * bare catalog row still yields a working mine. No-op (returns 0) with no table
 * attached — sandbox/tests that carry no traps. */
static int commit_trap(Traps *tr, const PlItem *it, float cx, float cy) {
    if (!tr) return 0;
    TrapDef d = {0};
    d.kind      = TRAP_MINE;
    d.x = cx; d.y = cy;
    d.trig_r    = it->trig_r    > 0.0f ? it->trig_r    : 1.2f;
    d.blast_r   = it->blast_r   > 0.0f ? it->blast_r   : 6.0f;
    d.dmg       = it->blast_dmg > 0.0f ? it->blast_dmg : 150.0f;
    d.strength  = it->strength  > 0.0f ? it->strength  : 22.0f;
    d.up        = it->up_ratio  > 0.0f ? it->up_ratio  : 0.6f;
    d.arm_delay = it->arm_delay;                       /* 0 = armed immediately */
    return traps_add(tr, &d) >= 0;
}

static int commit_car(SimP *s, const PlItem *it, float cx, float cy, int rot90) {
    float ax, ay, bx, by; pl_car_discs(it, cx, cy, rot90, &ax, &ay, &bx, &by);
    int a = simp_drag_add(s, ax, ay, it->radius, it->mass);
    int b = simp_drag_add(s, bx, by, it->radius, it->mass);
    if (a < 0 || b < 0) return 0;
    simp_drag_link(s, a, b);
    return 1;
}

/* ---- line placement (PREP_UI_DESIGN §5) --------------------------------- */

/* Cells whose CENTER falls inside a rotated rect (center cx,cy, half extents
 * hu along the line × hv across, angle ca/sa). Same center-sampling semantics
 * as scene_raster_cells, self-contained (no scene.h dep). The along-axis test
 * is HALF-OPEN ([-hu, hu)) so adjacent modules never claim the same cell.
 * NOTE: barrier thickness must be ≥ 2 cells (h = 1.0 m at 0.5 cells) so the
 * rasterized staircase of a slanted line has no diagonal-only pinch points. */
static int pl_rect_cells(const SimP *s, float cx, float cy, float hu, float hv,
                         float ca, float sa, int *gx, int *gy, int maxn) {
    float cs = simp_cell_size(s);
    int gw = simp_grid_w(s), gh = simp_grid_h(s);
    float r = sqrtf(hu * hu + hv * hv);          /* bbox of the rotated rect */
    int x0 = (int)floorf((cx - r) / cs), x1 = (int)floorf((cx + r) / cs);
    int y0 = (int)floorf((cy - r) / cs), y1 = (int)floorf((cy + r) / cs);
    if (x0 < 0) x0 = 0;
    if (x1 >= gw) x1 = gw - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= gh) y1 = gh - 1;
    int n = 0;
    for (int j = y0; j <= y1; j++)
        for (int i = x0; i <= x1; i++) {
            float px = (i + 0.5f) * cs - cx, py = (j + 0.5f) * cs - cy;
            float u = ca * px + sa * py, v = -sa * px + ca * py;
            if (u >= -hu && u < hu && v >= -hv && v < hv) {
                if (n >= maxn) return n;
                gx[n] = i; gy[n] = j; n++;
            }
        }
    return n;
}

/* Snap the segment length to 0.5 m and fill it with 2.5/1.0/0.5 m modules.
 * Fills geometry + cost; returns nmod (0 = degenerate/too long). */
static int pl_line_plan(const PlItem *it, float x0, float y0, float x1, float y1,
                        PlLinePlan *pl) {
    float dx = x1 - x0, dy = y1 - y0;
    float raw = sqrtf(dx * dx + dy * dy);
    float len = 0.5f * lroundf(raw / 0.5f);      /* 0.5 m granularity */
    pl->nmod = 0; pl->cost = 0; pl->len = len; pl->ang = atan2f(dy, dx);
    if (len < 0.5f || raw < 1e-6f) return 0;
    float ux = dx / raw, uy = dy / raw, at = 0.0f;
    static const float MODS[3] = { 2.5f, 1.0f, 0.5f };
    while (len - at > 0.25f) {                   /* residual ≥ one 0.5 module */
        float rem = len - at, m = 0.0f;
        for (int k = 0; k < 3; k++) if (rem >= MODS[k] - 0.01f) { m = MODS[k]; break; }
        if (m == 0.0f || pl->nmod >= PL_LINE_MAX_MODULES) return 0;   /* too long */
        int i = pl->nmod++;
        pl->mx[i] = x0 + ux * (at + 0.5f * m);
        pl->my[i] = y0 + uy * (at + 0.5f * m);
        pl->mlen[i] = m;
        pl->cost += (int)lroundf(m * (float)it->cost);   /* cost is PER METER */
        at += m;
    }
    return pl->nmod;
}

/* Occupancy of one planned module: PL_OK = free, else PL_BLOCKED (host static
 * veto or off-grid) / PL_OVERLAP (agents/walls). */
static int pl_module_status(const Placement *p, SimP *s, const PlLinePlan *pl,
                            int m, float ca, float sa, float hv, float cs) {
    int gx[PL_MAX_CELLS], gy[PL_MAX_CELLS];
    int n = pl_rect_cells(s, pl->mx[m], pl->my[m], 0.5f * pl->mlen[m], hv,
                          ca, sa, gx, gy, PL_MAX_CELLS);
    if (n <= 0) return PL_BLOCKED;
    for (int k = 0; k < n; k++) {
        float wx = (gx[k] + 0.5f) * cs, wy = (gy[k] + 0.5f) * cs;
        if (p->blocked && p->blocked(p->blocked_user, wx, wy)) return PL_BLOCKED;
        if (!simp_free_at(s, wx, wy, cs * 0.45f)) return PL_OVERLAP;   /* agents/walls */
    }
    return PL_OK;
}

/* Validate a wall LINE, AUTO-SHORTENING occupied ENDS instead of vetoing the
 * whole gesture: a shared vertex (chained polyline) or a bumped endpoint slides
 * the near/far point inward along the segment (0.5 m steps, the plan grain) until
 * the end module clears, then the line is re-planned shorter. A genuine obstacle
 * left in the MIDDLE still refuses (PL_OVERLAP/PL_BLOCKED). `out` gets the final
 * shortened plan on success, or the full attempted plan on refusal (red ghost). */
int pl_line_validate(Placement *p, DefGame *g, SimP *s,
                     float x0, float y0, float x1, float y1, PlLinePlan *out) {
    PlLinePlan pl;
    const PlItem *it = pl_selected(p);
    p->valid = 0;
    if (!it || it->kind != PL_BARRICADE || !pl_line_plan(it, x0, y0, x1, y1, &pl)) {
        p->reason = PL_BADITEM; if (out) { out->nmod = 0; out->cost = 0; }
        return 0;
    }
    pl.cost = wprice(p, pl.cost);              /* plan cost in active currency */
    if (out) *out = pl;                        /* default: full plan (refusal ghost) */
    float cs = simp_cell_size(s), hv = 0.5f * it->h;
    float dx = x1 - x0, dy = y1 - y0, raw = sqrtf(dx * dx + dy * dy);
    float ux = dx / raw, uy = dy / raw;        /* raw >= 0.5 here (plan succeeded) */
    const float STEP = 0.5f;
    int reason = PL_OVERLAP;                   /* if the ends never clear */
    /* nudge the two endpoints inward until their modules are free (bounded: the
     * segment self-degenerates once it shrinks below one module). */
    for (int iter = 0; iter < 2 * PL_LINE_MAX_MODULES + 4; iter++) {
        if (!pl_line_plan(it, x0, y0, x1, y1, &pl)) { reason = PL_OVERLAP; break; }
        pl.cost = wprice(p, pl.cost);
        float ca = cosf(pl.ang), sa = sinf(pl.ang);
        if (pl_module_status(p, s, &pl, 0, ca, sa, hv, cs) != PL_OK) {
            x0 += ux * STEP; y0 += uy * STEP; continue;              /* slide start in */
        }
        if (pl_module_status(p, s, &pl, pl.nmod - 1, ca, sa, hv, cs) != PL_OK) {
            x1 -= ux * STEP; y1 -= uy * STEP; continue;              /* pull end in */
        }
        /* both ends clear: a leftover in the MIDDLE is a real obstacle -> refuse */
        int mid = PL_OK;
        for (int m = 1; m < pl.nmod - 1; m++) {
            int st = pl_module_status(p, s, &pl, m, ca, sa, hv, cs);
            if (st != PL_OK) { mid = st; break; }
        }
        if (mid != PL_OK) { p->reason = mid; return 0; }   /* out keeps full plan (red) */
        if (out) *out = pl;                    /* shortened plan (green ghost) */
        if (wavail(p, g) < pl.cost) { p->reason = PL_NOFUNDS; return 0; }
        if (def_struct_count(g) + pl.nmod > def_struct_cap()) {
            p->reason = PL_BLOCKED; return 0;
        }
        p->valid = 1; p->reason = PL_OK; return 1;
    }
    p->reason = reason; return 0;
}

int pl_line_commit(Placement *p, DefGame *g, SimP *s,
                   float x0, float y0, float x1, float y1) {
    PlLinePlan pl;
    if (!pl_line_validate(p, g, s, x0, y0, x1, y1, &pl)) return 0;
    const PlItem *it = pl_selected(p);           /* barricade kind: validated */
    float ca = cosf(pl.ang), sa = sinf(pl.ang), hv = 0.5f * it->h;
    int semi = (it->opacity > 0.0f && it->opacity < 1.0f);
    for (int m = 0; m < pl.nmod; m++) {
        float hp = it->hp * (pl.mlen[m] / 2.5f); /* per-module, length-scaled */
        int sid = def_add_structure(g, hp, 0);   /* room pre-checked */
        int gx[PL_MAX_CELLS], gy[PL_MAX_CELLS];
        int n = pl_rect_cells(s, pl.mx[m], pl.my[m], 0.5f * pl.mlen[m], hv,
                              ca, sa, gx, gy, PL_MAX_CELLS);
        for (int k = 0; k < n; k++) {
            def_struct_cell(g, sid, gx[k], gy[k]);
            if (semi) simp_set_opacity(s, gx[k], gy[k], it->opacity);
        }
        if (it->mass > 0.0f) def_struct_set_debris(g, sid, it->mass);
    }
    simp_terrain_commit(s);                      /* one reroute for the line */
    wspend(p, g, pl.cost);                       /* already in active currency */
    undo_push(p, pl.nmod, 0, 0, pl.cost);        /* one record = whole line */
    return 1;
}

int pl_commit(Placement *p, DefGame *g, SimP *s) {
    if (!pl_validate(p, g, s)) return 0;        /* refuses on nofunds/blocked/overlap */
    const PlItem *it = pl_selected(p);          /* non-NULL: validate passed */

    int ok = 0;
    switch (it->kind) {
        case PL_BARRICADE: ok = commit_barricade(g, s, it, p->cx, p->cy, p->rot90); break;
        case PL_TURRET:    ok = commit_turret(g, s, it, p->cx, p->cy, p->facing); break;
        case PL_BIN:       ok = simp_drag_add(s, p->cx, p->cy, it->radius, it->mass) >= 0; break;
        case PL_CAR:       ok = commit_car(s, it, p->cx, p->cy, p->rot90); break;
        case PL_TRAP:      ok = commit_trap(p->traps, it, p->cx, p->cy); break;
        default: break;
    }
    if (ok) {
        int charged = wprice(p, it->cost);
        wspend(p, g, charged);                  /* deduct only on success */
        if      (it->kind == PL_BARRICADE) undo_push(p, 1, 0, 0, charged);
        else if (it->kind == PL_TURRET)    undo_push(p, 0, 1, 0, charged);
        else if (it->kind == PL_TRAP)      undo_push(p, 0, 0, 1, charged);
        /* BIN/CAR: no draggable-removal primitive — not recorded (place.h) */
    }
    return ok;
}

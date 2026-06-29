/* place.c — runtime player placement (PLACEMENT_DESIGN.md). See place.h. */
#include "place.h"
#include <math.h>

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
    p->valid = 0; p->reason = PL_BADITEM;
    p->blocked = 0; p->blocked_user = 0;
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

void pl_rotate(Placement *p, int dir) { p->rot90 = ((p->rot90 + dir) % 4 + 4) % 4; }

const PlItem *pl_selected(const Placement *p) {
    if (p->sel < 0 || p->sel >= p->ncat) return 0;
    return &p->cat[p->sel];
}

/* ---- validity ----------------------------------------------------------- */

int pl_validate(Placement *p, DefGame *g, SimP *s) {
    const PlItem *it = pl_selected(p);
    if (!it) { p->valid = 0; p->reason = PL_BADITEM; return 0; }

    /* 1. budget */
    if (def_budget(g) < it->cost) { p->valid = 0; p->reason = PL_NOFUNDS; return 0; }

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
    } else { /* PL_TURRET (point) / PL_BIN (disc) */
        free = simp_free_at(s, p->cx, p->cy, it->radius);
    }
    if (!free) { p->valid = 0; p->reason = PL_OVERLAP; return 0; }

    p->valid = 1; p->reason = PL_OK; return 1;
}

/* ---- commit ------------------------------------------------------------- */

/* Standard light turret built at the cursor. Combat params are fixed here
 * (upgrades = future / economy M5b); the catalog carries only cost. */
static int commit_turret(DefGame *g, float cx, float cy) {
    DefTurret t = {0};
    t.x = cx; t.y = cy;
    t.arc_min = -3.14159265f; t.arc_max = 3.14159265f;   /* full sweep */
    t.sweep_dir = 1; t.sweep_speed = 2.5f; t.range = 40.0f;
    t.fire_period = 0.12f; t.damage = 40.0f; t.heavy = 0; t.piercing = 0;
    return def_add_turret(g, &t) >= 0;
}

static int commit_barricade(DefGame *g, SimP *s, const PlItem *it,
                            float cx, float cy, int rot90) {
    int gx[PL_MAX_CELLS], gy[PL_MAX_CELLS];
    int n = pl_cells(it, cx, cy, rot90, s, gx, gy, PL_MAX_CELLS);
    if (n <= 0) return 0;
    int sid = def_add_structure(g, it->hp, 0);
    if (sid < 0) return 0;
    for (int k = 0; k < n; k++) def_struct_cell(g, sid, gx[k], gy[k]);
    if (it->mass > 0.0f) def_struct_set_debris(g, sid, it->mass);  /* hybrid */
    simp_terrain_commit(s);                                        /* reroute */
    return 1;
}

static int commit_car(SimP *s, const PlItem *it, float cx, float cy, int rot90) {
    float ax, ay, bx, by; pl_car_discs(it, cx, cy, rot90, &ax, &ay, &bx, &by);
    int a = simp_drag_add(s, ax, ay, it->radius, it->mass);
    int b = simp_drag_add(s, bx, by, it->radius, it->mass);
    if (a < 0 || b < 0) return 0;
    simp_drag_link(s, a, b);
    return 1;
}

int pl_commit(Placement *p, DefGame *g, SimP *s) {
    if (!pl_validate(p, g, s)) return 0;        /* refuses on nofunds/blocked/overlap */
    const PlItem *it = pl_selected(p);          /* non-NULL: validate passed */

    int ok = 0;
    switch (it->kind) {
        case PL_BARRICADE: ok = commit_barricade(g, s, it, p->cx, p->cy, p->rot90); break;
        case PL_TURRET:    ok = commit_turret(g, p->cx, p->cy); break;
        case PL_BIN:       ok = simp_drag_add(s, p->cx, p->cy, it->radius, it->mass) >= 0; break;
        case PL_CAR:       ok = commit_car(s, it, p->cx, p->cy, p->rot90); break;
        default: break;
    }
    if (ok) def_spend(g, it->cost);             /* deduct only on success */
    return ok;
}

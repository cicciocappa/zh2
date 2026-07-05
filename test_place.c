/* test_place.c — runtime placement verification (PLACEMENT_DESIGN.md §7).
 *
 * Game-side logic (place.c), deterministic, no SDL/GL. Cases:
 *   1) BUDGET gate    — affordable until the pot runs dry, def_budget exact.
 *   2) SPACE veto     — PL_OVERLAP on an occupied spot, nothing placed/spent.
 *   3) STATIC veto    — PL_BLOCKED via the host callback (ter_blocked stand-in).
 *   4) BARRICADE      — placed cells become walls, the horde funnels through the
 *                       gap (drain > 0) and never centers inside a wall.
 *   5) ROTATION       — rot90 transposes the footprint (wide ↔ tall).
 *   6) DETERMINISM    — same script ⇒ same world (checksum), zero NaN.
 *   7) SELECT         — pl_select direct pick, out-of-range ignored.
 *   8) DATA-DRIVEN    — catalog combat params reach the turret (heavy flag,
 *                       period, range); bare rows keep the old defaults.
 *   9) CANCELLATA     — opacity entry: cells solid but transmit in (0,1);
 *                       full barricade blocks (0); collapse restores 1.
 *  10) LINE FILL      — 11 m line = 4×2.5 + 1×1 modules, exact cost (per m),
 *                       one structure per module, snap to 0.5 m.
 *  11) LINE VETO      — all-or-nothing: an obstacle mid-line = nothing
 *                       placed, nothing spent; budget gate on the total.
 *  12) LINE ANGLE     — 45° line: walls raised along the diagonal, agents
 *                       kept out, deterministic (same line ⇒ same cells).
 */
#define _POSIX_C_SOURCE 199309L
#include "place.h"
#include "defense.h"
#include "sim_particles.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)
#define MAXA 4000

/* catalog (mirrors a minimal in-game palette). The first four rows leave the
 * combat fields at 0 = "standard default" — case 8 regresses that contract. */
enum { I_BARR, I_TUR, I_BIN, I_CAR, I_HEAVY, I_FENCE };
static const PlItem CAT[] = {
    /* kind        name          cost   w     h    radius  hp     mass   combat: 0 = default */
    { PL_BARRICADE, "Barricata",   50,  4.0f, 1.0f, 0.0f, 300.0f,  0.0f, 0, 0,0,0, 0 },
    { PL_TURRET,    "Torretta",   100,  1.0f, 1.0f, 0.5f,   0.0f,  0.0f, 0, 0,0,0, 0 },
    { PL_BIN,       "Cassonetto",  20,  0.0f, 0.0f, 0.6f,   0.0f, 12.0f, 0, 0,0,0, 0 },
    { PL_CAR,       "Auto",        60,  3.0f, 0.0f, 0.6f,   0.0f, 20.0f, 0, 0,0,0, 0 },
    { .kind = PL_TURRET, .name = "Pesante", .cost = 250, .radius = 0.5f,
      .heavy = 1, .range = 55.0f },                      /* period/dmg default */
    { .kind = PL_BARRICADE, .name = "Cancellata", .cost = 80,
      .w = 4.0f, .h = 1.0f, .hp = 200.0f, .opacity = 0.3f },
    { .kind = PL_BARRICADE, .name = "Muro-linea", .cost = 12,   /* PER METER */
      .w = 2.5f, .h = 1.0f, .hp = 300.0f },
};
#define NCAT ((int)(sizeof(CAT)/sizeof(CAT[0])))
#define I_LINE 6

static int fails = 0;
#define CHECK(c, ...) do{ if(!(c)){ fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } }while(0)

/* host static-veto stand-in: blocks a 10×10 m box around (100,100). */
static int blocked_box(void *u, float x, float y) {
    (void)u; return (x >= 35.0f && x <= 45.0f && y >= 25.0f && y <= 35.0f);
}

static int any_nan(SimP *s) {
    const float *px = simp_px(s), *py = simp_py(s);
    int n = simp_count(s), bad = 0;
    for (int i = 0; i < n; i++) if (isnan(px[i]) || isnan(py[i])) bad++;
    return bad;
}

/* ---- case 1: budget gate ----------------------------------------------- */
static void test_budget(void) {
    printf("[1] budget gate\n");
    SimP *s = simp_create(GW, GH, CELL, MAXA);
    simp_terrain_commit(s);                      /* init SDF (free_at) — the game steps */
    DefGame *g = def_create(s, MAXA);
    def_set_budget(g, 250);                      /* exactly 2 turrets @100 */
    Placement p; pl_init(&p, CAT, NCAT); p.sel = I_TUR;

    int placed = 0;
    float xs[3] = { 20.0f, 30.0f, 40.0f };
    for (int k = 0; k < 3; k++) {
        pl_set_cursor(&p, xs[k], 20.0f);
        pl_validate(&p, g, s);
        if (k < 2) CHECK(p.valid && p.reason == PL_OK, "turret %d should be valid", k);
        else       CHECK(!p.valid && p.reason == PL_NOFUNDS, "3rd turret should be PL_NOFUNDS (reason=%d)", p.reason);
        if (pl_commit(&p, g, s)) placed++;
    }
    CHECK(placed == 2, "placed %d turrets, expected 2", placed);
    CHECK(def_turret_count(g) == 2, "def_turret_count %d, expected 2", def_turret_count(g));
    CHECK(def_budget(g) == 50, "budget left %d, expected 50", def_budget(g));

    def_destroy(g); simp_destroy(s);
}

/* ---- case 2: space veto ------------------------------------------------- */
static void test_space(void) {
    printf("[2] space veto\n");
    SimP *s = simp_create(GW, GH, CELL, MAXA);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, MAXA);
    def_set_budget(g, 1000);
    Placement p; pl_init(&p, CAT, NCAT); p.sel = I_BIN;

    simp_spawn(s, 30.0f, 30.0f);                 /* occupy the spot */
    int b0 = def_budget(g);

    pl_set_cursor(&p, 30.0f, 30.0f);
    pl_validate(&p, g, s);
    CHECK(!p.valid && p.reason == PL_OVERLAP, "bin on agent should be PL_OVERLAP (reason=%d)", p.reason);
    CHECK(!pl_commit(&p, g, s), "commit on occupied spot must fail");
    CHECK(simp_drag_count(s) == 0, "no draggable should exist (got %d)", simp_drag_count(s));
    CHECK(def_budget(g) == b0, "budget must be untouched (%d vs %d)", def_budget(g), b0);

    pl_set_cursor(&p, 55.0f, 40.0f);             /* free spot */
    pl_validate(&p, g, s);
    CHECK(p.valid, "free spot should validate");
    CHECK(pl_commit(&p, g, s), "commit on free spot should place");
    CHECK(simp_drag_count(s) == 1, "one draggable expected (got %d)", simp_drag_count(s));
    CHECK(def_budget(g) == b0 - CAT[I_BIN].cost, "budget should drop by bin cost");

    def_destroy(g); simp_destroy(s);
}

/* ---- case 3: static veto ------------------------------------------------ */
static void test_static(void) {
    printf("[3] static veto\n");
    SimP *s = simp_create(GW, GH, CELL, MAXA);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, MAXA);
    def_set_budget(g, 1000);
    Placement p; pl_init(&p, CAT, NCAT); p.sel = I_BIN;
    pl_set_blocked_cb(&p, blocked_box, 0);

    pl_set_cursor(&p, 40.0f, 30.0f);             /* inside the static box */
    pl_validate(&p, g, s);
    CHECK(!p.valid && p.reason == PL_BLOCKED, "on a static should be PL_BLOCKED (reason=%d)", p.reason);
    CHECK(!pl_commit(&p, g, s), "commit on static must fail");

    pl_set_cursor(&p, 15.0f, 15.0f);             /* clear of the box */
    pl_validate(&p, g, s);
    CHECK(p.valid && p.reason == PL_OK, "clear of static should validate");

    def_destroy(g); simp_destroy(s);
}

/* ---- case 4: barricade walls + reroute through the gap ------------------ */
static void test_barricade(void) {
    printf("[4] barricade walls + reroute\n");
    SimP *s = simp_create(GW, GH, CELL, MAXA);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, MAXA);
    def_set_budget(g, 100000);
    Placement p; pl_init(&p, CAT, NCAT); p.sel = I_BARR; p.rot90 = 1;  /* vertical wall */

    /* goal column just east of the barricade (so the gap is the only way in) */
    for (int cy = 0; cy < GH; cy++) simp_set_goal(s, 100, cy, true);   /* x = 50 m */
    simp_terrain_commit(s);

    /* a VERTICAL barricade wall at x≈40 m spanning y 6..54 m (each piece 4 m tall
     * rotated), leaving an open 4 m gap at y≈30. */
    int segs = 0;
    for (float y = 6.0f; y <= 54.0f; y += 4.0f) {
        if (y > 28.0f && y < 32.0f) continue;    /* the open gap */
        pl_set_cursor(&p, 40.0f, y);
        if (pl_commit(&p, g, s)) segs++;
    }
    CHECK(segs > 0, "should have placed barricade pieces (got %d)", segs);

    /* the wall column (cell x=80, x≈40 m) must be solid except across the gap */
    int wallcells = 0;
    for (int cy = 8; cy <= 112; cy++)
        if (cy < 56 || cy > 64)                  /* skip the gap rows (y≈28..32) */
            if (simp_is_wall(s, 80, cy)) wallcells++;
    CHECK(wallcells > 0, "barricade should have raised walls at x-cell 80 (got %d)", wallcells);

    /* dense block west of the wall */
    int spawned = 0;
    for (float yy = 8.0f; yy <= 52.0f; yy += 0.62f)
        for (float xx = 5.0f; xx <= 25.0f; xx += 0.62f)
            if (simp_spawn(s, xx, yy) >= 0) spawned++;
    CHECK(spawned > 100, "should spawn a crowd (got %d)", spawned);

    int n0 = simp_count(s);
    for (int step = 0; step < 3000; step++) simp_step(s, DT);

    /* no agent center may sit inside a wall cell (collision respected) */
    const float *px = simp_px(s), *py = simp_py(s);
    int inside = 0, n = simp_count(s);
    for (int i = 0; i < n; i++) {
        int cx = (int)(px[i] / CELL), cy = (int)(py[i] / CELL);
        if (cx >= 0 && cx < GW && cy >= 0 && cy < GH && simp_is_wall(s, cx, cy)) inside++;
    }
    CHECK(inside == 0, "%d agents inside a wall cell (must be 0)", inside);
    CHECK(simp_count(s) < n0, "horde should drain through the gap (%d → %d)", n0, simp_count(s));
    CHECK(any_nan(s) == 0, "NaN in positions");

    def_destroy(g); simp_destroy(s);
}

/* ---- case 5: rotation transposes the footprint -------------------------- */
/* place a w=4,h=1 barricade and return the bbox (cells) of the raised walls */
static void place_and_bbox(int rot90, int *bw, int *bh) {
    SimP *s = simp_create(GW, GH, CELL, MAXA);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, MAXA);
    def_set_budget(g, 1000);
    Placement p; pl_init(&p, CAT, NCAT); p.sel = I_BARR; p.rot90 = rot90;
    pl_set_cursor(&p, 40.0f, 30.0f);             /* center of the 80×60 m world */
    CHECK(pl_commit(&p, g, s), "barricade should place (rot %d)", rot90);

    int minx = GW, maxx = -1, miny = GH, maxy = -1;
    for (int cy = 0; cy < GH; cy++)
        for (int cx = 0; cx < GW; cx++)
            if (simp_is_wall(s, cx, cy)) {
                if (cx < minx) minx = cx;
                if (cx > maxx) maxx = cx;
                if (cy < miny) miny = cy;
                if (cy > maxy) maxy = cy;
            }
    *bw = maxx - minx + 1; *bh = maxy - miny + 1;
    def_destroy(g); simp_destroy(s);
}

static void test_rotation(void) {
    printf("[5] rotation\n");
    int w0, h0, w1, h1;
    place_and_bbox(0, &w0, &h0);   /* 4 m wide × 1 m tall → 8×2 cells */
    place_and_bbox(1, &w1, &h1);   /* transposed → 2×8 cells */
    CHECK(w0 > h0, "rot0 footprint should be wider than tall (%d×%d)", w0, h0);
    CHECK(w1 < h1, "rot1 footprint should be taller than wide (%d×%d)", w1, h1);
    CHECK(w0 == h1 && h0 == w1, "rot1 should transpose rot0 (%d×%d vs %d×%d)", w0, h0, w1, h1);
}

/* ---- case 6: determinism ------------------------------------------------ */
/* a fixed placement script; checksum (drag positions + wall cells + budget) */
static unsigned long run_script(void) {
    SimP *s = simp_create(GW, GH, CELL, MAXA);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, MAXA);
    def_set_budget(g, 100000);
    Placement p; pl_init(&p, CAT, NCAT);

    struct { int kind, rot; float x, y; } script[] = {   /* world is 80×60 m */
        { I_BIN,  0, 20.0f, 20.0f }, { I_CAR,  1, 35.0f, 40.0f },
        { I_BARR, 0, 55.0f, 25.0f }, { I_BARR, 1, 65.0f, 45.0f },
        { I_TUR,  0, 70.0f, 20.0f }, { I_BIN,  0, 30.0f, 50.0f },
    };
    for (unsigned k = 0; k < sizeof(script)/sizeof(script[0]); k++) {
        p.sel = script[k].kind; p.rot90 = script[k].rot;
        pl_set_cursor(&p, script[k].x, script[k].y);
        if (pl_validate(&p, g, s)) pl_commit(&p, g, s);
    }
    for (int step = 0; step < 300; step++) simp_step(s, DT);

    unsigned long h = 1469598103934665603UL;     /* FNV-1a over the world state */
    #define MIX(v) do{ unsigned long _x=(unsigned long)(v); h^=_x; h*=1099511628211UL; }while(0)
    const float *dx = simp_drag_px(s), *dy = simp_drag_py(s);
    int nd = simp_drag_count(s); MIX(nd);
    for (int i = 0; i < nd; i++) { MIX((long)(dx[i]*1000.0f)); MIX((long)(dy[i]*1000.0f)); }
    for (int cy = 0; cy < GH; cy++)
        for (int cx = 0; cx < GW; cx++)
            if (simp_is_wall(s, cx, cy)) { MIX(cx); MIX(cy); }
    MIX(def_budget(g));
    #undef MIX

    int bad = any_nan(s);
    def_destroy(g); simp_destroy(s);
    return bad ? 0UL : h;                          /* 0 sentinel = NaN seen */
}

static void test_determinism(void) {
    printf("[6] determinism + no-NaN\n");
    unsigned long a = run_script(), b = run_script();
    CHECK(a != 0UL, "NaN encountered in scripted run");
    CHECK(a == b, "checksum mismatch %lu vs %lu", a, b);
}

/* ---- case 7: direct selection ------------------------------------------- */
static void test_select(void) {
    printf("[7] pl_select\n");
    Placement p; pl_init(&p, CAT, NCAT);
    pl_select(&p, I_CAR);
    CHECK(pl_selected(&p) == &CAT[I_CAR], "select should pick the car row");
    pl_select(&p, NCAT + 3);                     /* out of range: ignored */
    pl_select(&p, -1);
    CHECK(pl_selected(&p) == &CAT[I_CAR], "out-of-range select must be ignored");
}

/* ---- case 8: catalog combat params reach the turret ---------------------- */
static void test_turret_params(void) {
    printf("[8] data-driven turret\n");
    SimP *s = simp_create(GW, GH, CELL, MAXA);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, MAXA);
    def_set_budget(g, 1000);
    Placement p; pl_init(&p, CAT, NCAT);

    pl_select(&p, I_TUR);                        /* bare row: old defaults */
    pl_set_cursor(&p, 20.0f, 20.0f);
    CHECK(pl_commit(&p, g, s), "light turret should place");
    pl_select(&p, I_HEAVY);
    pl_set_cursor(&p, 40.0f, 20.0f);
    CHECK(pl_commit(&p, g, s), "heavy turret should place");

    const DefTurret *lt = def_turret(g, 0), *ht = def_turret(g, 1);
    CHECK(lt && !lt->heavy, "turret 0 should be light");
    CHECK(lt && fabsf(lt->range - 40.0f) < 1e-4f && fabsf(lt->fire_period - 0.12f) < 1e-4f
             && fabsf(lt->damage - 40.0f) < 1e-4f,
          "light defaults wrong (range %.1f period %.2f dmg %.1f)",
          lt ? (double)lt->range : 0, lt ? (double)lt->fire_period : 0, lt ? (double)lt->damage : 0);
    CHECK(ht && ht->heavy, "turret 1 should be heavy");
    CHECK(ht && fabsf(ht->range - 55.0f) < 1e-4f && fabsf(ht->fire_period - 0.5f) < 1e-4f
             && fabsf(ht->damage - 0.0f) < 1e-4f,
          "heavy params wrong (range %.1f period %.2f dmg %.1f)",
          ht ? (double)ht->range : 0, ht ? (double)ht->fire_period : 0, ht ? (double)ht->damage : 0);
    CHECK(def_budget(g) == 1000 - 100 - 250, "budget after both turrets (%d)", def_budget(g));

    def_destroy(g); simp_destroy(s);
}

/* ---- case 9: cancellata — see-through cover ------------------------------ */
static void test_fence(void) {
    printf("[9] cancellata (opacity)\n");
    SimP *s = simp_create(GW, GH, CELL, MAXA);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, MAXA);
    def_set_budget(g, 1000);
    Placement p; pl_init(&p, CAT, NCAT); p.rot90 = 1;   /* vertical, ray along x */

    /* fence at x=30 m, full barricade at x=50 m, same y */
    pl_select(&p, I_FENCE); pl_set_cursor(&p, 30.0f, 30.0f);
    CHECK(pl_commit(&p, g, s), "fence should place");
    pl_select(&p, I_BARR);  pl_set_cursor(&p, 50.0f, 30.0f);
    CHECK(pl_commit(&p, g, s), "full barricade should place");

    /* both are SOLID for the crowd */
    CHECK(simp_is_wall(s, 60, 60), "fence cell should be a wall (solid)");
    CHECK(simp_is_wall(s, 100, 60), "barricade cell should be a wall");

    /* rays across each, well clear of the other (origin+direction+maxdist) */
    float tf = simp_ray_transmit(s, 25.0f, 30.0f, 1.0f, 0.0f, 10.0f);
    float tb = simp_ray_transmit(s, 45.0f, 30.0f, 1.0f, 0.0f, 10.0f);
    CHECK(tf > 0.0f && tf < 1.0f, "fence transmit should be in (0,1), got %.3f", (double)tf);
    CHECK(tb < 0.001f, "full barricade transmit should be ~0, got %.3f", (double)tb);

    /* collapse the fence: cells free again AND transparent again */
    def_struct_damage(g, 0, 10000.0f);           /* fence was structure 0 */
    CHECK(def_struct_collapsed(g, 0), "fence should collapse under damage");
    simp_terrain_commit(s);
    CHECK(!simp_is_wall(s, 60, 60), "fence cell should be free after collapse");
    float tf2 = simp_ray_transmit(s, 25.0f, 30.0f, 1.0f, 0.0f, 10.0f);
    CHECK(tf2 > 0.999f, "transmit should be 1 after collapse, got %.3f", (double)tf2);

    def_destroy(g); simp_destroy(s);
}

/* ---- case 10: line fill --------------------------------------------------- */
static void test_line_fill(void) {
    printf("[10] line fill (modules + cost)\n");
    SimP *s = simp_create(GW, GH, CELL, MAXA);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, MAXA);
    def_set_budget(g, 1000);
    Placement p; pl_init(&p, CAT, NCAT); pl_select(&p, I_LINE);

    /* horizontal 11 m line: 4×2.5 + 1×1, cost 4·30 + 12 = 132 */
    PlLinePlan pl;
    CHECK(pl_line_validate(&p, g, s, 20.0f, 30.0f, 31.0f, 30.0f, &pl), "11 m line should validate");
    CHECK(pl.nmod == 5, "11 m should fill 5 modules (got %d)", pl.nmod);
    int n25 = 0, n10 = 0, n05 = 0;
    for (int m = 0; m < pl.nmod; m++) {
        if      (fabsf(pl.mlen[m] - 2.5f) < 1e-4f) n25++;
        else if (fabsf(pl.mlen[m] - 1.0f) < 1e-4f) n10++;
        else if (fabsf(pl.mlen[m] - 0.5f) < 1e-4f) n05++;
    }
    CHECK(n25 == 4 && n10 == 1 && n05 == 0, "expected 4×2.5+1×1 (got %d/%d/%d)", n25, n10, n05);
    CHECK(pl.cost == 132, "cost should be 132 (got %d)", pl.cost);

    int st0 = def_struct_count(g);
    CHECK(pl_line_commit(&p, g, s, 20.0f, 30.0f, 31.0f, 30.0f), "line should commit");
    CHECK(def_struct_count(g) - st0 == 5, "5 structures expected (got %d)", def_struct_count(g) - st0);
    CHECK(def_budget(g) == 1000 - 132, "budget should drop by 132 (got %d)", def_budget(g));

    /* walls raised along the line: 22 cells long × 2 thick at (40..61, 59..60) */
    int wal = 0;
    for (int cx = 40; cx < 62; cx++)
        for (int cy = 59; cy <= 60; cy++) if (simp_is_wall(s, cx, cy)) wal++;
    CHECK(wal == 44, "expected 44 wall cells (22×2), got %d", wal);

    /* snap: a 10.7 m drag behaves as 10.5 (4×2.5 + 0.5) */
    CHECK(pl_line_validate(&p, g, s, 20.0f, 45.0f, 30.7f, 45.0f, &pl), "10.7 m line should validate");
    CHECK(fabsf(pl.len - 10.5f) < 1e-4f, "10.7 should snap to 10.5 (got %.2f)", (double)pl.len);
    CHECK(pl.nmod == 5 && fabsf(pl.mlen[4] - 0.5f) < 1e-4f, "snapped fill should end with a 0.5 stub");

    /* degenerate: too short */
    CHECK(!pl_line_validate(&p, g, s, 20.0f, 50.0f, 20.1f, 50.0f, &pl), "0.1 m line must refuse");
    CHECK(p.reason == PL_BADITEM, "degenerate line reason should be PL_BADITEM (got %d)", p.reason);

    def_destroy(g); simp_destroy(s);
}

/* ---- case 11: line veto (all-or-nothing) ---------------------------------- */
static void test_line_veto(void) {
    printf("[11] line all-or-nothing\n");
    SimP *s = simp_create(GW, GH, CELL, MAXA);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, MAXA);
    def_set_budget(g, 1000);
    Placement p; pl_init(&p, CAT, NCAT); pl_select(&p, I_LINE);

    /* an agent parked mid-line vetoes the WHOLE line */
    simp_spawn(s, 25.0f, 30.0f);
    int b0 = def_budget(g), st0 = def_struct_count(g);
    CHECK(!pl_line_commit(&p, g, s, 20.0f, 30.0f, 31.0f, 30.0f), "line across agent must refuse");
    CHECK(p.reason == PL_OVERLAP, "reason should be PL_OVERLAP (got %d)", p.reason);
    CHECK(def_budget(g) == b0 && def_struct_count(g) == st0, "nothing placed, nothing spent");
    int wal = 0;
    for (int cx = 0; cx < GW; cx++)
        for (int cy = 0; cy < GH; cy++) if (simp_is_wall(s, cx, cy)) wal++;
    CHECK(wal == 0, "no wall cell may exist after refusal (got %d)", wal);

    /* budget gate on the TOTAL: 11 m costs 132, 100 in the pot = refuse */
    def_set_budget(g, 100);
    CHECK(!pl_line_commit(&p, g, s, 20.0f, 50.0f, 31.0f, 50.0f), "unaffordable line must refuse");
    CHECK(p.reason == PL_NOFUNDS, "reason should be PL_NOFUNDS (got %d)", p.reason);
    CHECK(def_budget(g) == 100, "budget untouched on refusal");

    def_destroy(g); simp_destroy(s);
}

/* ---- case 12: angled line -------------------------------------------------- */
static unsigned long line_cells_hash(void) {
    SimP *s = simp_create(GW, GH, CELL, MAXA);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, MAXA);
    def_set_budget(g, 100000);
    Placement p; pl_init(&p, CAT, NCAT); pl_select(&p, I_LINE);
    /* 45° diagonal, ~10.6 m raw → snaps to 10.5 */
    int ok = pl_line_commit(&p, g, s, 20.0f, 20.0f, 27.5f, 27.5f);
    unsigned long h = ok ? 1469598103934665603UL : 0UL;
    if (ok)
        for (int cy = 0; cy < GH; cy++)
            for (int cx = 0; cx < GW; cx++)
                if (simp_is_wall(s, cx, cy)) { h ^= (unsigned long)(cy * GW + cx); h *= 1099511628211UL; }
    def_destroy(g); simp_destroy(s);
    return h;
}

static void test_line_angle(void) {
    printf("[12] angled line\n");
    SimP *s = simp_create(GW, GH, CELL, MAXA);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, MAXA);
    def_set_budget(g, 100000);
    Placement p; pl_init(&p, CAT, NCAT); pl_select(&p, I_LINE);

    CHECK(pl_line_commit(&p, g, s, 20.0f, 20.0f, 27.5f, 27.5f), "45° line should commit");
    int wal = 0, offdiag = 0;
    for (int cx = 0; cx < GW; cx++)
        for (int cy = 0; cy < GH; cy++)
            if (simp_is_wall(s, cx, cy)) {
                wal++;
                if (abs(cx - cy) > 4) offdiag++;   /* staircase hugs y=x */
            }
    CHECK(wal > 20, "diagonal should raise a run of wall cells (got %d)", wal);
    CHECK(offdiag == 0, "all cells must hug the diagonal (%d stray)", offdiag);

    /* crowd pushed at the wall never centers inside it */
    for (float yy = 21.0f; yy <= 26.0f; yy += 0.65f)
        for (float xx = 15.0f; xx <= 19.0f; xx += 0.65f) simp_spawn(s, xx, yy);
    for (int cy = 0; cy < GH; cy++) simp_set_goal(s, GW - 2, cy, true);
    simp_terrain_commit(s);
    for (int step = 0; step < 600; step++) simp_step(s, DT);
    const float *px = simp_px(s), *py = simp_py(s);
    int inside = 0;
    for (int i = 0; i < simp_count(s); i++) {
        int cx = (int)(px[i] / CELL), cy = (int)(py[i] / CELL);
        if (cx >= 0 && cx < GW && cy >= 0 && cy < GH && simp_is_wall(s, cx, cy)) inside++;
    }
    CHECK(inside == 0, "%d agents centered inside the angled wall (must be 0)", inside);
    CHECK(any_nan(s) == 0, "NaN after angled-line run");
    def_destroy(g); simp_destroy(s);

    unsigned long a = line_cells_hash(), b = line_cells_hash();
    CHECK(a != 0UL && a == b, "angled line must be deterministic (%lu vs %lu)", a, b);
}

int main(void) {
    test_budget();
    test_space();
    test_static();
    test_barricade();
    test_rotation();
    test_determinism();
    test_select();
    test_turret_params();
    test_fence();
    test_line_fill();
    test_line_veto();
    test_line_angle();
    if (fails == 0) printf("test_place: ALL PASS\n");
    else            printf("test_place: %d FAIL\n", fails);
    return fails ? 1 : 0;
}

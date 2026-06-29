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
 */
#define _POSIX_C_SOURCE 199309L
#include "place.h"
#include "defense.h"
#include "sim_particles.h"
#include <stdio.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)
#define MAXA 4000

/* catalog (mirrors a minimal in-game palette) */
enum { I_BARR, I_TUR, I_BIN, I_CAR };
static const PlItem CAT[] = {
    /* kind        name          cost   w     h    radius  hp     mass */
    { PL_BARRICADE, "Barricata",   50,  4.0f, 1.0f, 0.0f, 300.0f,  0.0f },
    { PL_TURRET,    "Torretta",   100,  1.0f, 1.0f, 0.5f,   0.0f,  0.0f },
    { PL_BIN,       "Cassonetto",  20,  0.0f, 0.0f, 0.6f,   0.0f, 12.0f },
    { PL_CAR,       "Auto",        60,  3.0f, 0.0f, 0.6f,   0.0f, 20.0f },
};
#define NCAT ((int)(sizeof(CAT)/sizeof(CAT[0])))

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

int main(void) {
    test_budget();
    test_space();
    test_static();
    test_barricade();
    test_rotation();
    test_determinism();
    if (fails == 0) printf("test_place: ALL PASS\n");
    else            printf("test_place: %d FAIL\n", fails);
    return fails ? 1 : 0;
}

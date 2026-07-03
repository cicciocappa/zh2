/* test_cover.c — bullet-opacity axis C verification (ENTITY_DESIGN.md §4+§7).
 *
 *   A) simp_ray_transmit unit: opacity-free maps are bit-identical to the
 *      binary simp_wall_ray; known cells crossed straight / diagonally / only
 *      partially match the hand-computed (1-op)^(len/cell) product; a fence
 *      (solid, op 0.3) does NOT occlude simp_wall_ray or simp_query_ray, a
 *      full wall does.
 *   B) turret DPS: per-shot damage 10 in the open, 7 behind a fence (op 0.3,
 *      one cell -> transmit 0.7), zero behind a full wall — where the turret
 *      must NOT even engage (LoS regression 2026-07-03).
 *   C) acquisition: a turret prefers a farther clear target over a nearer one
 *      behind a full wall, but NOT over one behind a fence (transmit >= T_ACQ:
 *      nearest eligible wins).
 *   D) collapse: a sieged fence structure falls -> its cells' opacity resets
 *      to 0 (simp_set_wall false) -> transmit 1 -> full per-shot damage
 *      (reuses the test_turret_siege siege mechanic).
 *   E) determinism (scenario B-fence twice, identical hp/shots) + no NaN.
 */
#include "defense.h"
#include <math.h>
#include <stdio.h>

#define GW 80
#define GH 60
#define CELL 0.5f
#define DT (1.0f / 60.0f)
#define CAP 512

static int no_nan(const SimP *s) {
    const float *px = simp_px(s), *py = simp_py(s);
    for (int i = 0; i < simp_count(s); i++)
        if (isnan(px[i]) || isnan(py[i])) return 0;
    return 1;
}

/* dormant target dummy: soaks shots without moving or dying (tank, 600 hp) */
static int spawn_dummy(SimP *s, DefGame *g, float x, float y) {
    SimPHandle h = def_spawn(g, x, y, BT_TANK);
    int i = simp_index_of(s, h);
    simp_sleep(s, i);
    return simp_slot_of(s, i);
}

static DefTurret turret_at(float x, float y, float half_arc) {
    DefTurret t = {0};
    t.x = x; t.y = y; t.ang = 0.0f;
    t.arc_min = -half_arc; t.arc_max = half_arc;
    t.sweep_speed = 8.0f; t.range = 15.0f;
    t.fire_period = 0.2f; t.damage = 10.0f;
    return t;
}

/* ---- B/E: per-shot damage open / fence / wall --------------------------- */

static void run_dps(int fence_opacity_on, int wall, int *out_shots, int *out_lost) {
    SimP *s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);
    if (wall || fence_opacity_on) {
        for (int cy = 26; cy <= 34; cy++) {
            simp_set_wall(s, 30, cy, true);              /* x = 15.0..15.5 */
            if (fence_opacity_on) simp_set_opacity(s, 30, cy, 0.3f);
        }
    }
    simp_terrain_commit(s);
    int slot = spawn_dummy(s, g, 20.25f, 15.25f);        /* 10 m east of turret */
    int hp0 = def_hp(g)[slot];
    DefTurret t = turret_at(10.25f, 15.25f, 0.6f);
    def_add_turret(g, &t);
    for (int k = 0; k < 240; k++) { simp_step(s, DT); def_update(g, DT); }
    *out_shots = def_shots(g);
    *out_lost = hp0 - def_hp(g)[slot];
    if (!no_nan(s)) *out_lost = -1;
    def_destroy(g); simp_destroy(s);
}

int main(void) {
    int ok = 1;

    /* ---- A) simp_ray_transmit unit ----------------------------------- */

    /* A1: opacity-free map -> binary, identical to simp_wall_ray */
    SimP *s = simp_create(GW, GH, CELL, 64);
    uint32_t rs = 99991u;
    for (int k = 0; k < 200; k++) {                      /* random wall cells */
        rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
        simp_set_wall(s, (int)(rs % GW), (int)((rs >> 8) % GH), true);
    }
    simp_terrain_commit(s);
    int a1_bad = 0;
    for (int k = 0; k < 300; k++) {
        rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
        float ox = (float)(rs % 1000) * 0.04f, oy = (float)((rs >> 10) % 1000) * 0.03f;
        float ang = (float)(rs % 6283) * 0.001f;
        float dx = cosf(ang), dy = sinf(ang), maxd = 12.0f;
        float tr = simp_ray_transmit(s, ox, oy, dx, dy, maxd);
        float wr = simp_wall_ray(s, ox, oy, dx, dy, maxd);
        if (tr != (wr < maxd ? 0.0f : 1.0f)) a1_bad++;
        if (tr != 0.0f && tr != 1.0f) a1_bad++;          /* must stay binary */
    }
    printf("A1) opacity-free binary vs wall_ray: 300 rays, %d mismatches\n", a1_bad);
    ok = ok && !a1_bad;
    simp_destroy(s);

    /* A2: hand-computed transmittance on known geometry */
    s = simp_create(GW, GH, CELL, 64);
    simp_set_wall(s, 30, 30, true); simp_set_opacity(s, 30, 30, 0.3f); /* fence */
    simp_set_wall(s, 31, 30, true); simp_set_opacity(s, 31, 30, 0.3f); /* 2nd col */
    simp_set_wall(s, 30, 31, true); simp_set_opacity(s, 30, 31, 0.3f); /* 1-cell row */
    simp_set_opacity(s, 10, 10, 0.3f);                   /* open smoke cell */
    simp_set_wall(s, 40, 30, true);                      /* full wall (op 1) */
    simp_terrain_commit(s);
    float y30 = 30.5f * CELL;                            /* mid of cell row 30 */
    float y31 = 31.5f * CELL;                            /* mid of cell row 31 */
    /* one fence cell (30,31) crossed in full: 0.7^1 */
    float t1 = simp_ray_transmit(s, 14.0f, y31, 1.0f, 0.0f, 2.0f);
    /* two fence cells (30,30)+(31,30): 0.7^2 */
    float t2 = simp_ray_transmit(s, 14.0f, y30, 1.0f, 0.0f, 3.0f);
    /* stop halfway inside the fence cell: 0.7^0.5 (entry at t=1.0, +0.25 in) */
    float th = simp_ray_transmit(s, 14.0f, y31, 1.0f, 0.0f, 1.25f);
    /* 45-degree diagonal through the full smoke cell: 0.7^sqrt(2) */
    float td = simp_ray_transmit(s, 4.75f, 4.75f, 1.0f, 1.0f, 10.0f);
    /* full wall: 0 */
    float tw = simp_ray_transmit(s, 14.0f, y30, 1.0f, 0.0f, 15.0f);
    int a2_bad = fabsf(t1 - powf(0.7f, 1.0f))       > 1e-4f ||
                 fabsf(t2 - powf(0.7f, 2.0f))       > 1e-4f ||
                 fabsf(th - powf(0.7f, 0.5f))       > 1e-4f ||
                 fabsf(td - powf(0.7f, sqrtf(2.0f))) > 1e-4f ||
                 tw != 0.0f;
    printf("A2) transmit: 1-cell %.4f (%.4f) 2-cell %.4f (%.4f) half %.4f (%.4f) "
           "diag %.4f (%.4f) wall %.1f\n",
           t1, powf(0.7f, 1.0f), t2, powf(0.7f, 2.0f), th, powf(0.7f, 0.5f),
           td, powf(0.7f, sqrtf(2.0f)), tw);
    ok = ok && !a2_bad;

    /* A3: a fence does not occlude wall_ray/query_ray, a full wall does */
    float wr_fence = simp_wall_ray(s, 14.0f, y30, 1.0f, 0.0f, 2.0f);
    float y_wall = 30.5f * CELL;
    float wr_wall = simp_wall_ray(s, 18.0f, y_wall, 1.0f, 0.0f, 4.0f); /* hits (40,30) */
    int behind = simp_spawn(s, 17.0f, y30);              /* past the fence col */
    simp_step(s, DT);                                     /* build the grid */
    int qi[4]; float qt[4];
    int nq = simp_query_ray(s, 14.0f, y30, 1.0f, 0.0f, 5.0f, qi, qt, 4, 0);
    int a3_bad = !(wr_fence >= 2.0f) || !(wr_wall < 4.0f) ||
                 !(nq == 1 && qi[0] == behind);
    printf("A3) fence occlusion: wall_ray clear=%d, full-wall blocked=%d, "
           "query_ray hits through fence=%d\n",
           wr_fence >= 2.0f, wr_wall < 4.0f, nq == 1 && qi[0] == behind);
    ok = ok && !a3_bad;
    simp_destroy(s);

    /* ---- B) per-shot damage open / fence / wall ----------------------- */
    int sh_open, lost_open, sh_fence, lost_fence, sh_wall, lost_wall;
    run_dps(0, 0, &sh_open, &lost_open);
    run_dps(1, 1, &sh_fence, &lost_fence);
    run_dps(0, 1, &sh_wall, &lost_wall);
    int b_bad = !(sh_open > 0 && lost_open == sh_open * 10) ||
                !(sh_fence > 0 && lost_fence == sh_fence * 7) ||
                !(sh_wall == 0 && lost_wall == 0);
    printf("B) per-shot dmg: open %d/%d shots (10/shot ok=%d) | fence %d/%d "
           "(7/shot ok=%d) | wall shots=%d lost=%d (silent ok=%d)\n",
           lost_open, sh_open, sh_open > 0 && lost_open == sh_open * 10,
           lost_fence, sh_fence, sh_fence > 0 && lost_fence == sh_fence * 7,
           sh_wall, lost_wall, sh_wall == 0 && lost_wall == 0);
    ok = ok && !b_bad;

    /* ---- C) acquisition preference ------------------------------------ */
    /* near target at 4 m behind a short wall stub, far target at 8.5 m on a
     * clear bearing inside the arc. Full wall -> far engaged; fence -> near. */
    for (int fence = 0; fence <= 1; fence++) {
        s = simp_create(GW, GH, CELL, CAP);
        DefGame *g = def_create(s, CAP);
        for (int cy = 29; cy <= 31; cy++) {
            simp_set_wall(s, 24, cy, true);              /* x = 12.0..12.5 */
            if (fence) simp_set_opacity(s, 24, cy, 0.3f);
        }
        simp_terrain_commit(s);
        int slot_near = spawn_dummy(s, g, 14.25f, 15.25f);
        int slot_far  = spawn_dummy(s, g, 16.25f, 19.25f); /* clears the stub */
        int hp0_near = def_hp(g)[slot_near], hp0_far = def_hp(g)[slot_far];
        DefTurret t = turret_at(10.25f, 15.25f, 0.6f);
        def_add_turret(g, &t);
        for (int k = 0; k < 240; k++) { simp_step(s, DT); def_update(g, DT); }
        int lost_near = hp0_near - def_hp(g)[slot_near];
        int lost_far  = hp0_far - def_hp(g)[slot_far];
        int good = fence ? (lost_near > 0 && lost_far == 0)
                         : (lost_far > 0 && lost_near == 0);
        printf("C%d) %s: near lost %d, far lost %d -> %s\n", fence + 1,
               fence ? "fence (near wins)" : "full wall (far wins)",
               lost_near, lost_far, good ? "ok" : "BAD");
        ok = ok && good && no_nan(s);
        def_destroy(g); simp_destroy(s);
    }

    /* ---- D) fence collapse -> opacity 0 -> full damage ----------------- */
    s = simp_create(GW, GH, CELL, CAP);
    DefGame *g = def_create(s, CAP);
    /* full-height indestructible wall at cx=30, palazzo tier, except a fence
     * band rows 40..48 registered as a destructible structure (barricade) */
    for (int cy = 0; cy < GH; cy++) {
        if (cy >= 40 && cy <= 48) continue;
        simp_set_wall(s, 30, cy, true);
        simp_set_wall_cost(s, 30, cy, 10.0f * simp_wall_base_cost());
    }
    int sid = def_add_structure(g, 60.0f, 0);
    for (int cy = 40; cy <= 48; cy++) def_struct_cell(g, sid, 30, cy);
    simp_terrain_commit(s);
    for (int cy = 40; cy <= 48; cy++) simp_set_opacity(s, 30, cy, 0.3f);
    for (int cx = GW - 2; cx < GW; cx++)                 /* goal beyond, 2 deep */
        for (int cy = 0; cy < GH; cy++) simp_set_goal(s, cx, cy, true);
    simp_terrain_commit(s);
    float yc = 44.5f * CELL;                             /* fence centre row */
    float tr_before = simp_ray_transmit(s, 12.25f, yc, 1.0f, 0.0f, 6.0f);
    /* dense hex-packed horde (test_base-style): the wall siege needs a REAL
     * pushing crowd — a sparse pack tops out under ATTACK_MIN_P (measured) */
    { const float SPC = 0.62f;
      for (int row = 0; ; row++) {
          float yy = 17.0f + row * SPC * 0.866f;
          if (yy > 27.5f) break;
          float xoff = (row & 1) ? SPC * 0.5f : 0.0f;
          for (float x = 3.0f + xoff; x <= 14.5f; x += SPC)
              def_spawn(g, x, yy, BT_MAN);
      } }
    int col_step = -1, steps = 0;
    while (steps < 3600) {                               /* siege: <= 60 s */
        simp_step(s, DT); def_update(g, DT); steps++;
        if (def_struct_collapsed(g, sid)) { col_step = steps; break; }
    }
    int op_clear = 1;
    for (int cy = 40; cy <= 48; cy++)
        if (simp_opacity(s, 30, cy) != 0.0f) op_clear = 0;
    float tr_after = simp_ray_transmit(s, 12.25f, yc, 1.0f, 0.0f, 6.0f);
    while (steps < 7200 && simp_count(s) > 0) {          /* drain the crowd */
        simp_step(s, DT); def_update(g, DT); steps++;
    }
    /* post-collapse DPS across the fallen fence line */
    int slot_d = spawn_dummy(s, g, 18.25f, yc);
    int hp0_d = def_hp(g)[slot_d];
    DefTurret td2 = turret_at(12.25f, yc, 0.3f);
    def_add_turret(g, &td2);
    int shots0 = def_shots(g);
    for (int k = 0; k < 240; k++) { simp_step(s, DT); def_update(g, DT); }
    int sh_d = def_shots(g) - shots0;
    int lost_d = hp0_d - def_hp(g)[slot_d];
    int d_bad = col_step < 0 || !op_clear ||
                fabsf(tr_before - 0.7f) > 1e-4f || tr_after != 1.0f ||
                !(sh_d > 0 && lost_d == sh_d * 10);
    printf("D) collapse at step %d | transmit %.3f -> %.3f | opacity cleared=%d "
           "| post-collapse %d/%d shots (10/shot ok=%d) | %s\n",
           col_step, tr_before, tr_after, op_clear, lost_d, sh_d,
           sh_d > 0 && lost_d == sh_d * 10, d_bad ? "BAD" : "ok");
    ok = ok && !d_bad && no_nan(s);
    def_destroy(g); simp_destroy(s);

    /* ---- E) determinism ------------------------------------------------ */
    int sh_e, lost_e;
    run_dps(1, 1, &sh_e, &lost_e);
    int e_bad = sh_e != sh_fence || lost_e != lost_fence;
    printf("E) determinism: fence rerun %d/%d vs %d/%d -> %s\n",
           lost_e, sh_e, lost_fence, sh_fence, e_bad ? "BAD" : "ok");
    ok = ok && !e_bad && lost_open >= 0 && lost_fence >= 0;   /* no_nan flags */

    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

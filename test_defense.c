/* test_defense.c — M5 game layer (defense.c): turrets, damage, wounds, death.
 *
 *   1) Light turret mows a cluster: kills accumulate, every wound type
 *      (bloody / maimed-arm / crawling) shows up, no NaN.
 *   2) Heavy turret: instant kill leaves NO corpse; a tank survives several
 *      heavy hits then gibs.
 *   3) Determinism: two identical runs give the same kill count.
 */
#include "defense.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)

/* a compact cluster of `body` enemies near (cx,cy), spacing 0.7 m (no overlap) */
static void spawn_cluster(DefGame *g, float cx, float cy, int cols, int rows,
                          DefBody body) {
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            def_spawn(g, cx + (float)c * 0.7f, cy + (float)r * 0.7f, body);
}

static DefTurret mk_turret(float x, float y, int heavy, float dmg,
                           float period, int pierce) {
    DefTurret t = {0};
    t.x = x; t.y = y; t.ang = 0.0f;
    t.arc_min = -0.7f; t.arc_max = 0.7f; t.sweep_dir = 1;
    t.sweep_speed = 3.0f; t.range = 30.0f;
    t.fire_period = period; t.damage = dmg;
    t.heavy = heavy; t.piercing = pierce;
    return t;
}

static int any_nan(SimP *s) {
    const float *px = simp_px(s), *py = simp_py(s);
    for (int i = 0; i < simp_count(s); i++)
        if (!isfinite(px[i]) || !isfinite(py[i])) return 1;
    return 0;
}

int main(void) {
    float ww = GW * CELL, wh = GH * CELL; (void)ww; (void)wh;

    /* ---- 1) light turret mows a cluster ---- */
    SimP *s = simp_create(GW, GH, CELL, 2000);
    simp_terrain_commit(s);
    DefGame *g = def_create(s, 2000);
    spawn_cluster(g, 18.0f, 28.0f, 6, 5, BT_MAN);   /* 30 men, hp 100 each */
    int spawned = simp_count(s);
    DefTurret lt = mk_turret(5.0f, 29.4f, 0, 60.0f, 0.08f, 0);  /* 2 hits/man */
    def_add_turret(g, &lt);

    int saw_bloody = 0, saw_arm = 0, saw_crawl = 0, nan1 = 0;
    for (int k = 0; k < 3000; k++) {
        simp_step(s, DT);
        def_update(g, DT);
        if (def_count_wound(g, DW_BLOODY))      saw_bloody = 1;
        if (def_count_wound(g, DW_MAIMED_ARM))  saw_arm = 1;
        if (def_count_wound(g, DW_CRAWLING))    saw_crawl = 1;
        if (any_nan(s)) { nan1 = 1; break; }
    }
    int kills1 = def_kills(g);
    int bad1 = (kills1 < 20) || !saw_bloody || !saw_arm || !saw_crawl || nan1;
    printf("1) light: spawned %d, kills %d, wounds seen b=%d arm=%d crawl=%d, nan=%d -> ok=%d\n",
           spawned, kills1, saw_bloody, saw_arm, saw_crawl, nan1, !bad1);
    def_destroy(g); simp_destroy(s);

    /* ---- 2a) heavy turret leaves no corpse ---- */
    s = simp_create(GW, GH, CELL, 64);
    simp_terrain_commit(s);
    g = def_create(s, 64);
    spawn_cluster(g, 19.0f, 29.0f, 4, 2, BT_MAN);   /* 8 men */
    int men = simp_count(s);
    DefTurret ht = mk_turret(5.0f, 29.4f, 1, 0.0f, 0.15f, 0);   /* heavy */
    def_add_turret(g, &ht);
    int max_corpse = 0;
    for (int k = 0; k < 2000 && simp_count(s) > 0; k++) {
        simp_step(s, DT);
        def_update(g, DT);
        if (simp_corpse_count(s) > max_corpse) max_corpse = simp_corpse_count(s);
    }
    int bad2a = (def_kills(g) != men) || (max_corpse != 0);
    printf("2a) heavy: killed %d/%d, max corpses %d (want 0) -> ok=%d\n",
           def_kills(g), men, max_corpse, !bad2a);
    def_destroy(g); simp_destroy(s);

    /* ---- 2b) tank survives several heavy hits, then gibs ---- */
    s = simp_create(GW, GH, CELL, 64);
    simp_terrain_commit(s);
    g = def_create(s, 64);
    def_spawn(g, 20.0f, 29.4f, BT_TANK);
    DefTurret ht2 = mk_turret(5.0f, 29.4f, 1, 0.0f, 0.15f, 0);
    def_add_turret(g, &ht2);
    int shots_at_death = -1;
    for (int k = 0; k < 2000; k++) {
        simp_step(s, DT);
        def_update(g, DT);
        if (simp_count(s) == 0) { shots_at_death = def_shots(g); break; }
    }
    /* tank heavy_hits = 4: at least 4 shots must land before it dies */
    int bad2b = (shots_at_death < 4) || (def_kills(g) != 1) ||
                (simp_corpse_count(s) != 0);
    printf("2b) tank: died after %d shots (need >=4), kills %d, corpses %d -> ok=%d\n",
           shots_at_death, def_kills(g), simp_corpse_count(s), !bad2b);
    def_destroy(g); simp_destroy(s);

    /* ---- 2c) line of sight: a nearer target shielded by a wall is skipped
     *         in favour of a farther VISIBLE one (old code aimed at the near
     *         one and the blocked bullet killed nobody). ---- */
    s = simp_create(GW, GH, CELL, 64);
    /* short vertical wall at cx=16 (world x=8), y in [29.5,31.0) */
    for (int cy = 59; cy <= 61; cy++) simp_set_wall(s, 16, cy, true);
    simp_terrain_commit(s);
    g = def_create(s, 64);
    int sh = def_spawn(g, 11.0f, 30.0f, BT_MAN);   /* NEAR, behind the wall  */
    int vi = def_spawn(g, 12.0f, 34.0f, BT_MAN);   /* FAR, clear line of sight */
    (void)sh; (void)vi;
    DefTurret lt2 = mk_turret(5.0f, 30.0f, 0, 60.0f, 0.08f, 0);  /* 2 hits/man */
    def_add_turret(g, &lt2);
    for (int k = 0; k < 600 && simp_count(s) > 1; k++) {
        simp_step(s, DT); def_update(g, DT);
    }
    /* the visible one must die; the shielded one survives (turret holds fire) */
    int survivors = simp_count(s);
    float sx = survivors == 1 ? simp_px(s)[0] : -1.0f;
    int bad2c = (def_kills(g) != 1) || (survivors != 1) || (sx < 10.0f || sx > 12.0f);
    printf("2c) LoS: kills %d (want 1), survivors %d (want 1, shielded x=%.1f) -> ok=%d\n",
           def_kills(g), survivors, sx, !bad2c);
    def_destroy(g); simp_destroy(s);

    /* ---- 3) determinism: same scenario twice -> same kills ---- */
    int run_kills[2];
    for (int run = 0; run < 2; run++) {
        SimP *s2 = simp_create(GW, GH, CELL, 256);
        simp_terrain_commit(s2);
        DefGame *g2 = def_create(s2, 256);
        spawn_cluster(g2, 18.0f, 28.0f, 5, 4, BT_MAN);   /* 20 men */
        DefTurret t = mk_turret(5.0f, 29.0f, 0, 60.0f, 0.08f, 1);  /* piercing */
        def_add_turret(g2, &t);
        for (int k = 0; k < 1500; k++) { simp_step(s2, DT); def_update(g2, DT); }
        run_kills[run] = def_kills(g2);
        def_destroy(g2); simp_destroy(s2);
    }
    int bad3 = (run_kills[0] != run_kills[1]);
    printf("3) determinism: run A %d kills, run B %d kills -> ok=%d\n",
           run_kills[0], run_kills[1], !bad3);

    int ok = !bad1 && !bad2a && !bad2b && !bad2c && !bad3;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

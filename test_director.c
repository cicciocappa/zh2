/* test_director.c — §8 spawn director + budget (M5_DESIGN.md §8, defense.c).
 *
 * The director emits waves from border rects, BURST-FREE (simp_free_at gates
 * every spawn → it auto-throttles to the exit's capacity), with emission rate
 * and enemy toughness RAMPING per wave. Arena: a big open field with a generous
 * central drain so the population stays bounded and the ramp is readable at the
 * (clear) edges rather than masked by congestion.
 *
 * Asserted: the emission rate climbs across waves; the body mix toughens (late
 * tanks fraction > early); spawns never overlap (burst-free → bounded residual,
 * zero NaN); the budget deducts correctly; and the run is deterministic.
 */
#define _POSIX_C_SOURCE 199309L
#include "defense.h"
#include "sim_particles.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

#define GW 240
#define GH 240
#define CELL 0.5f
#define DT (1.0f / 60.0f)
#define MAXA 8000
#define WAVE_PERIOD 10.0f
#define RUN_STEPS 7200          /* 120 s → 12 waves */

typedef struct {
    DefDirector *dir;           /* set after create, for wave lookup in cb   */
    long body[BT_COUNT];        /* cumulative spawn histogram                */
    long early_tank, early_tot; /* waves 0..1                                */
    long late_tank, late_tot;   /* waves >= 6                                */
} Stats;

static void on_spawn(void *user, SimPHandle h, DefBody body, unsigned roll) {
    (void)h; (void)roll;
    Stats *st = (Stats *)user;
    st->body[body]++;
    int w = def_director_wave(st->dir);
    if (w <= 1)      { st->early_tot++; if (body == BT_TANK) st->early_tank++; }
    else if (w >= 6) { st->late_tot++;  if (body == BT_TANK) st->late_tank++;  }
}

typedef struct {
    int  emitted;
    long body[BT_COUNT];
    int  e_at20, e_at40, e_at100, e_at120;   /* emitted snapshots (seconds)   */
    float max_overlap;
    int  bad;
} Run;

static Run run_once(Stats *st_out) {
    Run r; memset(&r, 0, sizeof r);
    Stats st; memset(&st, 0, sizeof st);

    SimP *s = simp_create(GW, GH, CELL, MAXA);
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, 1); simp_set_wall(s, x, GH - 1, 1); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, 1); simp_set_wall(s, GW - 1, y, 1); }
    /* generous central drain: agents walk in and leave, population stays bounded */
    for (int cy = 100; cy < 140; cy++) for (int cx = 100; cx < 140; cx++) simp_set_goal(s, cx, cy, 1);
    simp_terrain_commit(s);

    DefGame *g = def_create(s, MAXA);

    /* thin spawn bands hugging the four edges (meters; world is 120×120) */
    DefRect rects[4] = {
        {  2.0f,   2.0f, 116.0f,   3.0f },
        {  2.0f, 115.0f, 116.0f,   3.0f },
        {  2.0f,   2.0f,   3.0f, 116.0f },
        {115.0f,   2.0f,   3.0f, 116.0f },
    };
    DefDirectorCfg cfg = {0};
    cfg.rects = rects; cfg.nrects = 4;
    cfg.spawn_radius = 0.34f;
    cfg.base_rate = 5.0f; cfg.rate_ramp = 4.0f; cfg.wave_period = WAVE_PERIOD;
    cfg.seed = 0xABCDE1u; cfg.on_spawn = on_spawn; cfg.user = &st;
    DefDirector *d = def_director_create(g, &cfg);
    st.dir = d;

    const float *px = simp_px(s), *py = simp_py(s), *rad = simp_radius_arr(s);

    for (int step = 0; step < RUN_STEPS; step++) {
        simp_step(s, DT);
        def_director_update(d, DT);

        int n = simp_count(s);
        for (int i = 0; i < n; i++)
            if (!isfinite(px[i]) || !isfinite(py[i])) r.bad++;

        /* burst-free check just after emission: sample a worst-case overlap on
         * the freshly spawned edge agents would be O(N²); instead trust free_at
         * and spot-check the global worst residual cheaply every 600 steps. */
        if (step % 600 == 0) {
            for (int i = 0; i < n; i++) {
                if (px[i] > 8.0f && px[i] < 112.0f && py[i] > 8.0f && py[i] < 112.0f) continue;
                for (int j = i + 1; j < n; j++) {
                    float dx = px[i]-px[j], dy = py[i]-py[j];
                    float d2 = dx*dx+dy*dy, rr = rad[i]+rad[j];
                    if (d2 < rr*rr) { float ov = rr - sqrtf(d2); if (ov > r.max_overlap) r.max_overlap = ov; }
                }
            }
        }

        int em = def_director_emitted(d);
        float t = (step + 1) * DT;
        if (t >=  20.0f && !r.e_at20)  r.e_at20  = em;
        if (t >=  40.0f && !r.e_at40)  r.e_at40  = em;
        if (t >= 100.0f && !r.e_at100) r.e_at100 = em;
        if (t >= 120.0f && !r.e_at120) r.e_at120 = em;
    }
    r.emitted = def_director_emitted(d);
    for (int b = 0; b < BT_COUNT; b++) r.body[b] = st.body[b];
    if (st_out) *st_out = st;

    def_director_destroy(d); def_destroy(g); simp_destroy(s);
    return r;
}

int main(void) {
    Stats st;
    Run a = run_once(&st);
    Run b = run_once(NULL);   /* determinism */

    float rate_early = (a.e_at40  - a.e_at20)  / 20.0f;   /* waves 2..3 */
    float rate_late  = (a.e_at120 - a.e_at100) / 20.0f;   /* waves 10..11 */
    float early_tankf = st.early_tot ? (float)st.early_tank / st.early_tot : 0.0f;
    float late_tankf  = st.late_tot  ? (float)st.late_tank  / st.late_tot  : 0.0f;

    printf("emitted %d | rate early %.1f/s late %.1f/s | tank%% early %.1f late %.1f | "
           "max overlap %.3f m | nan %d\n",
           a.emitted, (double)rate_early, (double)rate_late,
           100.0*early_tankf, 100.0*late_tankf, (double)a.max_overlap, a.bad);
    printf("mix: obese %ld man %ld woman %ld child %ld tank %ld\n",
           a.body[BT_OBESE], a.body[BT_MAN], a.body[BT_WOMAN], a.body[BT_CHILD], a.body[BT_TANK]);

    /* budget: deduct correctly, refuse when too poor */
    SimP *bs = simp_create(40, 40, CELL, 16);
    DefGame *bg = def_create(bs, 16);
    def_set_budget(bg, 100);
    int ok_budget = def_spend(bg, 40) && def_spend(bg, 40) && !def_spend(bg, 40)
                    && def_budget(bg) == 20 && !def_spend(bg, -5);
    def_destroy(bg); simp_destroy(bs);

    /* 1) emission rate climbs with the wave ramp */
    int ok_ramp = rate_late > rate_early * 1.5f && rate_early > 0.0f;
    /* 2) the mix toughens: more tanks in late waves than early */
    int ok_mix = late_tankf > early_tankf && st.late_tot > 0 && st.early_tot > 0;
    /* 3) burst-free: residual overlap stays in the PBD guardrail band, no NaN */
    int ok_burst = a.max_overlap < 0.20f && a.bad == 0;
    /* 4) determinism */
    int ok_det = b.emitted == a.emitted;
    for (int t = 0; t < BT_COUNT; t++) if (b.body[t] != a.body[t]) ok_det = 0;

    int ok = ok_ramp && ok_mix && ok_burst && ok_budget && ok_det;
    printf("ramp=%d mix=%d burst=%d budget=%d det=%d\n",
           ok_ramp, ok_mix, ok_burst, ok_budget, ok_det);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

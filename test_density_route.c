/* test_density_route.c — M3.6 verification: density -> Dijkstra cost
 * (Continuum Crowds light).
 *
 * Scene: two routes to a small goal. The SHORT route goes through a narrow
 * gap (2.5 m) straight ahead; the LONG route detours through a wide gap
 * (12 m) further down the wall. With k_density = 0 the whole horde queues
 * at the short gap (single optimal path). With k_density > 0 the queue
 * makes the short route expensive, so a substantial fraction diverts to the
 * long one, and the total drain gets FASTER. Both are asserted:
 *
 *   - wide-gap usage fraction rises by at least 0.15
 *   - steps to drain 90% strictly decrease
 *
 * Gap crossings are tracked per SLOT (stable for the agent's lifetime):
 * an agent whose x crosses the wall line is classified by the y it crossed at.
 */
#include "sim_particles.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)
#define MAX_AGENTS 1200
#define MAX_STEPS 9000              /* 150 s cap */
#define WALL_X 40.0f                /* wall line (m), cell 80 */

typedef struct {
    int spawned, drained, steps;
    long cross_narrow, cross_wide;
    int bad;
} RunStats;

static RunStats run(float k_density) {
    RunStats st = { 0, 0, 0, 0, 0, 0 };
    SimP *s = simp_create(GW, GH, CELL, MAX_AGENTS);
    simp_params(s)->k_density = k_density;

    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
    /* wall at x = 40 m: narrow gap 29..31.5 m (2.5 m, straight ahead),
     * wide gap 8..20 m (12 m, detour) */
    for (int y = 1; y < GH - 1; y++) {
        bool narrow = (y >= 58 && y < 63);
        bool wide   = (y >= 16 && y < 40);
        if (!narrow && !wide) simp_set_wall(s, 80, y, true);
    }
    /* small goal block on the right, centered on the narrow gap's height */
    for (int y = 54; y < 66; y++) simp_set_goal(s, 150, y, true);
    simp_terrain_commit(s);

    /* hex-staggered spawn block left of the wall, centered on y = 30 m */
    static float last_x[MAX_AGENTS];
    const float SPACING = 0.74f;
    for (int row = 0; ; row++) {
        float y = 22.0f + row * SPACING * 0.866f;
        if (y > 38.0f) break;
        float xoff = (row & 1) ? SPACING * 0.5f : 0.0f;
        for (int col = 0; ; col++) {
            float x = 4.0f + xoff + col * SPACING;
            if (x > 28.0f) break;
            int i = simp_spawn(s, x, y);
            if (i >= 0) { last_x[simp_slot_of(s, i)] = x; st.spawned++; }
        }
    }

    const float *px = simp_px(s), *py = simp_py(s);
    const int target = (int)(0.9f * (float)st.spawned);
    int step = 0;
    for (; step < MAX_STEPS; step++) {
        st.drained += simp_step(s, DT);
        for (int i = 0; i < simp_count(s); i++) {
            int slot = simp_slot_of(s, i);
            if (last_x[slot] < WALL_X && px[i] >= WALL_X) {
                if (py[i] < 24.0f) st.cross_wide++; else st.cross_narrow++;
            }
            last_x[slot] = px[i];
            if (!isfinite(px[i]) || !isfinite(py[i])) st.bad++;
        }
        if (st.drained >= target) break;
    }
    st.steps = step;
    simp_destroy(s);
    return st;
}

int main(void) {
    RunStats a = run(0.0f);
    RunStats b = run(2.5f);

    double fa = (double)a.cross_wide / (double)(a.cross_wide + a.cross_narrow + 1);
    double fb = (double)b.cross_wide / (double)(b.cross_wide + b.cross_narrow + 1);
    printf("k_density 0.0: spawned %d | wide %ld narrow %ld (wide %.0f%%) | "
           "90%% drained in %d steps (drained %d)\n",
           a.spawned, a.cross_wide, a.cross_narrow, 100.0 * fa, a.steps, a.drained);
    printf("k_density 2.5: spawned %d | wide %ld narrow %ld (wide %.0f%%) | "
           "90%% drained in %d steps (drained %d)\n",
           b.spawned, b.cross_wide, b.cross_narrow, 100.0 * fb, b.steps, b.drained);

    int ok_split = fb > fa + 0.15;
    /* faster drain: fewer steps to 90%; if the baseline hit the cap, more
     * drained at the cap also counts */
    int ok_speed = (b.steps < a.steps) ||
                   (a.steps == MAX_STEPS && b.drained > a.drained);
    int ok = (a.bad == 0) && (b.bad == 0) && ok_split && ok_speed;
    printf("split ok=%d speed ok=%d nan ok=%d\n", ok_split, ok_speed,
           a.bad == 0 && b.bad == 0);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

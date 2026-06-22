/* test_jam.c — M3.7 verification: stalled crowd -> Dijkstra cost.
 *
 * Density->cost (M3.6) measures how CROWDED a cell is, not whether anyone is
 * actually moving: its penalty saturates at k_density, so a chokepoint with
 * zero throughput (real cost in time: infinite) never gets expensive enough
 * to beat a long detour, and the horde queues at it forever. The jam term
 * weights each agent by its stillness, so a stopped queue — and only a
 * stopped queue — blows the cost up and the flow reroutes.
 *
 * Scene: a wall with two openings. The DIRECT one, straight between spawn
 * block and goal, is a single nav-open cell sealed by an infinite-mass corpse
 * (corpses are never marked in the nav grid: the Dijkstra loves the route,
 * physics forbids it — the idealized "clogged chokepoint"). The DETOUR is a
 * wide gap 14 m further down. Asserted:
 *
 *   - k_jam = 0 (density only, default k_density): the horde piles up at the
 *     sealed slit and (almost) nobody drains;
 *   - k_jam = default: the queue prices itself out, the horde reroutes
 *     through the detour and most of it drains.
 */
#include "sim_particles.h"
#include <stdio.h>
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
    long cross_direct, cross_detour;
    int bad;
} RunStats;

static RunStats run(float kj) {
    RunStats st = { 0, 0, 0, 0, 0, 0 };
    SimP *s = simp_create(GW, GH, CELL, MAX_AGENTS);
    simp_params(s)->k_jam = kj;
    simp_params(s)->k_corpse = 0.0f;   /* isolate the jam term: the slit corpse
                                          would otherwise reroute via §7-bis */
    simp_params(s)->corpse_weight = 0.0f; /* idealized hard plug (§3 off): the
                                          slit corpse must stay an immovable seal */

    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
    /* wall at x = 40 m: direct slit = one cell at y = 30 m (cell 60),
     * detour gap 5..9 m (cells 10..18). The detour adds ~46 m of path:
     * beyond what the saturating density term alone can justify (it tops
     * out at k_density x the jam blob's width), within reach of the jam
     * term. A closer gap (~14 m offset) reroutes on density alone. */
    for (int y = 1; y < GH - 1; y++) {
        bool slit   = (y == 60);
        bool detour = (y >= 10 && y < 18);
        if (!slit && !detour) simp_set_wall(s, 80, y, true);
    }
    /* goal block on the right, centered on the slit's height */
    for (int y = 54; y < 66; y++) simp_set_goal(s, 150, y, true);
    simp_terrain_commit(s);

    /* plug the slit: a permanent infinite-mass corpse wider than the gap.
     * Nav-open, physically impassable. */
    simp_corpse_add(s, 40.25f, 30.25f, 0.5f, 1e9f);

    /* hex-staggered spawn block left of the wall, centered on the slit.
     * Sized so the queue saturates the jam term (a couple of agents per
     * cell suffice) while its DENSITY blob stays too small to ever justify
     * the detour on k_density alone: the baseline must stay stuck. */
    static float last_x[MAX_AGENTS];
    const float SPACING = 0.74f;
    for (int row = 0; ; row++) {
        float y = 25.0f + row * SPACING * 0.866f;
        if (y > 35.0f) break;
        float xoff = (row & 1) ? SPACING * 0.5f : 0.0f;
        for (int col = 0; ; col++) {
            float x = 8.0f + xoff + col * SPACING;
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
                if (py[i] < 15.0f) st.cross_detour++; else st.cross_direct++;
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
    RunStats a = run(0.0f);   /* density only: must stay (mostly) stuck */
    RunStats b = run(8.0f);   /* the default gain: must reroute and drain */

    printf("k_jam off: spawned %d | drained %d (%.0f%%) in %d steps | "
           "crossings detour %ld direct %ld\n",
           a.spawned, a.drained, 100.0 * a.drained / a.spawned, a.steps,
           a.cross_detour, a.cross_direct);
    printf("k_jam on : spawned %d | drained %d (%.0f%%) in %d steps | "
           "crossings detour %ld direct %ld\n",
           b.spawned, b.drained, 100.0 * b.drained / b.spawned, b.steps,
           b.cross_detour, b.cross_direct);

    /* density alone is allowed to leak a little (the crowd's southern
     * fringe legitimately prefers the detour once the blob saturates),
     * but it must not come close to draining; the jam run must actually
     * hit the 90% target before the cap. */
    int ok_stuck   = a.drained < (int)(0.50f * (float)a.spawned);
    int ok_reroute = (b.steps < MAX_STEPS) && b.drained >= (int)(0.90f * (float)b.spawned);
    int ok = (a.bad == 0) && (b.bad == 0) && ok_stuck && ok_reroute;
    printf("stuck ok=%d reroute ok=%d nan ok=%d\n", ok_stuck, ok_reroute,
           a.bad == 0 && b.bad == 0);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

/* test_siege.c — siege sensor verification (SIEGE_DESIGN.md).
 *
 * The core exposes, per grounded agent, the wall contact of the last step:
 *   simp_wall_pressure[i] = push * max(into_wall, 0)  (0 if not besieging)
 *   simp_wall_cell[i]     = the nav cell being pressed (-1 if none)
 * "Besieging" = in contact AND steering INTO the wall to reach the goal beyond.
 * A purely tangential brush (the hazard mechanic, not siege) stays at 0.
 *
 * Two halves:
 *
 *  A) SELECTIVITY (deterministic micro-test). Agents placed in identical contact
 *     with a wall, once with the goal BEYOND it (flow into the wall => siege) and
 *     once with the goal alongside (flow tangent => grazing). Same penetration,
 *     opposite flow: the sensor must light up in the first case and stay ~0 in the
 *     second. This isolates the into_wall filter from crowd geometry (a confined
 *     crowd always manufactures head-on contact somewhere, so an emergent "pure
 *     grazing" scene is unreliable as a control).
 *
 *  B) THE MECHANIC (crowd test). A wall fully seals the goal; the horde presses a
 *     destructible band. The GAME-SIDE discrete-attack model the design prescribes
 *     runs here: a per-slot attack timer fires a fixed-damage hit while the agent
 *     presses; at HP 0 the band's cells are freed and the nav recommits, so the
 *     horde drains through the breach on its own. With the band INDESTRUCTIBLE the
 *     horde sieges forever (drain stays 0) — the M3 baseline, unchanged.
 *
 * Asserted: selectivity (tangent ~0 << head-on), cell attribution (every pressed
 * cell is solid), the collapse->reroute->drain chain, the indestructible control,
 * determinism, and zero NaN.
 */
#include "sim_particles.h"
#include <stdio.h>
#include <math.h>
#include <stdbool.h>

#define GW 160
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)
#define MAX_AGENTS 1600
#define MAX_STEPS 9000

/* discrete-attack model (game-side, per SIEGE_DESIGN.md §3) */
#define ATTACK_PERIOD 0.8f      /* s between hits while pressing       */
#define ATTACK_DAMAGE 5.0f      /* HP per hit                          */
#define STRUCT_HP     2000.0f   /* destructible band starting HP       */
#define ATTACK_MIN_P  0.006f    /* min wall_pressure to count as a real attack:
                                 * a tangential brush leaks a near-zero pressure
                                 * (into_wall ~ 0), and without a floor even that
                                 * would slowly chip a wall. Gate it out. (M4: the
                                 * colored-tile PBD sweep nudged the tangent leak
                                 * 0.0037->0.0043, still 8x below the head-on
                                 * 0.0316; floor lifted from 0.004 for margin.) */

/* ============================ A) SELECTIVITY ============================== */
/* A horizontal wall at cy=WR with the agents just SOUTH of it (larger y),
 * discs overlapping its south face (identical contact in both cases). into=true
 * seals a goal to the NORTH, beyond the wall, so the flow points up INTO the wall
 * (the documented siege case); into=false puts the goal due east at the agents'
 * own row so the flow runs tangent along the wall (grazing). Returns the peak
 * wall_pressure seen on any agent over a short run. */
#define WR 20                   /* wall row (cy); south face at y = (WR+1)*CELL */

static float selectivity_run(bool into) {
    SimP *s = simp_create(GW, GH, CELL, MAX_AGENTS);
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }

    if (into) {
        /* full-width wall seals the north half; a walled-off goal above it pulls
         * the flow straight up into the wall from the agents penned to the south. */
        for (int x = 1; x < GW - 1; x++) simp_set_wall(s, x, WR, true);
        for (int y = 8; y < 12; y++) for (int x = 76; x < 84; x++) simp_set_goal(s, x, y, true);
    } else {
        /* wall segment; a TALL east goal pulls the flow east/away from the wall
         * (never north into it) => any wall contact is tangential. */
        for (int x = 10; x < 130; x++) simp_set_wall(s, x, WR, true);
        for (int y = WR + 1; y < WR + 40; y++) simp_set_goal(s, GW - 2, y, true);
    }
    simp_terrain_commit(s);

    /* a block of agents just south of the wall (clear of it at spawn). Sustained
     * contact needs a crowd: a lone agent placed in the wall is just ejected as a
     * velocity and never presses again. The block lets the flow drive it in. */
    float south_face = (WR + 1) * CELL;        /* 10.5 m */
    const float SP = 0.62f;
    for (int row = 0; ; row++) {
        float y = south_face + 0.9f + row * SP * 0.866f;   /* top row clear of the wall */
        if (y > south_face + 8.0f) break;
        float xoff = (row & 1) ? SP * 0.5f : 0.0f;
        for (float x = 18.0f + xoff; x <= 58.0f; x += SP)
            simp_spawn(s, x, y);
    }

    /* peak measured AFTER settling: a head-on flow keeps the front row pinned to
     * the wall (sustained), while the spawn transient that briefly grazes the
     * tangent wall has long since dissipated. */
    const float *wp = simp_wall_pressure(s);
    float peak = 0.0f;
    for (int step = 0; step < 240; step++) {
        simp_step(s, DT);
        if (step < 30) continue;
        for (int i = 0; i < simp_count(s); i++)
            if (wp[i] > peak) peak = wp[i];
    }
    simp_destroy(s);
    return peak;
}

/* ============================ B) THE MECHANIC ============================= */

typedef struct {
    bool  indestructible;       /* if true the band never collapses */
} Cfg;

typedef struct {
    int    spawned, drained_total, drained_after_collapse;
    int    collapse_step;       /* -1 = never collapsed                  */
    long   total_hits;
    int    max_attackers;       /* peak agents attacking the band        */
    int    cell_nonsolid_viol;  /* pressure>0 but wall_cell not solid (must be 0) */
    int    bad;                 /* NaN / non-finite                      */
    int    steps;
} Stats;

/* sealed wall at cx=80; the goal beyond is fully walled => siege. A band in
 * front of the spawn (cy 50..70) is the destructible structure. */
static void build_siege(SimP *s, uint8_t *destructible /*GW*GH*/) {
    for (int k = 0; k < GW * GH; k++) destructible[k] = 0;
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
    for (int y = 1; y < GH - 1; y++) simp_set_wall(s, 80, y, true);
    for (int y = 50; y < 70; y++) destructible[y * GW + 80] = 1;
    for (int y = 54; y < 66; y++) simp_set_goal(s, 150, y, true);
    simp_terrain_commit(s);
}

static int spawn_block(SimP *s) {     /* hex block left of the wall, on the band */
    const float SP = 0.62f;
    int n = 0;
    for (int row = 0; ; row++) {
        float y = 22.0f + row * SP * 0.866f;
        if (y > 38.0f) break;
        float xoff = (row & 1) ? SP * 0.5f : 0.0f;
        for (float x = 10.0f + xoff; x <= 38.0f; x += SP)
            if (simp_spawn(s, x, y) >= 0) n++;
    }
    return n;
}

static Stats run(Cfg c) {
    Stats st = { 0 };
    st.collapse_step = -1;
    SimP *s = simp_create(GW, GH, CELL, MAX_AGENTS);

    static uint8_t destructible[GW * GH];
    build_siege(s, destructible);
    st.spawned = spawn_block(s);

    static float attack_timer[MAX_AGENTS];   /* per SLOT */
    for (int k = 0; k < MAX_AGENTS; k++) attack_timer[k] = 0.0f;
    float hp = STRUCT_HP;
    bool collapsed = false;

    const float *px = simp_px(s), *py = simp_py(s);
    const float *wp = simp_wall_pressure(s);
    const int   *wc = simp_wall_cell(s);

    int step = 0;
    for (; step < MAX_STEPS; step++) {
        int drained = simp_step(s, DT);
        st.drained_total += drained;
        if (collapsed) st.drained_after_collapse += drained;

        int attackers = 0, n = simp_count(s);
        for (int i = 0; i < n; i++) {
            if (!isfinite(px[i]) || !isfinite(py[i])) st.bad++;
            float p = wp[i];
            if (p <= 0.0f) continue;
            int cell = wc[i];
            if (!simp_is_wall(s, cell % GW, cell / GW)) st.cell_nonsolid_viol++;

            /* discrete attack: real presses only (gate out tangential leak) */
            int slot = simp_slot_of(s, i);
            if (p >= ATTACK_MIN_P && cell >= 0 && destructible[cell] && !collapsed) {
                attackers++;
                attack_timer[slot] += DT;
                if (attack_timer[slot] >= ATTACK_PERIOD) {
                    attack_timer[slot] -= ATTACK_PERIOD;
                    hp -= ATTACK_DAMAGE;
                    st.total_hits++;
                }
            } else {
                attack_timer[slot] = 0.0f;   /* not really attacking: no free hit */
            }
        }
        if (attackers > st.max_attackers) st.max_attackers = attackers;

        /* collapse: free the band, recommit nav -> the horde reroutes itself */
        if (!collapsed && !c.indestructible && hp <= 0.0f) {
            for (int cy = 0; cy < GH; cy++)
                for (int cx = 0; cx < GW; cx++)
                    if (destructible[cy * GW + cx]) simp_set_wall(s, cx, cy, false);
            simp_terrain_commit(s);
            collapsed = true;
            st.collapse_step = step;
        }

        if (collapsed && st.drained_total >= (int)(0.70f * st.spawned)) break;
        if (c.indestructible && step >= 1800) break;   /* prove the baseline holds */
    }
    st.steps = step;
    simp_destroy(s);
    return st;
}

static void report(const char *tag, Stats st) {
    printf("%-20s spawned %d | drained %d (after collapse %d) | collapse@%d | "
           "hits %ld | peak attackers %d | cell-viol %d | nan %d | steps %d\n",
           tag, st.spawned, st.drained_total, st.drained_after_collapse,
           st.collapse_step, st.total_hits, st.max_attackers, st.cell_nonsolid_viol,
           st.bad, st.steps);
}

int main(void) {
    float tang = selectivity_run(false);   /* flow tangent: must stay ~0   */
    float into = selectivity_run(true);    /* flow into wall: must light up */
    printf("selectivity: tangent peak %.4f | into-wall peak %.4f\n", tang, into);

    Stats siege = run((Cfg){ false });
    Stats hold  = run((Cfg){ true  });      /* indestructible control */
    Stats again = run((Cfg){ false });      /* determinism */
    report("siege (breakable)", siege);
    report("siege (indestruct)", hold);

    /* 1) selectivity: head-on pressing registers; an identical tangential contact
     *    stays near zero (and well below the gameplay attack floor). */
    int ok_select = into > 0.01f && tang < 0.25f * into && tang < ATTACK_MIN_P;
    /* 2) cell attribution: every pressed cell is solid */
    int ok_cells = siege.cell_nonsolid_viol == 0 && hold.cell_nonsolid_viol == 0;
    /* 3) discrete attacks erode HP and collapse the band */
    int ok_attack = siege.max_attackers > 0 && siege.total_hits > 0
                    && siege.collapse_step >= 0;
    /* 4) collapse -> reroute -> drain through the breach */
    int ok_drain = siege.drained_after_collapse > (int)(0.5f * siege.spawned);
    /* 5) indestructible control: presses forever, nothing drains */
    int ok_hold = hold.collapse_step < 0 && hold.drained_total == 0
                  && hold.max_attackers > 0;
    /* 6) determinism */
    int ok_det = again.collapse_step == siege.collapse_step
                 && again.total_hits == siege.total_hits
                 && again.drained_total == siege.drained_total;
    /* 7) no NaN anywhere */
    int ok_nan = (siege.bad | hold.bad | again.bad) == 0;

    int ok = ok_select && ok_cells && ok_attack && ok_drain && ok_hold
             && ok_det && ok_nan;
    printf("select=%d cells=%d attack=%d drain=%d hold=%d det=%d nan=%d\n",
           ok_select, ok_cells, ok_attack, ok_drain, ok_hold, ok_det, ok_nan);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

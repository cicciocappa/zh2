/* test_handles.c — slot-map handle layer (M3.1).
 *
 * Brute-force shadow check: spawn agents at positions that encode their
 * identity, kill random subsets, respawn over the freed slots, and verify
 * that every live handle still resolves to the agent carrying its identity
 * and every dead handle resolves to -1 — including after slot reuse.
 * No stepping: positions are never touched, so comparisons are exact.
 */
#include "sim_particles.h"
#include <stdio.h>
#include <stdlib.h>

#define CAP 2000
#define N0  1000

static uint32_t rs = 99u;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

/* identity k -> unique position inside a 50x50 m world */
static float id_x(int k) { return 1.0f + (float)(k % 90) * 0.5f; }
static float id_y(int k) { return 1.0f + (float)(k / 90) * 0.5f; }

typedef struct { SimPHandle h; int alive; } Shadow;
static Shadow sh[N0 * 4];
static int n_sh = 0;

static int spawn_one(SimP *s) {
    int k = n_sh++;
    int i = simp_spawn(s, id_x(k), id_y(k));
    if (i < 0) { printf("spawn failed at k=%d\n", k); exit(1); }
    sh[k].h = simp_handle_of(s, i);
    sh[k].alive = 1;
    return k;
}

/* every shadow entry must agree with the core */
static int verify(SimP *s, const char *phase) {
    const float *px = simp_px(s), *py = simp_py(s);
    int bad = 0;
    for (int k = 0; k < n_sh; k++) {
        int i = simp_index_of(s, sh[k].h);
        if (!sh[k].alive) {
            if (i != -1) { bad++; }
            continue;
        }
        if (i < 0 || i >= simp_count(s) ||
            px[i] != id_x(k) || py[i] != id_y(k)) bad++;
        else if (simp_handle_of(s, i) != sh[k].h) bad++;
        else if (simp_slot_of(s, i) != (int)(sh[k].h & 0xFFFFFu) - 1) bad++;
    }
    printf("%-28s shadows=%d live=%d bad=%d\n", phase, n_sh, simp_count(s), bad);
    return bad;
}

static void kill_fraction(SimP *s, int permille) {
    for (int k = 0; k < n_sh; k++) {
        if (!sh[k].alive || rnd() % 1000 >= (uint32_t)permille) continue;
        int i = simp_index_of(s, sh[k].h);
        simp_kill(s, i);
        sh[k].alive = 0;
    }
}

int main(void) {
    SimP *s = simp_create(120, 120, 0.5f, CAP);
    simp_terrain_commit(s);
    int bad = 0;

    for (int k = 0; k < N0; k++) spawn_one(s);
    bad += verify(s, "spawn 1000");

    kill_fraction(s, 500);
    bad += verify(s, "kill ~50%");

    /* respawn over the freed slots, then churn twice more: reused slots get
     * fresh generations, so every old dead handle must keep resolving to -1 */
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 300; k++) spawn_one(s);
        bad += verify(s, "respawn 300 (slot reuse)");
        kill_fraction(s, 400);
        bad += verify(s, "kill ~40%");
    }

    /* drain everything and check the world is consistent when empty */
    for (int i = simp_count(s) - 1; i >= 0; i--) simp_kill(s, i);
    for (int k = 0; k < n_sh; k++) sh[k].alive = 0;
    bad += verify(s, "kill all");

    simp_destroy(s);
    printf(bad == 0 ? "PASS\n" : "FAIL\n");
    return bad == 0 ? 0 : 1;
}

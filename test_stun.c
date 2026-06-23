/* test_stun.c — hit stun (simp_stun): a stunned agent stays planted (steering
 * brakes to rest, self-momentum dropped) so it doesn't foot-slide under its hit
 * animation, then resumes on its own when the timer expires — vpref untouched.
 *
 *   A) plant: at cruise speed, a 1 s stun => near-zero self-advance over that
 *      second, vs a free control that keeps marching.
 *   B) resume: after the timer, the agent walks again at ~its old speed.
 *   C) SoA: the per-agent timer survives the periodic reorder permute (stun a
 *      whole column, force reorders, all stay planted) — guards array bookkeeping.
 *   D) determinism + no NaN.
 */
#define _POSIX_C_SOURCE 200112L   /* setenv under -std=c11 */
#include "sim_particles.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define DT (1.0f / 60.0f)

static SimP *corridor(int *idx) {
    SimP *s = simp_create(120, 20, 0.5f, 64);
    for (int cy = 0; cy < 20; cy++) simp_set_goal(s, 118, cy, true);
    simp_terrain_commit(s);
    if (idx) *idx = simp_spawn(s, 5.0f, 5.0f);
    return s;
}

static int any_nan(const SimP *s) {
    const float *px = simp_px(s), *py = simp_py(s);
    for (int i = 0; i < simp_count(s); i++)
        if (!isfinite(px[i]) || !isfinite(py[i])) return 1;
    return 0;
}

int main(void) {
    int ok = 1, nan = 0;

    /* --- A) plant: free vs stunned single agent --- */
    float adv[2], resume[2];
    for (int stun = 0; stun < 2; stun++) {
        int i; SimP *s = corridor(&i);
        for (int t = 0; t < 120; t++) simp_step(s, DT);      /* reach cruise */
        float x0 = simp_px(s)[i];
        if (stun) simp_stun(s, i, 1.0f);
        for (int t = 0; t < 60; t++) simp_step(s, DT);        /* the stunned second */
        adv[stun] = simp_px(s)[i] - x0;
        float x1 = simp_px(s)[i];
        for (int t = 0; t < 30; t++) simp_step(s, DT);        /* 0.5 s after */
        resume[stun] = simp_px(s)[i] - x1;
        nan += any_nan(s);
        simp_destroy(s);
    }
    int plant_ok = adv[1] < adv[0] * 0.1f && adv[0] > 0.5f;   /* stunned <10% of free */
    int resume_ok = resume[1] > resume[0] * 0.7f;             /* back to ~free speed */
    printf("A) free adv=%.3f stun adv=%.3f (plant %s) | resume free=%.3f stun=%.3f (%s)\n",
           (double)adv[0], (double)adv[1], plant_ok ? "ok" : "BAD",
           (double)resume[0], (double)resume[1], resume_ok ? "ok" : "BAD");
    ok = ok && plant_ok && resume_ok;

    /* --- C) SoA: stun a column, force reorders, all stay planted --- */
    setenv("SIMP_REORDER", "12", 1);                         /* permute often (read at create) */
    SimP *s = corridor(NULL);
    int col[40], nc = 0;
    for (int r = 0; r < 8; r++) for (int c = 0; c < 5; c++) {
        int k = simp_spawn(s, 8.0f + c * 0.8f, 3.0f + r * 0.5f);
        if (k >= 0) col[nc++] = k;
    }
    for (int t = 0; t < 60; t++) simp_step(s, DT);            /* cruise */
    /* stun by HANDLE (indices churn across reorders) */
    SimPHandle hcol[40]; float sx[40];
    for (int j = 0; j < nc; j++) {
        int i = col[j]; hcol[j] = simp_handle_of(s, i);
        simp_stun(s, i, 1.0f); sx[j] = simp_px(s)[i];
    }
    for (int t = 0; t < 54; t++) simp_step(s, DT);            /* < 1 s: still stunned; reorders fire */
    float worst = 0.0f;
    for (int j = 0; j < nc; j++) {
        int i = simp_index_of(s, hcol[j]); if (i < 0) continue;
        float d = fabsf(simp_px(s)[i] - sx[j]); if (d > worst) worst = d;
    }
    nan += any_nan(s);
    int soa_ok = worst < 0.20f;                              /* none marched off (crowd jitter only) */
    printf("C) reorder-safe: worst planted drift=%.3f m | %s\n", (double)worst, soa_ok ? "ok" : "BAD");
    ok = ok && soa_ok;
    simp_destroy(s);

    /* --- D) determinism --- */
    long sig[2];
    for (int run = 0; run < 2; run++) {
        int i; SimP *t = corridor(&i);
        for (int k = 0; k < 30; k++) simp_step(t, DT);
        simp_stun(t, i, 0.5f);
        for (int k = 0; k < 60; k++) simp_step(t, DT);
        sig[run] = (long)(simp_px(t)[i] * 1e5f);
        simp_destroy(t);
    }
    int det_ok = sig[0] == sig[1];
    printf("D) determinism: %ld==%ld %s | nan=%d\n", sig[0], sig[1], det_ok ? "ok" : "BAD", nan);
    ok = ok && det_ok && nan == 0;

    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

/* bench_sim.c — headless CPU benchmark of the particle core (zero deps: just
 * sim_particles + scene + libm; no SDL, no GL, no assets, no display). Measures
 * the renderer-agnostic cost of simp_step — the architectural limiter and the
 * target of M4 (multithreading / SIMD). Portable across machines for cross-CPU
 * comparison and as an M4 regression baseline.
 *
 *   ./bench_sim [scene.scn] [N] [warmup] [measure]
 * defaults: scenes/stress.scn, 20000, 120, 300. Prefills N agents on a jittered
 * lattice (no PBD burst), then times simp_step over the measure window.
 */
#define _POSIX_C_SOURCE 199309L
#include "scene.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

/* same lattice prefill as vat_horde: pitch = sqrt(area/target), jittered,
 * admitted by simp_free_at. ~80% walker, 20% runner. */
static int prefill_lattice(SimP *s, const Scene *sc, int target) {
    float W = sc->world_w, H = sc->world_h;
    float pitch = sqrtf(W * H / (float)target); if (pitch < 0.62f) pitch = 0.62f;
    SimPAgentDesc runner = { 0.30f, 3.6f, 1.0f };
    unsigned rng = 99; int n = 0;
    /* BENCH_SCRAMBLE=1: spawn the lattice points in random order so the agent
     * index order is uncorrelated with space — the realistic state after a game's
     * worth of spawner/kill churn (swap-and-pop), where reorder_agents earns its
     * keep. Default keeps the in-order lattice (best-case locality). */
    int scramble = 0; { const char *e = getenv("BENCH_SCRAMBLE"); if (e) scramble = atoi(e); }
    int ncols = (int)(W / pitch) + 1, nrows = (int)(H / pitch) + 1;
    long npts = (long)ncols * nrows;
    if (scramble) {
        float *xs = malloc((size_t)npts * sizeof(float));
        float *ys = malloc((size_t)npts * sizeof(float));
        long m = 0;
        for (float y = pitch * 0.5f; y < H; y += pitch)
            for (float x = pitch * 0.5f; x < W; x += pitch) { xs[m] = x; ys[m] = y; m++; }
        for (long i = m - 1; i > 0; i--) {           /* Fisher-Yates */
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            long j = rng % (unsigned long)(i + 1);
            float tx = xs[i]; xs[i] = xs[j]; xs[j] = tx;
            float ty = ys[i]; ys[i] = ys[j]; ys[j] = ty;
        }
        for (long k = 0; k < m && n < target; k++) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            float jx = xs[k] + ((rng % 1000) / 1000.0f - 0.5f) * pitch * 0.4f;
            float jy = ys[k] + (((rng >> 10) % 1000) / 1000.0f - 0.5f) * pitch * 0.4f;
            if (simp_free_at(s, jx, jy, 0.34f)) {
                if ((rng >> 20) % 5 == 0) simp_spawn_desc(s, jx, jy, &runner); else simp_spawn(s, jx, jy);
                n++;
            }
        }
        free(xs); free(ys);
        return n;
    }
    for (float y = pitch * 0.5f; y < H && n < target; y += pitch)
        for (float x = pitch * 0.5f; x < W && n < target; x += pitch) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            float jx = x + ((rng % 1000) / 1000.0f - 0.5f) * pitch * 0.4f;
            float jy = y + (((rng >> 10) % 1000) / 1000.0f - 0.5f) * pitch * 0.4f;
            if (simp_free_at(s, jx, jy, 0.34f)) {
                if ((rng >> 20) % 5 == 0) simp_spawn_desc(s, jx, jy, &runner); else simp_spawn(s, jx, jy);
                n++;
            }
        }
    return n;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "scenes/stress.scn";
    int target  = argc > 2 ? atoi(argv[2]) : 20000;
    int warmup  = argc > 3 ? atoi(argv[3]) : 120;
    int measure = argc > 4 ? atoi(argv[4]) : 300;

    Scene sc;
    if (scene_load(path, &sc) != 0) { fprintf(stderr, "scene load fail: %s\n", path); return 1; }
    SimP *s = scene_instantiate(&sc, target + 1024);
    if (!s) { fprintf(stderr, "instantiate fail\n"); return 1; }
    int placed = prefill_lattice(s, &sc, target);

    float dt = 1.0f / 60.0f;
    for (int i = 0; i < warmup; i++) simp_step(s, dt);

    double total = 0, mn = 1e30, mx = 0;
    for (int i = 0; i < measure; i++) {
        double t0 = now_ms();
        simp_step(s, dt);
        double e = now_ms() - t0;
        total += e; if (e < mn) mn = e; if (e > mx) mx = e;
    }
    double avg = total / measure;
    /* position checksum: identical across thread counts proves the colored-tile
     * PBD is deterministic (sum is order-independent over the final positions) */
    const float *px = simp_px(s), *py = simp_py(s);
    double csum = 0; int cn = simp_count(s);
    for (int i = 0; i < cn; i++) csum += (double)px[i] * 1.0009 + (double)py[i] * 1.0003;
    printf("bench_sim %s: N=%d placed=%d thr=%d | core_sim avg %.3f ms (min %.3f, max %.3f) "
           "| %.3f ms/1000 | %.0f steps/s | live=%d checksum=%.6f\n",
           path, target, placed, simp_threads(s), avg, mn, mx,
           avg / placed * 1000.0, 1000.0 / avg, cn, csum);
    simp_destroy(s); scene_free(&sc);
    return 0;
}

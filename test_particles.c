/* test_particles.c — headless verification of sim_particles (no SDL).
 *
 * Scene: 80m x 60m world (160x120 nav cells of 0.5m). A vertical wall at
 * x=40m with two 3m gates. 30,000 agents spawn on the left, the goal is a
 * column on the right edge. We verify:
 *   - numerical sanity: no NaN/inf positions, all agents inside bounds
 *   - flow: agents actually reach the goal (drain count grows)
 *   - granularity: residual overlap stays small (PBD converges)
 *   - chokepoints: density piles at the gates (qualitative, via frames)
 *   - performance: ms per step at 30k agents
 * Dumps PPM frames into frames/ every DUMP_EVERY steps.
 */
#define _POSIX_C_SOURCE 199309L
#include "sim_particles.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define GW 320
#define GH 240
#define CELL 0.5f
#define N_AGENTS 16000
#define STEPS 9000
#define DT (1.0f / 60.0f)
#define DUMP_EVERY 600

static void dump_ppm(const SimP *s, int step) {
    const int W = 640, H = 480;
    const float sx = (float)W / (GW * CELL), sy = (float)H / (GH * CELL);
    unsigned char *img = (unsigned char *)calloc((size_t)W * H * 3, 1);
    /* terrain */
    for (int cy = 0; cy < GH; cy++) for (int cx = 0; cx < GW; cx++) {
        int x0 = (int)(cx * CELL * sx), x1 = (int)((cx + 1) * CELL * sx);
        int y0 = (int)(cy * CELL * sy), y1 = (int)((cy + 1) * CELL * sy);
        unsigned char r = 18, g = 18, b = 22;
        if (simp_is_wall(s, cx, cy)) { r = 90; g = 90; b = 100; }
        for (int y = y0; y < y1 && y < H; y++) for (int x = x0; x < x1 && x < W; x++) {
            unsigned char *p = img + 3 * ((size_t)y * W + x);
            p[0] = r; p[1] = g; p[2] = b;
        }
    }
    /* agents (speed-tinted) */
    const float *px = simp_px(s), *py = simp_py(s);
    const float *vx = simp_vx(s), *vy = simp_vy(s);
    for (int i = 0; i < simp_count(s); i++) {
        int x = (int)(px[i] * sx), y = (int)(py[i] * sy);
        if (x < 0 || y < 0 || x >= W || y >= H) continue;
        float sp = sqrtf(vx[i] * vx[i] + vy[i] * vy[i]);
        float t = sp / 1.6f; if (t > 1.0f) t = 1.0f;
        unsigned char *p = img + 3 * ((size_t)y * W + x);
        p[0] = (unsigned char)(200 + 55 * t);
        p[1] = (unsigned char)(60 + 160 * t);
        p[2] = 50;
    }
    char name[64];
    snprintf(name, sizeof name, "frames/p_%05d.ppm", step);
    FILE *f = fopen(name, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    fwrite(img, 1, (size_t)W * H * 3, f);
    fclose(f);
    free(img);
}

int main(void) {
    SimP *s = simp_create(GW, GH, CELL, N_AGENTS);
    if (!s) { fprintf(stderr, "create failed\n"); return 1; }

    /* border walls */
    for (int x = 0; x < GW; x++) { simp_set_wall(s, x, 0, true); simp_set_wall(s, x, GH - 1, true); }
    for (int y = 0; y < GH; y++) { simp_set_wall(s, 0, y, true); simp_set_wall(s, GW - 1, y, true); }
    /* vertical wall at x = 60m (cell 120), 2 cells thick, two 4m gates */
    for (int y = 1; y < GH - 1; y++) {
        bool gate = (y >= 56 && y < 64) || (y >= 176 && y < 184);
        if (!gate) { simp_set_wall(s, 120, y, true); simp_set_wall(s, 121, y, true); }
    }
    /* goal column at x = 90m (cell 180) */
    for (int y = 1; y < GH - 1; y++) simp_set_goal(s, 180, y, true);
    simp_terrain_commit(s);

    /* spawn on a hex-staggered lattice left of the wall: feasible density,
     * no deep initial overlap (spacing ~ max diameter) */
    srand(42);
    int spawned = 0;
    const float SPACING = 0.74f;
    for (int row = 0; spawned < N_AGENTS; row++) {
        float y = 2.0f + row * SPACING * 0.866f;
        if (y > 118.0f - 2.0f) break;
        float xoff = (row & 1) ? SPACING * 0.5f : 0.0f;
        for (int col = 0; spawned < N_AGENTS; col++) {
            float x = 2.0f + xoff + col * SPACING;
            if (x > 56.0f) break;
            float jx = ((float)rand() / RAND_MAX - 0.5f) * 0.06f;
            float jy = ((float)rand() / RAND_MAX - 0.5f) * 0.06f;
            if (simp_spawn(s, x + jx, y + jy) >= 0) spawned++;
        }
    }
    printf("spawned: %d\n", spawned);

    if (system("mkdir -p frames") != 0) return 1;
    long drained_total = 0;
    int nan_cells = 0;
    double sim_ms = 0.0;

    for (int step = 0; step < STEPS; step++) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        drained_total += simp_step(s, DT);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        sim_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

        if (step % DUMP_EVERY == 0) dump_ppm(s, step);

        if (step % 300 == 0 || step == STEPS - 1) {
            /* sanity sweep */
            const float *px = simp_px(s), *py = simp_py(s);
            int bad = 0;
            float maxsp = 0.0f;
            const float *vx = simp_vx(s), *vy = simp_vy(s);
            for (int i = 0; i < simp_count(s); i++) {
                if (!isfinite(px[i]) || !isfinite(py[i])) bad++;
                else if (px[i] < 0 || py[i] < 0 || px[i] > GW * CELL || py[i] > GH * CELL) bad++;
                float sp = sqrtf(vx[i]*vx[i] + vy[i]*vy[i]);
                if (sp > maxsp) maxsp = sp;
            }
            nan_cells += bad;
            printf("step %4d | alive %6d | drained %6ld | bad %d | "
                   "mean_overlap %.4f m | max_speed %.2f m/s | avg %.2f ms/step\n",
                   step, simp_count(s), drained_total, bad,
                   simp_mean_overlap(s), maxsp, sim_ms / (step + 1));
        }
    }
    dump_ppm(s, STEPS);

    printf("\n=== RESULT ===\n");
    printf("total drained: %ld / %d (%.1f%%)\n", drained_total, spawned,
           100.0 * (double)drained_total / spawned);
    printf("invalid positions ever seen: %d\n", nan_cells);
    printf("avg step time: %.2f ms\n", sim_ms / STEPS);
    int ok = (nan_cells == 0) && (drained_total > spawned / 2);
    printf(ok ? "PASS\n" : "FAIL\n");
    simp_destroy(s);
    return ok ? 0 : 1;
}

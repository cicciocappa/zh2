/* test_terrain.c — render-only heightmap (.zhm) sampling verification.
 *
 *   1. ROUNDTRIP: build a Terrain in code, save, load, compare every field
 *      and every Z sample bit-identical.
 *   2. PLANE: bilinear interpolation of a planar field is EXACT, so
 *      terrain_z(x,y) must equal the analytic plane at any interior point.
 *   3. STEP: a sidewalk step (flat / ramp / flat); check known samples, the
 *      midpoint average, and a point inside the ramp.
 *   4. CLAMP: samples outside the AABB return the edge value (no extrapolation).
 *
 * No Blender needed: the .zhm is produced in C (same format the bake writes),
 * so the sampling math is verified in isolation and headless (EDITOR_DESIGN §9).
 */
#include "terrain.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define TMP "test_terrain_tmp.zhm"

static int close_to(float a, float b, float eps) { return fabsf(a - b) <= eps; }

/* Build a Terrain whose Z is f(x,y), sampled on a regular grid. */
static void build(Terrain *t, int w, int h, float ox, float oy, float ppm,
                  float (*f)(float, float)) {
    t->w = w; t->h = h; t->origin_x = ox; t->origin_y = oy; t->px_per_m = ppm;
    t->z = malloc((size_t)w * h * sizeof(float));
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            float x = ox + i / ppm, y = oy + j / ppm;
            t->z[j * w + i] = f(x, y);
        }
}

static float plane(float x, float y) { return 0.1f * x + 0.05f * y - 1.0f; }

static int roundtrip(void) {
    Terrain a;
    build(&a, 81, 65, 0.0f, 0.0f, 4.0f, plane);

    int ok = terrain_save(TMP, &a) == 0;
    Terrain b;
    memset(&b, 0, sizeof b);
    ok = ok && terrain_load(TMP, &b) == 0;
    ok = ok && b.w == a.w && b.h == a.h;
    ok = ok && close_to(b.origin_x, a.origin_x, 0) && close_to(b.origin_y, a.origin_y, 0);
    ok = ok && close_to(b.px_per_m, a.px_per_m, 0);
    if (ok)
        for (size_t k = 0; k < (size_t)a.w * a.h; k++)
            if (a.z[k] != b.z[k]) { ok = 0; break; }

    printf("roundtrip: %dx%d save/load | %s\n", a.w, a.h, ok ? "ok" : "BAD");
    terrain_free(&a);
    terrain_free(&b);
    return ok;
}

static int plane_exact(void) {
    /* bilinear of a plane is exact: sample at arbitrary (non-grid) points */
    Terrain t;
    build(&t, 81, 65, 0.0f, 0.0f, 4.0f, plane);

    unsigned seed = 12345u;
    float worst = 0.0f;
    for (int n = 0; n < 2000; n++) {
        seed = seed * 1103515245u + 12345u;
        float x = (float)((seed >> 8) % 20000) / 1000.0f;   /* [0,20) */
        seed = seed * 1103515245u + 12345u;
        float y = (float)((seed >> 8) % 16000) / 1000.0f;   /* [0,16) */
        float got = terrain_z(&t, x, y);
        float want = plane(x, y);
        float e = fabsf(got - want);
        if (e > worst) worst = e;
    }
    int ok = worst < 1e-4f;
    printf("plane:     worst bilinear error %.2e over 2000 pts | %s\n",
           worst, ok ? "ok" : "BAD");
    terrain_free(&t);
    return ok;
}

/* sidewalk: flat 0 for x<=10, linear ramp 0->0.4 on [10,12], flat 0.4 for x>=12.
 * y-invariant. */
static float step_field(float x, float y) {
    (void)y;
    if (x <= 10.0f) return 0.0f;
    if (x >= 12.0f) return 0.4f;
    return 0.4f * (x - 10.0f) / 2.0f;
}

static int step_check(void) {
    Terrain t;
    build(&t, 81, 65, 0.0f, 0.0f, 4.0f, step_field);   /* step on a grid */

    int ok = 1;
    ok &= close_to(terrain_z(&t, 5.0f, 8.0f), 0.0f, 1e-5f);     /* flat low */
    ok &= close_to(terrain_z(&t, 15.0f, 8.0f), 0.4f, 1e-5f);    /* flat high */
    ok &= close_to(terrain_z(&t, 11.0f, 8.0f), 0.2f, 1e-5f);    /* ramp mid */
    /* midpoint between two grid samples = their average (sample step 0.25 m) */
    float a = terrain_z(&t, 10.5f, 8.0f), b = terrain_z(&t, 10.75f, 8.0f);
    ok &= close_to(terrain_z(&t, 10.625f, 8.0f), 0.5f * (a + b), 1e-5f);
    /* y-invariant: same x, different y => same height */
    ok &= close_to(terrain_z(&t, 11.0f, 1.0f), terrain_z(&t, 11.0f, 15.0f), 1e-5f);

    printf("step:      flat/ramp/flat + midpoint avg | %s\n", ok ? "ok" : "BAD");
    terrain_free(&t);
    return ok;
}

static int clamp_check(void) {
    Terrain t;
    build(&t, 81, 65, 0.0f, 0.0f, 4.0f, step_field);  /* x in [0,20], y in [0,16] */

    int ok = 1;
    /* below/left of origin -> edge sample (0,0) which is 0 */
    ok &= close_to(terrain_z(&t, -5.0f, -3.0f), step_field(0.0f, 0.0f), 1e-5f);
    /* far right/top -> edge (x=20,y=16): step_field(20,*) = 0.4 */
    ok &= close_to(terrain_z(&t, 999.0f, 999.0f), 0.4f, 1e-5f);
    /* clamp on one axis only */
    ok &= close_to(terrain_z(&t, 15.0f, -10.0f), terrain_z(&t, 15.0f, 0.0f), 1e-5f);

    printf("clamp:     out-of-AABB -> edge, no extrapolation | %s\n",
           ok ? "ok" : "BAD");
    terrain_free(&t);
    return ok;
}

int main(void) {
    int ok = 1;
    ok &= roundtrip();
    ok &= plane_exact();
    ok &= step_check();
    ok &= clamp_check();
    remove(TMP);
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

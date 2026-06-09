#define _POSIX_C_SOURCE 199309L
#include "sim_particles.h"
#include <stdio.h>
#include <math.h>

int main(void) {
    SimP *s = simp_create(100, 100, 0.5f, 5000);
    for (int x = 0; x < 100; x++) { simp_set_wall(s,x,0,true); simp_set_wall(s,x,99,true); }
    for (int y = 0; y < 100; y++) { simp_set_wall(s,0,y,true); simp_set_wall(s,99,y,true); }
    simp_set_goal(s, 95, 50, true);
    simp_terrain_commit(s);
    for (int r = 0; r < 50; r++) for (int c = 0; c < 50; c++)
        simp_spawn(s, 10.0f + c * 0.7f, 10.0f + r * 0.6f);
    for (int i = 0; i < 120; i++) simp_step(s, 1.0f/60.0f);
    /* blast in the middle of the pack */
    simp_apply_impulse(s, 25.0f, 25.0f, 8.0f, 15.0f);
    float peak = 0.0f; int bad = 0;
    for (int i = 0; i < 240; i++) {
        simp_step(s, 1.0f/60.0f);
        const float *px = simp_px(s), *py = simp_py(s);
        const float *vx = simp_vx(s), *vy = simp_vy(s);
        for (int k = 0; k < simp_count(s); k++) {
            if (!isfinite(px[k]) || !isfinite(py[k])) bad++;
            float sp = sqrtf(vx[k]*vx[k] + vy[k]*vy[k]);
            if (sp > peak) peak = sp;
        }
    }
    /* settle check: max speed after 4 s must be back near walking speed */
    float settled = 0.0f;
    const float *vx = simp_vx(s), *vy = simp_vy(s);
    for (int k = 0; k < simp_count(s); k++) {
        float sp = sqrtf(vx[k]*vx[k] + vy[k]*vy[k]);
        if (sp > settled) settled = sp;
    }
    printf("peak speed after blast: %.1f m/s | settled max: %.1f m/s | bad: %d\n",
           peak, settled, bad);
    int ok = bad == 0 && peak > 8.0f && settled < 6.0f;
    printf(ok ? "PASS\n" : "FAIL\n");
    simp_destroy(s);
    return ok ? 0 : 1;
}

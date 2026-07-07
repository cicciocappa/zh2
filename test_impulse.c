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
    simp_destroy(s);

    /* --- ballistic flight (M3.2): fresh sim, no goal so nobody drains ---- */
    s = simp_create(100, 100, 0.5f, 5000);
    simp_terrain_commit(s);
    for (int r = 0; r < 30; r++) for (int c = 0; c < 30; c++)
        simp_spawn(s, 15.0f + c * 0.7f, 15.0f + r * 0.6f);
    int n0 = simp_count(s);
    for (int i = 0; i < 60; i++) simp_step(s, 1.0f/60.0f);

    simp_apply_impulse_ex(s, 25.0f, 24.0f, 8.0f, 15.0f, 0.5f);
    const uint8_t *fl = simp_flags_arr(s);
    int nfly = 0;
    SimPHandle track = SIMP_HANDLE_INVALID;
    float track_vx = 0.0f, best = 0.0f;
    for (int k = 0; k < simp_count(s); k++) {
        if (!(fl[k] & SIMP_FLYING)) continue;
        nfly++;
        float av = fabsf(simp_vx(s)[k]);        /* track the fastest-horizontal flyer */
        if (av > best) { best = av; track = simp_handle_of(s, k); track_vx = simp_vx(s)[k]; }
    }

    /* vz peaks at 7.5 m/s -> airborne ~1.53 s = 92 steps; allow 120 */
    /* FLY_DRAG: horizontal vx now decays in flight (pure geometric drag) — no
       longer constant. Assert the decay is well-behaved: same sign throughout,
       never grows step-to-step, and is meaningfully reduced by touchdown. */
    int landed_total = 0, last_fly_step = -1, bad_fly = 0, vx_bad = 0;
    float track_vx_last = track_vx;
    for (int t = 0; t < 120; t++) {
        simp_step(s, 1.0f/60.0f);
        landed_total += simp_landed_count(s);
        int ti = simp_index_of(s, track);
        if (ti >= 0 && (fl[ti] & SIMP_FLYING)) {
            float v = simp_vx(s)[ti];
            if (v * track_vx < 0.0f) vx_bad = 1;                       /* sign flipped */
            if (fabsf(v) > fabsf(track_vx_last) + 1e-4f) vx_bad = 1;   /* grew (not pure drag) */
            track_vx_last = v;
        }
        for (int k = 0; k < simp_count(s); k++) {
            if (fl[k] & SIMP_FLYING) last_fly_step = t;
            if (!isfinite(simp_px(s)[k]) || !isfinite(simp_z_arr(s)[k])) bad_fly++;
        }
    }
    int still_fly = 0;
    for (int k = 0; k < simp_count(s); k++) {
        if (fl[k] & SIMP_FLYING) still_fly++;
        if (simp_z_arr(s)[k] != 0.0f) bad_fly++;
    }
    int drag_ok = vx_bad == 0 && fabsf(track_vx_last) < fabsf(track_vx) * 0.9f;
    printf("flight: launched=%d landed=%d last airborne step=%d "
           "vx %.2f->%.2f (drag) still flying=%d bad=%d count %d->%d\n",
           nfly, landed_total, last_fly_step, (double)track_vx, (double)track_vx_last,
           still_fly, bad_fly, n0, simp_count(s));
    int ok_fly = nfly > 20 && landed_total == nfly && still_fly == 0 &&
                 last_fly_step <= 100 && drag_ok && bad_fly == 0 &&
                 simp_count(s) == n0;
    simp_destroy(s);

    ok = ok && ok_fly;
    printf(ok ? "PASS\n" : "FAIL\n");
    return ok ? 0 : 1;
}

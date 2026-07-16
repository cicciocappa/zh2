/* soldier.c — playable soldier (SOLDIER_DESIGN.md). See soldier.h. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "soldier.h"

#define TOUCH_MAX 32            /* contact query buffer (geometry caps ~8) */
#define LURE_EPS  0.02f         /* skip rim cells below this (as defense.c) */

struct Soldier {
    SimP *s;
    SoldierDef def;

    bool  active;
    int   drag;                 /* draggable pool index while active */
    float hp;
    float fire_cd;              /* s until the gun may fire again */
    float down_t;               /* respawn lockout left */
    int   touchers;             /* contact count last step (HUD/FX) */

    /* mobile attraction stamp: deltas ACTUALLY applied by simp_add_cost
     * (read back around the call — the core clamp at -0.8 and overlapping
     * stamps make blind subtraction wrong; same pattern as defense.c). */
    bool     stamped;
    int      stamp_cx, stamp_cy;    /* nav cell the stamp was laid from */
    int32_t *lure_cell;
    float   *lure_delta;
    int      lure_n;
};

void soldier_def_defaults(SoldierDef *d) {
    memset(d, 0, sizeof *d);
    d->radius      = 0.35f;
    d->mass        = 15.0f;     /* a tank is 10: the crowd shoves, not bowls */
    d->speed       = 4.2f;      /* kites walkers (1.4), outrun by nobody     */
    d->accel       = 30.0f;
    d->hp_max      = 120.0f;
    d->touch_dps   = 15.0f;
    d->touch_knock = 20.0f;
    d->touch_pad   = 0.15f;
    d->gun_range   = 16.0f;
    d->gun_period  = 0.10f;
    d->gun_damage  = 35.0f;
    d->lure_w      = -0.8f;     /* full weight: the measured lesson from the
                                   fire lure — anything softer only grazes */
    d->lure_r      = 10.0f;
    d->down_s      = 8.0f;
}

Soldier *soldier_create(SimP *s, const SoldierDef *def) {
    Soldier *sol = (Soldier *)calloc(1, sizeof(Soldier));
    if (!sol) return NULL;
    sol->s = s;
    if (def) sol->def = *def;
    else soldier_def_defaults(&sol->def);
    sol->drag = -1;
    int cr = (int)ceilf(sol->def.lure_r / simp_cell_size(s));
    int cap = (2 * cr + 1) * (2 * cr + 1);
    sol->lure_cell  = (int32_t *)malloc((size_t)cap * sizeof(int32_t));
    sol->lure_delta = (float *)malloc((size_t)cap * sizeof(float));
    if (!sol->lure_cell || !sol->lure_delta) {
        soldier_destroy(sol);
        return NULL;
    }
    return sol;
}

void soldier_destroy(Soldier *sol) {
    if (!sol) return;
    if (sol->active) soldier_recall(sol);
    free(sol->lure_cell);
    free(sol->lure_delta);
    free(sol);
}

/* ---- attraction stamp ----------------------------------------------------- */

static void lure_remove(Soldier *sol) {
    int gw = simp_grid_w(sol->s);
    for (int k = 0; k < sol->lure_n; k++) {
        int idx = sol->lure_cell[k];
        simp_add_cost(sol->s, idx % gw, idx / gw, -sol->lure_delta[k]);
    }
    sol->lure_n = 0;
    sol->stamped = false;
}

static void lure_apply(Soldier *sol, float x, float y) {
    SimP *s = sol->s;
    float cs = simp_cell_size(s), R = sol->def.lure_r;
    int gw = simp_grid_w(s), gh = simp_grid_h(s);
    int cr = (int)ceilf(R / cs);
    int cx0 = (int)(x / cs) - cr, cx1 = (int)(x / cs) + cr;
    int cy0 = (int)(y / cs) - cr, cy1 = (int)(y / cs) + cr;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= gw) cx1 = gw - 1;
    if (cy1 >= gh) cy1 = gh - 1;
    const float *uc = simp_user_cost(s);
    int n = 0;
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++) {
            float dx = (cx + 0.5f) * cs - x, dy = (cy + 0.5f) * cs - y;
            float d = sqrtf(dx * dx + dy * dy);
            if (d >= R) continue;
            /* linear cone, minimum AT the soldier (flat plateaus only graze
             * routes — measured in test_lure) */
            float w = sol->def.lure_w * (1.0f - d / R);
            if (w > -LURE_EPS) continue;
            int idx = cy * gw + cx;
            float before = uc[idx];
            simp_add_cost(s, cx, cy, w);
            float delta = uc[idx] - before;
            if (delta == 0.0f) continue;    /* clamped away: nothing to undo */
            sol->lure_cell[n] = idx;
            sol->lure_delta[n] = delta;
            n++;
        }
    sol->lure_n = n;
    sol->stamped = true;
    sol->stamp_cx = (int)(x / cs);
    sol->stamp_cy = (int)(y / cs);
}

/* ---- lifecycle ------------------------------------------------------------ */

bool soldier_deploy(Soldier *sol, float x, float y) {
    if (sol->active || sol->down_t > 0.0f) return false;
    int i = simp_drag_add(sol->s, x, y, sol->def.radius, sol->def.mass);
    if (i < 0) return false;
    sol->drag = i;
    sol->active = true;
    sol->hp = sol->def.hp_max;
    sol->fire_cd = 0.0f;
    sol->touchers = 0;
    return true;
}

void soldier_recall(Soldier *sol) {
    if (!sol->active) return;
    if (sol->stamped) lure_remove(sol);
    simp_drag_remove(sol->s, sol->drag);
    sol->drag = -1;
    sol->active = false;
    sol->touchers = 0;
}

static void go_down(Soldier *sol) {
    soldier_recall(sol);
    sol->down_t = sol->def.down_s;
}

/* ---- step ------------------------------------------------------------------ */

void soldier_step(Soldier *sol, float mvx, float mvy,
                  float aimx, float aimy, bool fire, float dt,
                  SoldierShotFn shot, void *ud) {
    if (!sol->active) {
        if (sol->down_t > 0.0f) {
            sol->down_t -= dt;
            if (sol->down_t < 0.0f) sol->down_t = 0.0f;
        }
        return;
    }
    SimP *s = sol->s;
    float x = simp_drag_px(s)[sol->drag], y = simp_drag_py(s)[sol->drag];
    float vx = simp_drag_vx(s)[sol->drag], vy = simp_drag_vy(s)[sol->drag];

    /* contact: zombies whose disc overlaps ours (+pad) bite and push. The
     * grid query wants center distance: probe out to summed radii of the
     * LARGEST default-ish agent; per-agent radii re-checked below. */
    int idx[TOUCH_MAX];
    float probe = sol->def.radius + 0.60f + sol->def.touch_pad;
    int nq = simp_query_circle(s, x, y, probe, idx, TOUCH_MAX, 0);
    const float *px = simp_px(s), *py = simp_py(s);
    const float *rad = simp_radius_arr(s);
    int touch = 0;
    float kx = 0.0f, ky = 0.0f;             /* summed push-away direction */
    for (int k = 0; k < nq; k++) {
        int i = idx[k];
        float dx = x - px[i], dy = y - py[i];
        float d = sqrtf(dx * dx + dy * dy);
        float reach = rad[i] + sol->def.radius + sol->def.touch_pad;
        if (d > reach) continue;
        touch++;
        if (d > 1e-4f) { kx += dx / d; ky += dy / d; }
    }
    sol->touchers = touch;
    if (touch > 0) {
        sol->hp -= sol->def.touch_dps * (float)touch * dt;
        if (sol->hp <= 0.0f) {
            sol->hp = 0.0f;
            go_down(sol);
            return;
        }
    }

    /* drive: accelerate toward the wanted velocity. NEVER overwrite raw —
     * the blend is what lets the PBD shove (knockback) live through. */
    float ml = sqrtf(mvx * mvx + mvy * mvy);
    float wx = 0.0f, wy = 0.0f;
    if (ml > 1e-4f) {
        wx = mvx / ml * sol->def.speed;
        wy = mvy / ml * sol->def.speed;
    }
    float dvx = wx - vx, dvy = wy - vy;
    float dvl = sqrtf(dvx * dvx + dvy * dvy);
    float dvmax = sol->def.accel * dt;
    if (dvl > dvmax) {
        dvx *= dvmax / dvl;
        dvy *= dvmax / dvl;
    }
    vx += dvx; vy += dvy;

    /* knockback AFTER the blend: added before it, the brake-toward-wanted
     * would swallow the kick in the same step and bites would barely move
     * the soldier (measured). This way each contact step nets a push-away
     * of touch_knock*dt that the next step's blend has to fight. */
    if (touch > 0) {
        float kl = sqrtf(kx * kx + ky * ky);
        if (kl > 1e-4f) {
            vx += kx / kl * sol->def.touch_knock * dt;
            vy += ky / kl * sol->def.touch_knock * dt;
        }
    }
    simp_drag_set_vel(s, sol->drag, vx, vy);

    /* attraction: restamp when we cross into a new nav cell */
    if (sol->def.lure_w < 0.0f) {
        float cs = simp_cell_size(s);
        int cx = (int)(x / cs), cy = (int)(y / cs);
        if (!sol->stamped || cx != sol->stamp_cx || cy != sol->stamp_cy) {
            if (sol->stamped) lure_remove(sol);
            lure_apply(sol, x, y);
        }
    }

    /* machine gun: hitscan toward the aim point, first agent hit, damage
     * scaled by transmittance (shoots through fences like a turret). */
    if (sol->fire_cd > 0.0f) sol->fire_cd -= dt;
    if (fire && sol->fire_cd <= 0.0f) {
        float dx = aimx - x, dy = aimy - y;
        float dl = sqrtf(dx * dx + dy * dy);
        if (dl > 1e-3f) {
            dx /= dl; dy /= dl;
            int hi; float ht;
            int nh = simp_query_ray(s, x, y, dx, dy, sol->def.gun_range,
                                    &hi, &ht, 1, 0);
            float ex, ey, dmg = 0.0f;
            int hit = -1;
            if (nh > 0) {
                hit = hi;
                ex = x + dx * ht;
                ey = y + dy * ht;
                dmg = sol->def.gun_damage *
                      simp_ray_transmit(s, x, y, dx, dy, ht);
            } else {
                float wd = simp_wall_ray(s, x, y, dx, dy, sol->def.gun_range);
                ex = x + dx * wd;
                ey = y + dy * wd;
            }
            if (shot) shot(ud, x, y, ex, ey, hit, dmg);
            sol->fire_cd += sol->def.gun_period;
        }
    }
}

/* ---- getters ---------------------------------------------------------------- */

bool  soldier_active(const Soldier *sol)   { return sol->active; }
float soldier_x(const Soldier *sol)
{ return sol->active ? simp_drag_px(sol->s)[sol->drag] : 0.0f; }
float soldier_y(const Soldier *sol)
{ return sol->active ? simp_drag_py(sol->s)[sol->drag] : 0.0f; }
float soldier_hp(const Soldier *sol)       { return sol->hp; }
float soldier_hp_max(const Soldier *sol)   { return sol->def.hp_max; }
float soldier_down(const Soldier *sol)     { return sol->down_t; }
int   soldier_touchers(const Soldier *sol) { return sol->touchers; }
int   soldier_body_index(const Soldier *sol)
{ return sol->active ? sol->drag : -1; }

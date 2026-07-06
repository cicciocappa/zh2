/* destruct.c — destructible decor props. See destruct.h / DESTRUCT_DESIGN.md. */

#include "destruct.h"
#include <math.h>
#include <string.h>

#define DESTRUCT_QBUF 32   /* agents sampled to estimate the push direction */

void destruct_init(Destruct *d, const Scene *sc, const PropCatalog *cat) {
    memset(d, 0, sizeof *d);
    d->n = sc->n_prop;
    for (int i = 0; i < sc->n_prop; i++) {
        const PropDef *pd = prop_catalog_find(cat, sc->prop[i].key);
        d->def[i]   = pd;
        d->state[i] = (pd && pd->destructible) ? DESTRUCT_ALIVE : DESTRUCT_INERT;
    }
}

/* push heading from the mean velocity of the agents in `buf`; fall back to the
 * prop's authored rotation if the crowd is (near) still. */
static float push_dir(const SimP *s, const int *buf, int n, float fallback_rot) {
    const float *vx = simp_vx(s), *vy = simp_vy(s);
    float ax = 0.0f, ay = 0.0f;
    for (int k = 0; k < n; k++) { ax += vx[buf[k]]; ay += vy[buf[k]]; }
    if (ax * ax + ay * ay > 1e-6f) return atan2f(ay, ax);
    return fallback_rot * 0.01745329f;   /* deg -> rad */
}

int destruct_update(Destruct *d, const SimP *s, const Scene *sc, float dt,
                    DestructBurstFn on_burst, void *ud) {
    int changed = 0;
    int buf[DESTRUCT_QBUF];
    for (int i = 0; i < d->n; i++) {
        if (d->state[i] == DESTRUCT_ALIVE) {
            const PropDef *pd = d->def[i];   /* cached at init: ALIVE => valid */
            float px = sc->prop[i].x, py = sc->prop[i].y;
            int n = simp_query_circle(s, px, py, pd->trigger_radius, buf, DESTRUCT_QBUF, 0);
            if (n > 0) {
                /* estimate the push direction from a WIDER sample than the trigger
                 * radius: a couple of agents jostling in the contact disc give a
                 * noisy mean; the surrounding flow is smoother (and nicer debris). */
                float dr = pd->trigger_radius * 2.5f; if (dr < 2.5f) dr = 2.5f;
                int nd = simp_query_circle(s, px, py, dr, buf, DESTRUCT_QBUF, 0);
                d->dir[i] = push_dir(s, buf, nd > 0 ? nd : n, sc->prop[i].rot);
                changed = 1;
                if (pd->topple > 0.0f) {
                    d->state[i] = DESTRUCT_TOPPLING;
                    d->timer[i] = pd->topple;
                    d->topple_total[i] = pd->topple;
                } else {
                    d->state[i] = DESTRUCT_GONE;
                    if (on_burst) on_burst(i, pd->debris, px, py, d->dir[i], ud);
                }
            }
        } else if (d->state[i] == DESTRUCT_TOPPLING) {
            d->timer[i] -= dt;
            if (d->timer[i] <= 0.0f) {
                const PropDef *pd = d->def[i];
                d->state[i] = DESTRUCT_GONE;
                changed = 1;
                if (on_burst)
                    on_burst(i, pd ? pd->debris : "", sc->prop[i].x, sc->prop[i].y,
                             d->dir[i], ud);
            }
        }
    }
    return changed;
}

void destruct_force(Destruct *d, const Scene *sc, int i, float dir,
                    DestructBurstFn on_burst, void *ud) {
    if (i < 0 || i >= d->n) return;
    if (d->state[i] == DESTRUCT_GONE) return;   /* already shattered */
    const PropDef *pd = d->def[i];              /* may be NULL / inert */
    d->dir[i]   = dir;
    d->state[i] = DESTRUCT_GONE;                /* no topple: the wave is instant */
    if (on_burst)
        on_burst(i, pd ? pd->debris : "", sc->prop[i].x, sc->prop[i].y, dir, ud);
}

int destruct_state(const Destruct *d, int i) {
    return (i >= 0 && i < d->n) ? d->state[i] : DESTRUCT_GONE;
}

float destruct_topple_t(const Destruct *d, int i) {
    if (i < 0 || i >= d->n) return 0.0f;
    if (d->state[i] != DESTRUCT_TOPPLING || d->topple_total[i] <= 0.0f) return 0.0f;
    float t = 1.0f - d->timer[i] / d->topple_total[i];
    return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

float destruct_dir(const Destruct *d, int i) {
    return (i >= 0 && i < d->n) ? d->dir[i] : 0.0f;
}

int destruct_animating(const Destruct *d) {
    for (int i = 0; i < d->n; i++)
        if (d->state[i] == DESTRUCT_TOPPLING) return 1;
    return 0;
}

/* traps.c — placed traps (mine). See traps.h / GAME_PLAN fase D. */

#include "traps.h"
#include <string.h>

#define TRAP_PROBE 4   /* we only need to know IF any agent is in range */

void traps_init(Traps *t) { memset(t, 0, sizeof *t); }

int traps_add(Traps *t, const TrapDef *def) {
    if (t->n >= TRAPS_MAX) return -1;
    int id = t->n++;
    t->def[id]   = *def;
    t->live[id]  = 1;
    t->arm_t[id] = def->arm_delay > 0.0f ? def->arm_delay : 0.0f;
    return id;
}

int traps_count(const Traps *t) { return t->n; }

int traps_alive(const Traps *t, int id) {
    return (id >= 0 && id < t->n) ? t->live[id] : 0;
}

void traps_update(Traps *t, const SimP *s, float dt,
                  TrapBlastFn on_blast, void *user) {
    int buf[TRAP_PROBE];
    for (int id = 0; id < t->n; id++) {
        if (!t->live[id]) continue;
        if (t->arm_t[id] > 0.0f) {          /* still arming: can't fire yet */
            t->arm_t[id] -= dt;
            continue;
        }
        const TrapDef *d = &t->def[id];
        /* MINE: fire on the first live agent within the trigger radius. The
         * query is exact right after the step; a small probe suffices (we only
         * need presence, not the full set). Corpses are not returned. */
        int n = simp_query_circle(s, d->x, d->y, d->trig_r, buf, TRAP_PROBE, 0);
        if (n <= 0) continue;
        t->live[id] = 0;                    /* one-shot: consumed */
        if (on_blast)
            on_blast(user, id, d->x, d->y, d->blast_r, d->dmg, d->strength, d->up);
    }
}

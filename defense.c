/* defense.c — implementation. See defense.h and M5_DESIGN.md. */

#include "defense.h"
#include <stdlib.h>
#include <math.h>

/* Body table: hp_max, radius, v_pref, mass (walker units), heavy_hits, bounty. */
typedef struct {
    int   hp_max;
    float radius, v_pref, mass;
    int   heavy_hits;     /* heavy-turret hits to gib (normals = 1, tank > 1) */
    int   biomass;        /* bounty (economy, slice 2 — stored, unused here)  */
} DefEnemyDef;

static const DefEnemyDef ENEMY[BT_COUNT] = {
    /* OBESE */ { 160, 0.34f, 1.1f,  1.6f, 1,  30 },
    /* MAN   */ { 100, 0.30f, 1.4f,  1.0f, 1,  20 },
    /* WOMAN */ {  80, 0.27f, 1.5f,  0.85f,1,  18 },
    /* CHILD */ {  50, 0.22f, 1.7f,  0.5f, 1,  12 },
    /* TANK  */ { 600, 0.55f, 1.0f, 10.0f, 4, 100 },
};

#define V_CRAWL   0.35f      /* crawl preferred speed (maimed_legs)          */
#define P_CORPSE  0.25f      /* light kill leaves a corpse with this prob.   */
#define CORPSE_TTL 9.0f
#define GIB_RADIUS 1.0f      /* heavy-kill knockback radius                  */
#define GIB_PUSH   6.0f      /* heavy-kill knockback strength                */
#define MAXPIERCE  64        /* max agents a piercing shot resolves          */
#define TURRET_CAP 256

struct DefGame {
    SimP *s;
    int   cap;
    int     *hp;        /* per slot */
    uint8_t *body;      /* per slot: DefBody */
    uint8_t *wound;     /* per slot: DefWound */
    uint8_t *hheat;     /* per slot: heavy hits absorbed (tank) */
    DefTurret turrets[TURRET_CAP];
    int   nturrets;
    int  *qbuf;         /* cap: acquire buffer (query_circle) */
    int   raybuf[MAXPIERCE];
    float rayt[MAXPIERCE];
    SimPHandle rayh[MAXPIERCE];
    int   kills, shots;
};

/* deterministic per-slot hash (no RNG state: same slot -> same roll) */
static inline uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}

DefGame *def_create(SimP *s, int cap) {
    DefGame *g = (DefGame *)calloc(1, sizeof(DefGame));
    g->s = s; g->cap = cap;
    g->hp    = (int *)calloc((size_t)cap, sizeof(int));
    g->body  = (uint8_t *)calloc((size_t)cap, 1);
    g->wound = (uint8_t *)calloc((size_t)cap, 1);
    g->hheat = (uint8_t *)calloc((size_t)cap, 1);
    g->qbuf  = (int *)malloc((size_t)cap * sizeof(int));
    return g;
}

void def_destroy(DefGame *g) {
    if (!g) return;
    free(g->hp); free(g->body); free(g->wound); free(g->hheat); free(g->qbuf);
    free(g);
}

SimPHandle def_spawn(DefGame *g, float x, float y, DefBody body) {
    const DefEnemyDef *d = &ENEMY[body];
    SimPAgentDesc desc = { d->radius, d->v_pref, d->mass };
    int i = simp_spawn_desc(g->s, x, y, &desc);
    if (i < 0) return SIMP_HANDLE_INVALID;
    int slot = simp_slot_of(g->s, i);
    g->hp[slot] = d->hp_max;
    g->body[slot] = (uint8_t)body;
    g->wound[slot] = DW_NONE;
    g->hheat[slot] = 0;
    return simp_handle_of(g->s, i);
}

int def_add_turret(DefGame *g, const DefTurret *t) {
    if (g->nturrets >= TURRET_CAP) return -1;
    int id = g->nturrets++;
    g->turrets[id] = *t;
    if (g->turrets[id].sweep_dir == 0) g->turrets[id].sweep_dir = 1;
    return id;
}
DefTurret *def_turret(DefGame *g, int id) {
    return (id >= 0 && id < g->nturrets) ? &g->turrets[id] : NULL;
}
int def_turret_count(const DefGame *g) { return g->nturrets; }

/* ---- damage / wounds / death ---- */

static inline float wrap_pi(float a) {
    while (a >  3.14159265f) a -= 6.28318531f;
    while (a <= -3.14159265f) a += 6.28318531f;
    return a;
}

static void gib(DefGame *g, int i) {           /* heavy kill: no corpse */
    float x = simp_px(g->s)[i], y = simp_py(g->s)[i];
    simp_apply_impulse(g->s, x, y, GIB_RADIUS, GIB_PUSH);
    simp_kill(g->s, i);
    g->kills++;
}

static void die_light(DefGame *g, int i, int slot) {
    float x = simp_px(g->s)[i], y = simp_py(g->s)[i];
    float r = simp_radius_arr(g->s)[i];
    uint32_t h = hash_u32((uint32_t)slot * 2654435761u ^ 0x00C0FFEEu);
    if ((h & 1023u) < (uint32_t)(P_CORPSE * 1024.0f))
        simp_corpse_add(g->s, x, y, r * 0.9f, CORPSE_TTL);
    simp_kill(g->s, i);
    g->kills++;
}

static void wound_roll(DefGame *g, int i, int slot) {
    uint32_t r = hash_u32((uint32_t)slot * 2654435761u + 0x9E3779B9u) % 3u;
    DefWound w = (r == 0) ? DW_BLOODY : (r == 1) ? DW_MAIMED_ARM : DW_CRAWLING;
    g->wound[slot] = (uint8_t)w;
    if (w == DW_CRAWLING) simp_set_vpref(g->s, i, V_CRAWL);
}

/* Apply one shot's worth of damage to a live handle. Resolved from handle so
 * earlier kills in the same (piercing) shot don't invalidate it. */
static void apply_damage(DefGame *g, SimPHandle h, const DefTurret *t) {
    int i = simp_index_of(g->s, h);
    if (i < 0) return;
    int slot = simp_slot_of(g->s, i);
    if (t->heavy) {
        if (g->body[slot] == BT_TANK &&
            (int)(++g->hheat[slot]) < ENEMY[BT_TANK].heavy_hits)
            return;                            /* tank survives this hit */
        gib(g, i);                             /* normal, or tank's last hit */
    } else {
        g->hp[slot] -= (int)t->damage;
        if (g->hp[slot] <= 0) die_light(g, i, slot);
        else if (g->wound[slot] == DW_NONE) wound_roll(g, i, slot);
    }
}

/* ---- turret update ---- */

static void turret_update(DefGame *g, DefTurret *t, float dt) {
    SimP *s = g->s;
    const float *px = simp_px(s), *py = simp_py(s);

    /* acquire: nearest live agent inside range AND arc */
    float cx = (t->arc_min + t->arc_max) * 0.5f;
    float half = (t->arc_max - t->arc_min) * 0.5f;
    int n = simp_query_circle(s, t->x, t->y, t->range, g->qbuf, g->cap, 0);
    int best = -1; float bestd2 = 1e30f;
    for (int k = 0; k < n; k++) {
        int i = g->qbuf[k];
        float dx = px[i] - t->x, dy = py[i] - t->y;
        float bearing = atan2f(dy, dx);
        if (fabsf(wrap_pi(bearing - cx)) > half) continue;   /* outside arc */
        float d2 = dx * dx + dy * dy;
        if (d2 < bestd2) { bestd2 = d2; best = i; }
    }

    if (best >= 0) {                           /* dwell: turn toward target */
        float bearing = atan2f(py[best] - t->y, px[best] - t->x);
        float diff = wrap_pi(bearing - t->ang);
        float maxstep = t->sweep_speed * dt;
        t->ang += (fabsf(diff) <= maxstep) ? diff
                  : (diff > 0 ? maxstep : -maxstep);
    } else {                                   /* sweep the arc */
        t->ang += (float)t->sweep_dir * t->sweep_speed * dt;
        if (t->ang > t->arc_max) { t->ang = t->arc_max; t->sweep_dir = -1; }
        if (t->ang < t->arc_min) { t->ang = t->arc_min; t->sweep_dir =  1; }
    }

    /* fire only with a target (free bullets, no waste on empty arc) */
    t->fire_timer += dt;
    if (best < 0) {
        if (t->fire_timer > t->fire_period) t->fire_timer = t->fire_period;
        return;
    }
    if (t->fire_timer < t->fire_period) return;
    t->fire_timer -= t->fire_period;
    g->shots++;

    int max_out = t->piercing ? MAXPIERCE : 1;
    int nh = simp_query_ray(s, t->x, t->y, cosf(t->ang), sinf(t->ang),
                            t->range, g->raybuf, g->rayt, max_out, 0);
    if (nh <= 0) return;
    int count = t->piercing ? nh : 1;
    /* convert ALL hits to handles before any kill (M3.4 index trap) */
    for (int k = 0; k < count; k++) g->rayh[k] = simp_handle_of(s, g->raybuf[k]);
    for (int k = 0; k < count; k++) apply_damage(g, g->rayh[k], t);
}

void def_update(DefGame *g, float dt) {
    for (int id = 0; id < g->nturrets; id++)
        turret_update(g, &g->turrets[id], dt);
}

/* ---- read access ---- */

int def_kills(const DefGame *g) { return g->kills; }
int def_shots(const DefGame *g) { return g->shots; }

int def_count_wound(const DefGame *g, DefWound w) {
    int n = simp_count(g->s), c = 0;
    for (int i = 0; i < n; i++)
        if (g->wound[simp_slot_of(g->s, i)] == (uint8_t)w) c++;
    return c;
}

const int     *def_hp(const DefGame *g)    { return g->hp; }
const uint8_t *def_wound(const DefGame *g) { return g->wound; }
const uint8_t *def_body(const DefGame *g)  { return g->body; }

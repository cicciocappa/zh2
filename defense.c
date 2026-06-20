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

/* §7 siege of structures — same discrete-attack model as test_siege.c */
#define STRUCT_CAP    64
#define ATTACK_PERIOD 0.8f   /* s between hits while an agent presses        */
#define ATTACK_DAMAGE 5.0f   /* HP per hit                                   */
#define ATTACK_MIN_P  0.006f /* min wall_pressure to count as a real attack
                              * (gates out the tangential grazing leak)      */

typedef struct {
    float hp, hp_max;
    int   is_core;       /* collapse = loss, not reroute */
    int   collapsed;
} DefStruct;

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
    /* §7 base & defeat */
    int       gw, gh;       /* cached nav-grid dims (decode wall_cell)       */
    DefStruct structs[STRUCT_CAP];
    int       nstructs;
    int16_t  *cell_struct;  /* gw*gh: structure id per nav cell, -1 = none   */
    float    *atk_timer;    /* per slot: siege attack accumulator            */
    int       lost;         /* 1 once the core has collapsed                 */
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
    g->gw = simp_grid_w(s); g->gh = simp_grid_h(s);
    size_t ncell = (size_t)g->gw * (size_t)g->gh;
    g->cell_struct = (int16_t *)malloc(ncell * sizeof(int16_t));
    for (size_t k = 0; k < ncell; k++) g->cell_struct[k] = -1;
    g->atk_timer = (float *)calloc((size_t)cap, sizeof(float));
    return g;
}

void def_destroy(DefGame *g) {
    if (!g) return;
    free(g->hp); free(g->body); free(g->wound); free(g->hheat); free(g->qbuf);
    free(g->cell_struct); free(g->atk_timer);
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

#define TRACER_TTL 0.05f

static void turret_update(DefGame *g, DefTurret *t, float dt) {
    SimP *s = g->s;
    const float *px = simp_px(s), *py = simp_py(s);
    t->fired = 0;
    if (t->tracer_ttl > 0.0f) t->tracer_ttl -= dt;

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
    t->fired = 1;
    t->tracer_ttl = TRACER_TTL;

    int max_out = t->piercing ? MAXPIERCE : 1;
    int nh = simp_query_ray(s, t->x, t->y, cosf(t->ang), sinf(t->ang),
                            t->range, g->raybuf, g->rayt, max_out, 0);
    t->last_t = (nh > 0) ? g->rayt[0] : t->range;
    if (nh <= 0) return;
    int count = t->piercing ? nh : 1;
    /* convert ALL hits to handles before any kill (M3.4 index trap) */
    for (int k = 0; k < count; k++) g->rayh[k] = simp_handle_of(s, g->raybuf[k]);
    for (int k = 0; k < count; k++) apply_damage(g, g->rayh[k], t);
}

/* ---- §7 base & defeat: structures + siege ---- */

int def_add_structure(DefGame *g, float hp_max, int is_core) {
    if (g->nstructs >= STRUCT_CAP) return -1;
    int id = g->nstructs++;
    g->structs[id].hp = g->structs[id].hp_max = hp_max;
    g->structs[id].is_core = is_core ? 1 : 0;
    g->structs[id].collapsed = 0;
    return id;
}

void def_struct_cell(DefGame *g, int id, int cx, int cy) {
    if (id < 0 || id >= g->nstructs) return;
    if (cx < 0 || cy < 0 || cx >= g->gw || cy >= g->gh) return;
    g->cell_struct[cy * g->gw + cx] = (int16_t)id;
    simp_set_wall(g->s, cx, cy, true);
}

/* HP hit zero: free the structure's cells and recommit the nav so the horde
 * reroutes. The CORE is special — it doesn't reroute, its fall is the loss. */
static void collapse_structure(DefGame *g, int id) {
    DefStruct *st = &g->structs[id];
    st->collapsed = 1;
    st->hp = 0.0f;
    if (st->is_core) { g->lost = 1; return; }
    int gw = g->gw, n = gw * g->gh;
    for (int c = 0; c < n; c++)
        if (g->cell_struct[c] == (int16_t)id) {
            simp_set_wall(g->s, c % gw, c / gw, false);
            g->cell_struct[c] = -1;
        }
    simp_terrain_commit(g->s);
}

/* Per-slot discrete attacks from grounded agents pressing a structure to reach
 * the goal beyond (SIEGE sensor, fresh after the step). Mirrors test_siege.c. */
static void siege_update(DefGame *g, float dt) {
    SimP *s = g->s;
    const float *wp = simp_wall_pressure(s);
    const int   *wc = simp_wall_cell(s);
    int n = simp_count(s);
    for (int i = 0; i < n; i++) {
        int slot = simp_slot_of(s, i);
        float p = wp[i];
        int cell = (p > 0.0f) ? wc[i] : -1;
        int sid  = (cell >= 0) ? g->cell_struct[cell] : -1;
        if (p >= ATTACK_MIN_P && sid >= 0 && !g->structs[sid].collapsed) {
            g->atk_timer[slot] += dt;
            if (g->atk_timer[slot] >= ATTACK_PERIOD) {
                g->atk_timer[slot] -= ATTACK_PERIOD;
                g->structs[sid].hp -= ATTACK_DAMAGE;
                if (g->structs[sid].hp <= 0.0f) collapse_structure(g, sid);
            }
        } else {
            g->atk_timer[slot] = 0.0f;   /* not really pressing: no free hit */
        }
    }
}

int   def_struct_count(const DefGame *g) { return g->nstructs; }
int   def_cell_struct(const DefGame *g, int cx, int cy) {
    if (cx < 0 || cy < 0 || cx >= g->gw || cy >= g->gh) return -1;
    return g->cell_struct[cy * g->gw + cx];
}
float def_struct_hp(const DefGame *g, int id) {
    return (id >= 0 && id < g->nstructs) ? g->structs[id].hp : 0.0f;
}
float def_struct_hp_max(const DefGame *g, int id) {
    return (id >= 0 && id < g->nstructs) ? g->structs[id].hp_max : 0.0f;
}
int def_struct_collapsed(const DefGame *g, int id) {
    return (id >= 0 && id < g->nstructs) ? g->structs[id].collapsed : 0;
}
int def_lost(const DefGame *g) { return g->lost; }

void def_update(DefGame *g, float dt) {
    for (int id = 0; id < g->nturrets; id++)
        turret_update(g, &g->turrets[id], dt);
    siege_update(g, dt);
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

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
#define DISMEMBER_NAV 0.5f   /* gore-nav weight of a gibbed body vs a corpse:
                              * 0.5 = half, so ~8 splats out-cost a cell like
                              * 4 normal corpses (slower clog, CORPSE_DESIGN
                              * §7-bis). No physical disc (torn apart).        */
#define MAXPIERCE  64        /* max agents a piercing shot resolves          */
#define TURRET_CAP 256
/* blood-fear (CORPSE_DESIGN.md, 2026-06-25): every death stains the cell; the
 * horde's animal instinct reroutes around bloody killzones. ~4 deaths in a
 * cell saturate the cost term. Deposited where the blood decal is emitted. */
#define DANGER_R   0.7f      /* blood-fear deposit radius (m) per death       */
#define DANGER_W   0.25f     /* blood-fear added per death (saturates at 1)   */

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
    int       budget;       /* §8 placement budget                          */
    DefEventFn ev_cb; void *ev_user;  /* render hook: hit/death events        */
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

static void gib(DefGame *g, int i) {           /* heavy kill: no intact corpse */
    float x = simp_px(g->s)[i], y = simp_py(g->s)[i];
    float r = simp_radius_arr(g->s)[i];
    simp_apply_impulse(g->s, x, y, GIB_RADIUS, GIB_PUSH);
    /* dismembered: no collidable corpse, but it still gores the cell -> nav cost
     * at half a corpse's rate (§7-bis, slow clog in heavy-weapon killzones).
     * Same effective radius (r*0.9) as a normal corpse so DISMEMBER_NAV reads as
     * a clean fraction of one: 0.5 -> 8 splats == 4 corpses. */
    simp_corpse_splat(g->s, x, y, r * 0.9f, DISMEMBER_NAV);
    simp_add_danger(g->s, x, y, DANGER_R, DANGER_W);     /* bloody killzone */
    if (g->ev_cb) g->ev_cb(g->ev_user, simp_slot_of(g->s, i), i,
                           (DefBody)g->body[simp_slot_of(g->s, i)], DEF_EV_GIB);
    simp_kill(g->s, i);
    g->kills++;
}

static void die_light(DefGame *g, int i, int slot) {
    float x = simp_px(g->s)[i], y = simp_py(g->s)[i];
    float r = simp_radius_arr(g->s)[i];
    uint32_t h = hash_u32((uint32_t)slot * 2654435761u ^ 0x00C0FFEEu);
    if ((h & 1023u) < (uint32_t)(P_CORPSE * 1024.0f))
        simp_corpse_add(g->s, x, y, r * 0.9f, CORPSE_TTL);
    simp_add_danger(g->s, x, y, DANGER_R, DANGER_W);     /* bloody killzone */
    if (g->ev_cb) g->ev_cb(g->ev_user, slot, i, (DefBody)g->body[slot], DEF_EV_DEATH);
    simp_kill(g->s, i);
    g->kills++;
}

static void wound_roll(DefGame *g, int i, int slot) {
    /* il tank ha UN solo modello: si insanguina ma non perde arti */
    if (g->body[slot] == BT_TANK) { g->wound[slot] = DW_BLOODY; return; }
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
        if (g->hp[slot] <= 0) { die_light(g, i, slot); return; }
        int fresh = (g->wound[slot] == DW_NONE);
        if (fresh) wound_roll(g, i, slot);
        DefWound w = (DefWound)g->wound[slot];
        DefBody  body = (DefBody)g->body[slot];
        /* gore d'IMPATTO, una volta, al momento della ferita fresca: schizzi
         * sempre, + l'arto/gli arti volanti sulle mutilazioni (host). */
        if (fresh && g->ev_cb) {
            DefEvent ge = (w == DW_CRAWLING)   ? DEF_EV_WOUND_LEGS
                        : (w == DW_MAIMED_ARM) ? DEF_EV_WOUND_ARM
                                               : DEF_EV_WOUND_BLEED;
            g->ev_cb(g->ev_user, slot, i, body, ge);
        }
        /* HIT flinch + stun SOLO sul sanguinamento (o ri-colpo su uno gia'
         * ferito: "se e' gia' insanguinato, solo l'animazione hit"). Una
         * MUTILAZIONE appena inflitta (braccio/gambe) NON stunna: il suo
         * feedback e' lo swap di modello (monco/crawler) + gli arti/gib che
         * volano (host); uno stun qui congelerebbe il nuovo body a piedi fermi
         * (la walk del crawler e' guidata dalla distanza -> resterebbe al
         * frame 0). */
        int mutilated_now = fresh && (w == DW_MAIMED_ARM || w == DW_CRAWLING);
        if (g->ev_cb && !mutilated_now)
            g->ev_cb(g->ev_user, slot, i, body, DEF_EV_HIT);
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

/* Destructible structures (player barricades / base) sit at the BARRICATA tier
 * of the per-cell breakthrough cost (SIMULAZIONE.md §I.4): the base wall toll,
 * deliberately CHEAPER than the palazzo tier the terrain holes use (10x, see
 * vat_horde) so the horde prefers sfondare these over pressing indestructible
 * buildings. 1.0 = the tuning knob (raise to make barricades less attractive
 * than open detours, lower for more). Equals the default, so applying it is
 * intent, not a behaviour change. */
#define BARRICADE_WALL_TIER 1.0f

void def_struct_cell(DefGame *g, int id, int cx, int cy) {
    if (id < 0 || id >= g->nstructs) return;
    if (cx < 0 || cy < 0 || cx >= g->gw || cy >= g->gh) return;
    g->cell_struct[cy * g->gw + cx] = (int16_t)id;
    simp_set_wall(g->s, cx, cy, true);
    simp_set_wall_cost(g->s, cx, cy, BARRICADE_WALL_TIER * simp_wall_base_cost());
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

/* ---- §8 placement budget ---- */

void def_set_budget(DefGame *g, int budget) { g->budget = budget; }

void def_set_event_cb(DefGame *g, DefEventFn cb, void *user) {
    g->ev_cb = cb; g->ev_user = user;
}
int  def_budget(const DefGame *g) { return g->budget; }
int  def_spend(DefGame *g, int cost) {
    if (cost < 0 || g->budget < cost) return 0;
    g->budget -= cost;
    return 1;
}

/* ---- §8 spawn director ---- */

#define DIR_RECT_CAP 16
#define DIR_MAX_PER_FRAME 64   /* safety cap so a huge dt can't burst         */

struct DefDirector {
    DefGame *g;
    DefRect  rects[DIR_RECT_CAP];
    int      nrects;
    float    radius, base_rate, rate_ramp, wave_period;
    DefSpawnFn on_spawn; void *user;
    float    time, accum;
    int      emitted;
    uint32_t rng;
};

static inline uint32_t xs32(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x;
}

/* Body mix that toughens with the wave: tanks and obese grow, children shrink. */
static DefBody director_body(uint32_t *rng, int wave) {
    uint32_t k = xs32(rng) % 100u;
    int tank_pct = 2 + wave * 2; if (tank_pct > 15) tank_pct = 15;
    if (k < (uint32_t)tank_pct) return BT_TANK;
    uint32_t r2 = xs32(rng) % 100u;
    int obese_pct = 12 + wave * 2; if (obese_pct > 30) obese_pct = 30;
    if (r2 < (uint32_t)obese_pct)            return BT_OBESE;
    if (r2 < (uint32_t)(obese_pct + 30))     return BT_MAN;
    if (r2 < (uint32_t)(obese_pct + 55))     return BT_WOMAN;
    return BT_CHILD;
}

DefDirector *def_director_create(DefGame *g, const DefDirectorCfg *cfg) {
    DefDirector *d = (DefDirector *)calloc(1, sizeof(DefDirector));
    d->g = g;
    d->nrects = cfg->nrects < DIR_RECT_CAP ? cfg->nrects : DIR_RECT_CAP;
    for (int i = 0; i < d->nrects; i++) d->rects[i] = cfg->rects[i];
    d->radius      = cfg->spawn_radius > 0.0f ? cfg->spawn_radius : 0.34f;
    d->base_rate   = cfg->base_rate;
    d->rate_ramp   = cfg->rate_ramp;
    d->wave_period = cfg->wave_period > 0.0f ? cfg->wave_period : 20.0f;
    d->on_spawn = cfg->on_spawn; d->user = cfg->user;
    d->rng = cfg->seed ? cfg->seed : 0xD17EC709u;
    return d;
}

void def_director_destroy(DefDirector *d) { free(d); }

void def_director_update(DefDirector *d, float dt) {
    if (d->nrects <= 0) return;
    d->time += dt;
    int wave = (int)(d->time / d->wave_period);
    float rate = d->base_rate + (float)wave * d->rate_ramp;
    if (rate < 0.0f) rate = 0.0f;
    d->accum += rate * dt;
    int want = (int)d->accum; d->accum -= (float)want;
    if (want > DIR_MAX_PER_FRAME) want = DIR_MAX_PER_FRAME;

    SimP *s = d->g->s;
    for (int k = 0; k < want; k++) {
        const DefRect *rc = &d->rects[xs32(&d->rng) % (uint32_t)d->nrects];
        float fx = (float)(xs32(&d->rng) % 100000u) / 100000.0f;
        float fy = (float)(xs32(&d->rng) % 100000u) / 100000.0f;
        float x = rc->x + fx * rc->w, y = rc->y + fy * rc->h;
        if (!simp_free_at(s, x, y, d->radius)) continue;   /* burst-free throttle */
        unsigned roll = xs32(&d->rng);
        DefBody body = director_body(&d->rng, wave);
        SimPHandle h = def_spawn(d->g, x, y, body);
        if (h == SIMP_HANDLE_INVALID) continue;
        d->emitted++;
        if (d->on_spawn) d->on_spawn(d->user, h, body, roll);
    }
}

int def_director_wave(const DefDirector *d) { return (int)(d->time / d->wave_period); }
int def_director_emitted(const DefDirector *d) { return d->emitted; }

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

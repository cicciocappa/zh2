/* defense.c — implementation. See defense.h and M5_DESIGN.md. */

#include "defense.h"
#include <stdlib.h>
#include <string.h>
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
/* FLAME/ACID (Blocco 2): status DoT + cone defaults. Compiled numbers for
 * now — Blocco 3 moves them into the balance table. Fire kills a default
 * walker in ~6 s of sustained burning; acid is slower but lasts longer. */
#define BURN_DPS   16.0f
#define BURN_DUR    5.0f
#define ACID_DPS   12.0f
#define ACID_DUR    8.0f
#define CONE_HALF_DEF 0.21f  /* rad (~12°, 24° total jet)                    */
#define CORPSE_CLEAR_PERIOD 1.0f /* s of sustained jetting per corpse sweep  */
/* blood-fear (CORPSE_DESIGN.md, 2026-06-25): every death stains the cell; the
 * horde's animal instinct reroutes around bloody killzones. ~4 deaths in a
 * cell saturate the cost term. Deposited where the blood decal is emitted. */
#define DANGER_R   0.7f      /* blood-fear deposit radius (m) per death       */
#define DANGER_W   0.25f     /* blood-fear added per death (saturates at 1)   */

/* §7 siege of structures — same discrete-attack model as test_siege.c */
#define STRUCT_CAP    192   /* walls + destructible turrets + siegeable props */
#define ATTACK_PERIOD 0.8f   /* s between hits while an agent presses        */
#define ATTACK_DAMAGE 5.0f   /* HP per hit                                   */
#define ATTACK_MIN_P  0.006f /* min wall_pressure to count as a real attack
                              * (gates out the tangential grazing leak)      */
/* Turrets are attacked by CONTACT (the swarm mobbing the emplacement), not by
 * flow-pressure: a lone turret cell in the open is flanked (the flow routes
 * around it, into_wall ~ 0), so the wall-siege never fires on it. Instead every
 * agent within contact range of the emplacement chips it — a turret swarmed by
 * the horde falls; one with no neighbours is safe. A turret has FEW attackers
 * (small perimeter) vs a long wall's many, so to fall at a comparable rate it
 * needs a higher per-agent DPS and a wider reach (gathers the mob, not just the
 * touching ring). Both tunable per-game (def_set_turret_contact) so HP stays the
 * per-turret toughness knob. */
#define TURRET_DPS_DEF    12.0f  /* HP/s per contacting agent (wall attacker = 6.25) */
#define TURRET_REACH_DEF  0.90f  /* agent reach beyond the emplacement half-cell (m) */

typedef struct {
    float hp, hp_max;
    int   is_core;       /* collapse = loss, not reroute */
    int   is_turret;     /* backs a destructible turret (host skips wall mesh) */
    int   collapsed;
    float debris_mass;   /* >0 = Hybrid: scatter draggable debris on collapse   */
} DefStruct;

struct DefGame {
    SimP *s;
    int   cap;
    int     *hp;        /* per slot */
    uint8_t *body;      /* per slot: DefBody */
    uint8_t *wound;     /* per slot: DefWound */
    uint8_t *status;    /* per slot: DefStatus (FLAME/ACID DoT)             */
    float   *status_t;  /* per slot: DoT time left (refreshed in the jet)   */
    float   *dot_acc;   /* per slot: fractional DoT damage accumulator      */
    float   *hheat;     /* per slot: heavy hits absorbed; fractional because a
                         * shot through semi-transparent cover (ENTITY_DESIGN
                         * axis C) lands attenuated: heat += transmit */
    DefTurret turrets[TURRET_CAP];
    int   turret_struct[TURRET_CAP];  /* backing structure id, -1 = indestructible */
    int   nturrets;
    /* fire lure (def_set_fire_lure): per-turret linger clock + applied stamp
     * (cell index + EXACT delta read back after the clamped simp_add_cost,
     * so removal restores the field bit-exactly even under overlaps). */
    float lure_w, lure_r, lure_linger;      /* w >= 0 = lure off            */
    float lure_t[TURRET_CAP];               /* linger countdown (s)         */
    uint8_t lure_on[TURRET_CAP];            /* stamp currently applied      */
    int32_t *lure_cell[TURRET_CAP];         /* lazily sized (2r+1)^2 arrays */
    float   *lure_delta[TURRET_CAP];
    int      lure_n[TURRET_CAP];
    int  *qbuf;         /* cap: acquire buffer (query_circle) */
    SimPHandle *hbuf;   /* cap: blast handle snapshot (kills reorder indices) */
    int   raybuf[MAXPIERCE];
    float rayt[MAXPIERCE];
    SimPHandle rayh[MAXPIERCE];
    int   kills, shots;
    /* §7 base & defeat */
    int       gw, gh;       /* cached nav-grid dims (decode wall_cell)       */
    DefStruct structs[STRUCT_CAP];
    int       nstructs;
    int16_t  *cell_struct;  /* gw*gh: structure id per nav cell, -1 = none   */
    int       stx0, sty0, stx1, sty1;  /* bbox of cells EVER assigned (never shrinks) */
    float    *atk_timer;    /* per slot: siege attack accumulator            */
    int16_t  *contact_tid;  /* per slot: turret being contact-attacked THIS
                             * update, -1 = none (render hook: attack anim)  */
    int       lost;         /* 1 once the core has collapsed                 */
    int       budget;       /* §8 placement budget                          */
    float     turret_dps;   /* contact damage HP/s per agent (def_set_turret_contact) */
    float     turret_reach; /* contact range beyond the emplacement half-cell (m)     */
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
    g->status   = (uint8_t *)calloc((size_t)cap, 1);
    g->status_t = (float *)calloc((size_t)cap, sizeof(float));
    g->dot_acc  = (float *)calloc((size_t)cap, sizeof(float));
    g->hheat = (float *)calloc((size_t)cap, sizeof(float));
    g->qbuf  = (int *)malloc((size_t)cap * sizeof(int));
    g->hbuf  = (SimPHandle *)malloc((size_t)cap * sizeof(SimPHandle));
    g->gw = simp_grid_w(s); g->gh = simp_grid_h(s);
    size_t ncell = (size_t)g->gw * (size_t)g->gh;
    g->cell_struct = (int16_t *)malloc(ncell * sizeof(int16_t));
    for (size_t k = 0; k < ncell; k++) g->cell_struct[k] = -1;
    g->stx0 = g->gw; g->sty0 = g->gh; g->stx1 = -1; g->sty1 = -1;   /* empty bbox */
    g->atk_timer = (float *)calloc((size_t)cap, sizeof(float));
    g->contact_tid = (int16_t *)malloc((size_t)cap * sizeof(int16_t));
    for (int k = 0; k < cap; k++) g->contact_tid[k] = -1;
    g->turret_dps = TURRET_DPS_DEF;
    g->turret_reach = TURRET_REACH_DEF;
    g->lure_w = 0.0f;                       /* lure off by default (opt-in) */
    return g;
}

void def_destroy(DefGame *g) {
    if (!g) return;
    for (int i = 0; i < TURRET_CAP; i++) { free(g->lure_cell[i]); free(g->lure_delta[i]); }
    free(g->hp); free(g->body); free(g->wound); free(g->hheat); free(g->qbuf);
    free(g->status); free(g->status_t); free(g->dot_acc);
    free(g->hbuf);
    free(g->cell_struct); free(g->atk_timer); free(g->contact_tid);
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
    g->status[slot] = DST_NONE;
    g->status_t[slot] = 0.0f;
    g->dot_acc[slot] = 0.0f;
    g->hheat[slot] = 0;
    return simp_handle_of(g->s, i);
}

int def_add_turret(DefGame *g, const DefTurret *t) {
    if (g->nturrets >= TURRET_CAP) return -1;
    int id = g->nturrets++;
    g->turrets[id] = *t;
    g->turret_struct[id] = -1;          /* indestructible until bound */
    if (g->turrets[id].sweep_dir == 0) g->turrets[id].sweep_dir = 1;
    /* kind <-> heavy sync (Blocco 2): legacy callers set only heavy, new ones
     * set only kind — either way both fields come out coherent. */
    if (g->turrets[id].kind == TUR_LIGHT && g->turrets[id].heavy)
        g->turrets[id].kind = TUR_HEAVY;
    g->turrets[id].heavy = (g->turrets[id].kind == TUR_HEAVY);
    if (g->turrets[id].cone_half <= 0.0f) g->turrets[id].cone_half = CONE_HALF_DEF;
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

/* Light (non-heavy) damage of `dmg` HP to the live agent at index i / slot.
 * Shared by turret shots (dmg = damage*transmit) and explosions/fall damage
 * (direct HP). Handles death (corpse + danger + DEATH event), the one-shot
 * wound roll and the HIT/WOUND render events. No-op on dmg <= 0. */
static void hurt_light(DefGame *g, int i, int slot, int dmg) {
    if (dmg <= 0) return;                 /* cover soaked it: no wound/flinch */
    g->hp[slot] -= dmg;
    if (g->hp[slot] <= 0) { die_light(g, i, slot); return; }
    int fresh = (g->wound[slot] == DW_NONE);
    if (fresh) wound_roll(g, i, slot);
    DefWound w = (DefWound)g->wound[slot];
    DefBody  body = (DefBody)g->body[slot];
    /* gore d'IMPATTO, una volta, al momento della ferita fresca. */
    if (fresh && g->ev_cb) {
        DefEvent ge = (w == DW_CRAWLING)   ? DEF_EV_WOUND_LEGS
                    : (w == DW_MAIMED_ARM) ? DEF_EV_WOUND_ARM
                                           : DEF_EV_WOUND_BLEED;
        g->ev_cb(g->ev_user, slot, i, body, ge);
    }
    /* HIT flinch + stun SOLO sul sanguinamento (vedi apply_damage per il perché
     * una mutilazione appena inflitta non stunna). */
    int mutilated_now = fresh && (w == DW_MAIMED_ARM || w == DW_CRAWLING);
    if (g->ev_cb && !mutilated_now)
        g->ev_cb(g->ev_user, slot, i, body, DEF_EV_HIT);
}

/* Apply one shot's worth of damage to a live handle. Resolved from handle so
 * earlier kills in the same (piercing) shot don't invalidate it. transmit in
 * (0,1] scales the shot through semi-transparent cover (ENTITY_DESIGN axis C):
 * deterministic multiplier, same expected DPS as a per-bullet lottery. */
static void apply_damage(DefGame *g, SimPHandle h, const DefTurret *t,
                         float transmit) {
    int i = simp_index_of(g->s, h);
    if (i < 0) return;
    int slot = simp_slot_of(g->s, i);
    if (t->heavy) {
        /* attenuated heavy shots accumulate as fractional heat: behind a
         * fence a normal body takes ceil(1/transmit) hits to gib, a tank
         * proportionally more. transmit 1 = today's behaviour exactly. */
        g->hheat[slot] += transmit;
        if (g->body[slot] == BT_TANK) {
            if ((int)g->hheat[slot] < ENEMY[BT_TANK].heavy_hits)
                return;                        /* tank survives this hit */
        } else if (g->hheat[slot] < 1.0f) {
            return;                            /* cover soaked this one */
        }
        gib(g, i);                             /* normal, or tank's last hit */
    } else {
        /* the wound-not-stun subtlety lives in hurt_light (shared with blasts):
         * a maimed limb swaps the model + throws gore instead of flinching. */
        hurt_light(g, i, slot, (int)(t->damage * transmit + 0.5f));
    }
}

/* ---- turret update ---- */

#define TRACER_TTL 0.05f
/* min ray transmittance to acquire a target (ENTITY_DESIGN §4): the turret
 * engages through a fence (transmit ~0.7 >> this) but not through a wall (0);
 * low enough that any cover worth shooting through passes. */
#define T_ACQ 0.05f

/* Apply the elemental status to a live agent (FLAME/ACID hit). First status
 * wins (mutually exclusive, like wounds); same-type hits refresh the clock.
 * The event fires ONCE, on application — the host swaps the outfit there. */
static void apply_status(DefGame *g, int i, int slot, DefStatus st) {
    if (g->status[slot] == DST_NONE) {
        g->status[slot] = (uint8_t)st;
        g->dot_acc[slot] = 0.0f;
        if (g->ev_cb) g->ev_cb(g->ev_user, slot, i, (DefBody)g->body[slot],
                               st == DST_BURNING ? DEF_EV_IGNITE : DEF_EV_ACID);
    }
    if (g->status[slot] == (uint8_t)st)
        g->status_t[slot] = (st == DST_BURNING) ? BURN_DUR : ACID_DUR;
}

/* FLAME/ACID shot tick: every live agent inside range, within ±cone_half of
 * the barrel and with line of sight takes t->damage direct (SILENT: no
 * flinch, no wound roll — the elemental fiction replaces the impact gore)
 * plus the status. qbuf/n come from this update's acquire query (no kill in
 * between, indices still exact); handles first anyway (M3.4: the damage
 * pass kills and reorders). Capped at MAXPIERCE victims per tick. */
static void aoe_fire(DefGame *g, DefTurret *t, const int *qbuf, int n,
                     float moff) {
    SimP *s = g->s;
    const float *px = simp_px(s), *py = simp_py(s);
    int nh = 0;
    for (int k = 0; k < n && nh < MAXPIERCE; k++) {
        int i = qbuf[k];
        float dx = px[i] - t->x, dy = py[i] - t->y;
        float d = sqrtf(dx * dx + dy * dy);
        if (d < 1e-4f) continue;
        if (fabsf(wrap_pi(atan2f(dy, dx) - t->ang)) > t->cone_half) continue;
        float md = d - moff;
        if (md > 0.0f &&
            simp_ray_transmit(s, t->x + dx / d * moff, t->y + dy / d * moff,
                              dx / d, dy / d, md) < T_ACQ)
            continue;                              /* a wall shields it */
        g->rayh[nh++] = simp_handle_of(s, i);
    }
    DefStatus st = (t->kind == TUR_FLAME) ? DST_BURNING : DST_ACID;
    int dmg = (int)(t->damage + 0.5f);
    for (int k = 0; k < nh; k++) {
        int i = simp_index_of(s, g->rayh[k]);
        if (i < 0) continue;
        int slot = simp_slot_of(s, i);
        apply_status(g, i, slot, st);
        if (dmg > 0) {
            g->hp[slot] -= dmg;
            if (g->hp[slot] <= 0) die_light(g, i, slot);
        }
    }
}

/* DoT tick for every live agent with an elemental status. Downward index
 * scan: die_light swap-and-pops the tail, which a decreasing loop never
 * revisits. The outfit swap is permanent (scar); only the DoT expires. */
static void status_tick(DefGame *g, float dt) {
    SimP *s = g->s;
    for (int i = simp_count(s) - 1; i >= 0; i--) {
        int slot = simp_slot_of(s, i);
        if (g->status[slot] == DST_NONE) continue;
        g->status_t[slot] -= dt;
        float dps = (g->status[slot] == DST_BURNING) ? BURN_DPS : ACID_DPS;
        g->dot_acc[slot] += dps * dt;
        int whole = (int)g->dot_acc[slot];
        if (whole >= 1) {
            g->dot_acc[slot] -= (float)whole;
            g->hp[slot] -= whole;
            if (g->hp[slot] <= 0) { die_light(g, i, slot); continue; }
        }
        if (g->status_t[slot] <= 0.0f) g->status[slot] = DST_NONE;
    }
}

static void turret_update(DefGame *g, DefTurret *t, float dt) {
    SimP *s = g->s;
    const float *px = simp_px(s), *py = simp_py(s);
    t->fired = 0;
    if (t->tracer_ttl > 0.0f) t->tracer_ttl -= dt;

    /* muzzle offset: a DESTRUCTIBLE turret sits on its own solid cell, so a ray
     * (line of sight OR bullet) from the centre self-blocks. Start it just past
     * the emplacement cell; a free-standing turret keeps the centre (moff = 0). */
    float cs = simp_cell_size(s);
    float moff = simp_is_wall(s, (int)(t->x / cs), (int)(t->y / cs)) ? cs * 1.2f : 0.0f;

    /* acquire: nearest live agent inside range AND arc with ENOUGH line of
     * sight (transmittance >= T_ACQ, ENTITY_DESIGN §4): a target behind a
     * fence is engaged (damage scaled at fire time), one behind a full wall
     * is skipped (not merely un-hittable) — the turret holds fire or picks a
     * visible target instead. On opacity-free maps simp_ray_transmit is the
     * binary wall check of before, at the same cost. */
    float cx = (t->arc_min + t->arc_max) * 0.5f;
    float half = (t->arc_max - t->arc_min) * 0.5f;
    int n = simp_query_circle(s, t->x, t->y, t->range, g->qbuf, g->cap, 0);
    int best = -1; float bestd2 = 1e30f;
    for (int k = 0; k < n; k++) {
        int i = g->qbuf[k];
        float dx = px[i] - t->x, dy = py[i] - t->y;
        float d2 = dx * dx + dy * dy;
        if (d2 >= bestd2) continue;                /* farther than best visible */
        float bearing = atan2f(dy, dx);
        if (fabsf(wrap_pi(bearing - cx)) > half) continue;   /* outside arc */
        float d = sqrtf(d2);                       /* line of sight to target */
        if (d > 1e-4f) {
            float ndx = dx / d, ndy = dy / d, md = d - moff;
            if (md > 0.0f &&
                simp_ray_transmit(s, t->x + ndx * moff, t->y + ndy * moff,
                                  ndx, ndy, md) < T_ACQ)
                continue;                          /* a wall shields it */
        }
        bestd2 = d2; best = i;
    }

    float aim_err = 1e30f;                     /* |error| AFTER this turn step */
    if (best >= 0) {                           /* dwell: turn toward target */
        float bearing = atan2f(py[best] - t->y, px[best] - t->x);
        float maxstep = t->sweep_speed * dt;
        if (half >= 3.13f) {
            /* free mount (near-full arc, no stops): wrapped shortest path —
             * targets straddling the ±pi seam must not trigger long slews.
             * ang deliberately NOT re-wrapped (it may drift past ±pi until
             * the sweep clamp catches it): bit-identical to the pre-arc-gate
             * behaviour the M5 scenarios (test_lure) are calibrated on. */
            float diff = wrap_pi(bearing - t->ang);
            t->ang += (fabsf(diff) <= maxstep) ? diff
                      : (diff > 0 ? maxstep : -maxstep);
            aim_err = fabsf(wrap_pi(bearing - t->ang));
        } else {
            /* limited mount: chase the target's angle in the UNWRAPPED arc
             * frame (the acquire filter guarantees wrap_pi(bearing-cx) is
             * within ±half, so goal lies in [arc_min, arc_max] like t->ang) —
             * when the wrapped shortest path crosses the stops, the barrel
             * slews the long way around INSIDE the arc instead of pinning. */
            float goal = cx + wrap_pi(bearing - cx);
            float diff = goal - t->ang;
            t->ang += (fabsf(diff) <= maxstep) ? diff
                      : (diff > 0 ? maxstep : -maxstep);
            aim_err = fabsf(goal - t->ang);
        }
    } else {                                   /* sweep the arc */
        t->ang += (float)t->sweep_dir * t->sweep_speed * dt;
        if (t->ang > t->arc_max) { t->ang = t->arc_max; t->sweep_dir = -1; }
        if (t->ang < t->arc_min) { t->ang = t->arc_min; t->sweep_dir =  1; }
    }

    /* fire only with a target IN THE SIGHTS (turn-then-shoot): hold fire while
     * the barrel is still slewing, with the timer primed so the shot leaves the
     * instant alignment is reached (no waste on empty arc either).
     * aim_tol <= 0 = legacy gate-less turret (defense.h); the FLAME/ACID jet
     * gates on its own cone (a target anywhere in the jet is a hit). */
    t->fire_timer += dt;
    int aoe = (t->kind == TUR_FLAME || t->kind == TUR_ACID);
    float tol = aoe ? t->cone_half : t->aim_tol;
    if (best < 0 || (tol > 0.0f && aim_err > tol)) {
        if (t->fire_timer > t->fire_period) t->fire_timer = t->fire_period;
        return;
    }
    if (aoe) {
        /* sustained jetting time drives the periodic corpse sweep: fire and
         * acid eat the piles in the cone (GUIDA fase C) — two discs covering
         * the middle and the far end of the jet. */
        t->clear_timer += dt;
        if (t->fire_timer < t->fire_period) return;
        t->fire_timer -= t->fire_period;
        g->shots++;
        t->fired = 1;                          /* host: jet FX, no tracer */
        aoe_fire(g, t, g->qbuf, n, moff);
        if (t->clear_timer >= CORPSE_CLEAR_PERIOD) {
            t->clear_timer -= CORPSE_CLEAR_PERIOD;
            float cw = t->range * tanf(t->cone_half) + 0.6f;
            float ca = cosf(t->ang), sa = sinf(t->ang);
            simp_corpse_clear(s, t->x + ca * t->range * 0.45f,
                                 t->y + sa * t->range * 0.45f, cw * 0.6f);
            simp_corpse_clear(s, t->x + ca * t->range * 0.85f,
                                 t->y + sa * t->range * 0.85f, cw);
        }
        return;
    }
    if (t->fire_timer < t->fire_period) return;
    t->fire_timer -= t->fire_period;
    g->shots++;
    t->fired = 1;
    t->tracer_ttl = TRACER_TTL;

    int max_out = t->piercing ? MAXPIERCE : 1;
    /* fire from the muzzle (offset past the emplacement cell, computed above). */
    float mdx = cosf(t->ang), mdy = sinf(t->ang);
    float ox = t->x + mdx * moff, oy = t->y + mdy * moff;
    int nh = simp_query_ray(s, ox, oy, mdx, mdy,
                            t->range - moff, g->raybuf, g->rayt, max_out, 0);
    t->last_t = (nh > 0) ? g->rayt[0] + moff : t->range;
    if (nh <= 0) return;
    int count = t->piercing ? nh : 1;
    /* convert ALL hits to handles before any kill (M3.4 index trap) */
    for (int k = 0; k < count; k++) g->rayh[k] = simp_handle_of(s, g->raybuf[k]);
    for (int k = 0; k < count; k++) {
        /* damage scaled by the cover crossed up to THIS hit (per-hit: a
         * piercing shot loses more through each fence it punches). 1.0 on
         * opacity-free maps (the ray was already truncated at full walls). */
        float tr = simp_ray_transmit(s, ox, oy, mdx, mdy, g->rayt[k]);
        apply_damage(g, g->rayh[k], t, tr);
    }
}

/* ---- §7 base & defeat: structures + siege ---- */

int def_add_structure(DefGame *g, float hp_max, int is_core) {
    if (g->nstructs >= STRUCT_CAP) return -1;
    int id = g->nstructs++;
    g->structs[id].hp = g->structs[id].hp_max = hp_max;
    g->structs[id].is_core = is_core ? 1 : 0;
    g->structs[id].is_turret = 0;
    g->structs[id].collapsed = 0;
    g->structs[id].debris_mass = 0.0f;
    return id;
}

/* Hybrid barricade (DRAG_DESIGN.md): make structure id scatter draggable DEBRIS
 * on collapse instead of vanishing cleanly. mass <= 0 disables it. */
void def_struct_set_debris(DefGame *g, int id, float mass) {
    if (id < 0 || id >= g->nstructs) return;
    g->structs[id].debris_mass = mass > 0.0f ? mass : 0.0f;
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
    if (cx < g->stx0) g->stx0 = cx;
    if (cy < g->sty0) g->sty0 = cy;
    if (cx > g->stx1) g->stx1 = cx;
    if (cy > g->sty1) g->sty1 = cy;
    simp_set_wall(g->s, cx, cy, true);
    simp_set_wall_cost(g->s, cx, cy, BARRICADE_WALL_TIER * simp_wall_base_cost());
}

int def_struct_bbox(const DefGame *g, int *cx0, int *cy0, int *cx1, int *cy1) {
    if (g->stx1 < 0) return 0;
    if (cx0) *cx0 = g->stx0;
    if (cy0) *cy0 = g->sty0;
    if (cx1) *cx1 = g->stx1;
    if (cy1) *cy1 = g->sty1;
    return 1;
}

/* Bind turret tid to a fresh 1-cell structure at its (x,y): the cell turns into
 * a barricade-tier solid wall (def_struct_cell), so the horde presses it and the
 * siege chips its hp. Renderer reads def_turret_disabled / def_struct_is_turret.
 * Caller commits the nav (simp_terrain_commit) once after binding all turrets. */
int def_turret_make_destructible(DefGame *g, int tid, float hp) {
    if (tid < 0 || tid >= g->nturrets) return -1;
    int sid = def_add_structure(g, hp, 0);
    if (sid < 0) return -1;
    g->structs[sid].is_turret = 1;
    float cs = simp_cell_size(g->s);
    int cx = (int)(g->turrets[tid].x / cs), cy = (int)(g->turrets[tid].y / cs);
    def_struct_cell(g, sid, cx, cy);    /* raises the wall (batched commit) */
    g->turret_struct[tid] = sid;
    return sid;
}

int def_turret_disabled(const DefGame *g, int tid) {
    if (tid < 0 || tid >= g->nturrets) return 0;
    int sid = g->turret_struct[tid];
    return (sid >= 0) ? g->structs[sid].collapsed : 0;
}

int def_struct_is_turret(const DefGame *g, int id) {
    return (id >= 0 && id < g->nstructs) ? g->structs[id].is_turret : 0;
}

/* HP hit zero: free the structure's cells and recommit the nav so the horde
 * reroutes. The CORE is special — it doesn't reroute, its fall is the loss.
 * Hybrid (debris_mass > 0, DRAG_DESIGN.md): each freed cell drops a draggable
 * rubble disc, so the nav reroutes but the breach stays physically cluttered —
 * the horde shoves the debris out of the way. */
static void collapse_structure(DefGame *g, int id) {
    DefStruct *st = &g->structs[id];
    st->collapsed = 1;
    st->hp = 0.0f;
    if (st->is_core) { g->lost = 1; return; }
    int gw = g->gw, n = gw * g->gh;
    float cs = simp_cell_size(g->s), dmass = st->debris_mass;
    for (int c = 0; c < n; c++)
        if (g->cell_struct[c] == (int16_t)id) {
            int cx = c % gw, cy = c / gw;
            simp_set_wall(g->s, cx, cy, false);
            g->cell_struct[c] = -1;
            if (dmass > 0.0f)   /* ~cell-sized chunk: a single-layer rubble line the
                                 * crowd can disperse, not an overlapping boulder plug */
                simp_drag_add(g->s, (cx + 0.5f) * cs, (cy + 0.5f) * cs, cs * 0.55f, dmass);
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
        if (sid >= 0 && g->structs[sid].is_turret) sid = -1;   /* turrets: contact, not flow */
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

/* Contact siege of destructible turrets: every agent mobbing the emplacement
 * (centre within half-cell + reach) chips the turret at TURRET_DPS, regardless
 * of where it's heading. This is what lets a flanked turret in the open fall
 * when swarmed (the wall-siege, gated on flow INTO the cell, never fires there).
 * Only the count matters, so the M3.4 index trap (no kills here) doesn't bite. */
static void turret_contact_update(DefGame *g, float dt) {
    SimP *s = g->s;
    float rad = simp_cell_size(s) * 0.5f + g->turret_reach;
    /* refresh the per-slot attacker map (render hook: attack animation) */
    memset(g->contact_tid, 0xFF, (size_t)g->cap * sizeof(int16_t));
    for (int id = 0; id < g->nturrets; id++) {
        int sid = g->turret_struct[id];
        if (sid < 0 || g->structs[sid].collapsed) continue;
        int n = simp_query_circle(s, g->turrets[id].x, g->turrets[id].y,
                                  rad, g->qbuf, g->cap, 0);
        if (n <= 0) continue;
        for (int k = 0; k < n; k++)
            g->contact_tid[simp_slot_of(s, g->qbuf[k])] = (int16_t)id;
        g->structs[sid].hp -= (float)n * g->turret_dps * dt;
        if (g->structs[sid].hp <= 0.0f) collapse_structure(g, sid);
    }
}

int def_contact_turret(const DefGame *g, int slot) {
    return (slot >= 0 && slot < g->cap) ? g->contact_tid[slot] : -1;
}

void def_set_turret_contact(DefGame *g, float dps, float reach) {
    if (dps   > 0.0f) g->turret_dps   = dps;
    if (reach > 0.0f) g->turret_reach = reach;
}

/* ---- fire lure (def_set_fire_lure, defense.h) ----------------------------
 * The noise of a firing turret attracts the horde: negative user cost in a
 * disc (linear falloff), applied when the turret wakes, removed exactly when
 * it falls silent (linger elapsed) or dies. Deltas are what simp_add_cost
 * ACTUALLY changed (read back around the call): under the core clamp [-0.8,]
 * and overlapping lures, add/subtract of the nominal weight would corrupt
 * neighbouring stamps — the recorded delta always restores bit-exactly. */
#define LURE_EPS 0.02f          /* skip rim cells with |w| below this */

static void lure_apply(DefGame *g, int id) {
    SimP *s = g->s;
    float cs = simp_cell_size(s);
    float tx = g->turrets[id].x, ty = g->turrets[id].y, R = g->lure_r;
    int cr = (int)ceilf(R / cs);
    int cx0 = (int)(tx / cs) - cr, cx1 = (int)(tx / cs) + cr;
    int cy0 = (int)(ty / cs) - cr, cy1 = (int)(ty / cs) + cr;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= g->gw) cx1 = g->gw - 1;
    if (cy1 >= g->gh) cy1 = g->gh - 1;
    if (!g->lure_cell[id]) {
        int cap = (2 * cr + 1) * (2 * cr + 1);
        g->lure_cell[id]  = (int32_t *)malloc((size_t)cap * sizeof(int32_t));
        g->lure_delta[id] = (float *)malloc((size_t)cap * sizeof(float));
    }
    const float *uc = simp_user_cost(s);
    int n = 0;
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++) {
            float dx = (cx + 0.5f) * cs - tx, dy = (cy + 0.5f) * cs - ty;
            float d = sqrtf(dx * dx + dy * dy);
            if (d >= R) continue;
            /* linear cone: monotonic toward the centre, so the cheapest line
             * HUGS the emplacement (a flat plateau lets routes cross the disc
             * anywhere and they only graze; measured in test_lure). Full
             * game weight (-0.8 = the cost_user floor) is what makes the
             * Dijkstra pay the detour. */
            float w = g->lure_w * (1.0f - d / R);
            if (w > -LURE_EPS) continue;
            int idx = cy * g->gw + cx;
            float before = uc[idx];
            simp_add_cost(s, cx, cy, w);
            float delta = uc[idx] - before;
            if (delta == 0.0f) continue;             /* clamped away: nothing to undo */
            g->lure_cell[id][n] = idx;
            g->lure_delta[id][n] = delta;
            n++;
        }
    g->lure_n[id] = n;
    g->lure_on[id] = 1;
}

static void lure_remove(DefGame *g, int id) {
    SimP *s = g->s;
    for (int k = 0; k < g->lure_n[id]; k++) {
        int idx = g->lure_cell[id][k];
        simp_add_cost(s, idx % g->gw, idx / g->gw, -g->lure_delta[id][k]);
    }
    g->lure_n[id] = 0;
    g->lure_on[id] = 0;
}

static void lure_update(DefGame *g, float dt) {
    if (g->lure_w >= 0.0f) return;
    for (int id = 0; id < g->nturrets; id++) {
        int firing = g->turrets[id].fired && !def_turret_disabled(g, id);
        if (firing) g->lure_t[id] = g->lure_linger;
        else if (g->lure_t[id] > 0.0f) g->lure_t[id] -= dt;
        int want = g->lure_t[id] > 0.0f && !def_turret_disabled(g, id);
        if (want && !g->lure_on[id])       lure_apply(g, id);
        else if (!want && g->lure_on[id])  lure_remove(g, id);
    }
}

void def_set_fire_lure(DefGame *g, float weight, float radius, float linger_s) {
    if (weight >= 0.0f) {                    /* off: also undo active stamps */
        for (int id = 0; id < g->nturrets; id++)
            if (g->lure_on[id]) lure_remove(g, id);
        g->lure_w = 0.0f;
        return;
    }
    g->lure_w = weight;
    g->lure_r = radius > 0.0f ? radius : 8.0f;
    g->lure_linger = linger_s > 0.0f ? linger_s : 2.5f;
}

int   def_struct_count(const DefGame *g) { return g->nstructs; }
int   def_struct_cap(void) { return STRUCT_CAP; }

/* ---- undo teardown (PREP_UI_DESIGN §6) ------------------------------------
 * LIFO-only: ONLY the LAST structure/turret can be removed, so no live id is
 * ever invalidated (nothing references ids >= count). Clean teardown for the
 * PREP undo stack: cells freed WITHOUT debris, no core/loss logic. The caller
 * batches simp_terrain_commit (one reroute per undo pop). Return 1 on
 * success, 0 if the id is not the last or the entity is not removable
 * (core, turret-backing structure, destructible-bound or luring turret). */
static void struct_free_cells(DefGame *g, int id) {
    int gw = g->gw, n = gw * g->gh;
    for (int c = 0; c < n; c++)
        if (g->cell_struct[c] == (int16_t)id) {
            simp_set_wall(g->s, c % gw, c / gw, false);  /* mirrors opacity too */
            g->cell_struct[c] = -1;
        }
}

int def_remove_structure(DefGame *g, int id) {
    if (id < 0 || id != g->nstructs - 1) return 0;
    DefStruct *st = &g->structs[id];
    if (st->is_core || st->is_turret) return 0;
    struct_free_cells(g, id);
    g->nstructs--;
    return 1;
}

int def_remove_turret(DefGame *g, int tid) {
    if (tid < 0 || tid != g->nturrets - 1) return 0;
    if (g->turret_struct[tid] >= 0) return 0;  /* bound: def_remove_turret_bound */
    if (g->lure_on[tid]) return 0;             /* PREP-only API: never fired   */
    g->nturrets--;
    return 1;
}

int def_remove_turret_bound(DefGame *g, int tid) {
    if (tid < 0 || tid != g->nturrets - 1) return 0;
    int sid = g->turret_struct[tid];
    if (sid < 0 || sid != g->nstructs - 1) return 0;   /* LIFO on BOTH tables */
    if (g->lure_on[tid]) return 0;             /* PREP-only API: never fired   */
    struct_free_cells(g, sid);                 /* the emplacement wall cell    */
    g->nstructs--;
    g->turret_struct[tid] = -1;
    g->nturrets--;
    return 1;
}
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
void def_struct_damage(DefGame *g, int id, float dmg) {
    if (id < 0 || id >= g->nstructs || g->structs[id].collapsed) return;
    if (dmg <= 0.0f) return;
    g->structs[id].hp -= dmg;
    if (g->structs[id].hp <= 0.0f) collapse_structure(g, id);
}
/* Direct light damage to a live agent (fall damage, future scripted hazards).
 * Handle-resolved so it survives reordering between the step and the call. */
void def_damage_agent(DefGame *g, SimPHandle h, float dmg) {
    if (dmg <= 0.0f) return;
    int i = simp_index_of(g->s, h);
    if (i < 0) return;
    hurt_light(g, i, simp_slot_of(g->s, i), (int)(dmg + 0.5f));
}

/* Dismember an agent outright (no HP check): the explosive-death path — GIB
 * event (renderer bursts the body + drops a blood pool, no corpse), nav gore,
 * kill. Used for a lethal fall landing (EXPLOSION_DESIGN §3.4). */
void def_gib_agent(DefGame *g, SimPHandle h) {
    int i = simp_index_of(g->s, h);
    if (i < 0) return;
    gib(g, i);
}

/* Explosion primitive (EXPLOSION_DESIGN.md §3): ONE geometry drives both damage
 * and physics via the same linear falloff D(d) = dmg*(1 - d/r). Order:
 *   1. agents  — falloff damage (dead -> corpses a later blast can sweep), then
 *      a radial impulse + vertical kick on the survivors;
 *   2. structs — D(d_min) HP to every structure with a cell in range (walls,
 *      barricades, turrets, siegeable props: one path); collapse -> reroute;
 *   3. corpses — vaporised, nav pile fields zeroed.
 * Deterministic (no RNG beyond the existing per-slot wound/corpse hashes). The
 * host layers the fiction (FX, scorch, prop archetypes) on top. */
void def_blast(DefGame *g, float x, float y, float r, float dmg,
               float strength, float up_ratio) {
    SimP *s = g->s;
    if (r <= 0.0f) return;
    const float *px = simp_px(s), *py = simp_py(s);

    /* 1. agents — snapshot handles first: killing an agent swap-pops another
     *    into its index, so raw indices from the query go stale mid-loop. */
    int n = simp_query_circle(s, x, y, r, g->qbuf, g->cap, 0);
    for (int k = 0; k < n; k++) g->hbuf[k] = simp_handle_of(s, g->qbuf[k]);
    for (int k = 0; k < n; k++) {
        int i = simp_index_of(s, g->hbuf[k]);
        if (i < 0) continue;                       /* killed earlier this blast */
        float dx = px[i] - x, dy = py[i] - y;
        float d = sqrtf(dx * dx + dy * dy);
        float dd = dmg * (1.0f - d / r);
        if (dd <= 0.0f) continue;
        hurt_light(g, i, simp_slot_of(s, i), (int)(dd + 0.5f));
    }
    if (strength > 0.0f)
        simp_apply_impulse_ex(s, x, y, r, strength, up_ratio);

    /* 2. structures — collect the min blast distance per structure BEFORE
     *    applying any damage (a collapse frees cells / recommits the nav, which
     *    would mutate cell_struct mid-scan). Cell-center distance approximates
     *    d_min; touched via a sentinel of -1. */
    float cs = simp_cell_size(s);
    float mind[STRUCT_CAP];
    for (int j = 0; j < g->nstructs; j++) mind[j] = -1.0f;
    int cx0 = (int)((x - r) / cs), cy0 = (int)((y - r) / cs);
    int cx1 = (int)((x + r) / cs), cy1 = (int)((y + r) / cs);
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= g->gw) cx1 = g->gw - 1;
    if (cy1 >= g->gh) cy1 = g->gh - 1;
    for (int cy = cy0; cy <= cy1; cy++)
        for (int cx = cx0; cx <= cx1; cx++) {
            int sid = g->cell_struct[cy * g->gw + cx];
            if (sid < 0 || g->structs[sid].collapsed) continue;
            float ddx = (cx + 0.5f) * cs - x, ddy = (cy + 0.5f) * cs - y;
            float d = sqrtf(ddx * ddx + ddy * ddy);
            if (d > r) continue;
            if (mind[sid] < 0.0f || d < mind[sid]) mind[sid] = d;
        }
    for (int j = 0; j < g->nstructs; j++)
        if (mind[j] >= 0.0f) {
            float dd = dmg * (1.0f - mind[j] / r);
            if (dd > 0.0f) def_struct_damage(g, j, dd);
        }

    /* 3. corpses — fire/blast vaporises the pile (physical discs + nav fields). */
    simp_corpse_clear(s, x, y, r);
}

int def_lost(const DefGame *g) { return g->lost; }

void def_update(DefGame *g, float dt) {
    for (int id = 0; id < g->nturrets; id++) {
        if (def_turret_disabled(g, id)) { g->turrets[id].fired = 0; continue; }
        turret_update(g, &g->turrets[id], dt);
    }
    status_tick(g, dt);          /* elemental DoT (FLAME/ACID, Blocco 2) */
    siege_update(g, dt);
    turret_contact_update(g, dt);
    lure_update(g, dt);          /* after turret_update: reads fresh 'fired' */
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
    float    start_delay;      /* s of silence before the first spawn        */
    int      pool;             /* total to emit; 0 = unlimited (legacy)      */
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
    d->start_delay = cfg->start_delay > 0.0f ? cfg->start_delay : 0.0f;
    d->pool = cfg->pool > 0 ? cfg->pool : 0;
    d->rng = cfg->seed ? cfg->seed : 0xD17EC709u;
    return d;
}

void def_director_destroy(DefDirector *d) { free(d); }

void def_director_update(DefDirector *d, float dt) {
    if (d->nrects <= 0) return;
    d->time += dt;
    if (d->time < d->start_delay) return;             /* scripted silence     */
    if (d->pool > 0 && d->emitted >= d->pool) return; /* finite pool drained  */
    float t = d->time - d->start_delay;               /* waves ramp from wake */
    int wave = (int)(t / d->wave_period);
    float rate = d->base_rate + (float)wave * d->rate_ramp;
    if (rate < 0.0f) rate = 0.0f;
    d->accum += rate * dt;
    int want = (int)d->accum; d->accum -= (float)want;
    if (want > DIR_MAX_PER_FRAME) want = DIR_MAX_PER_FRAME;
    if (d->pool > 0 && d->emitted + want > d->pool) want = d->pool - d->emitted;

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

int def_director_wave(const DefDirector *d) {
    float t = d->time - d->start_delay;
    return t > 0.0f ? (int)(t / d->wave_period) : 0;
}
int def_director_emitted(const DefDirector *d) { return d->emitted; }
int def_director_pool(const DefDirector *d)    { return d->pool; }
int def_director_done(const DefDirector *d) {
    return d->pool > 0 && d->emitted >= d->pool;
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
const uint8_t *def_status(const DefGame *g){ return g->status; }

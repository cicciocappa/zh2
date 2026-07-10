/* bio.c — biomass economy (BIOMASS_DESIGN.md). See bio.h. */
#include "bio.h"

/* §6 defaults: cost / cap per item, in BioItem order. */
static const float DEF_COST[BIO_ITEM_COUNT] = { 25, 35, 30, 30, 40, 60, 100 };
static const int   DEF_CAP [BIO_ITEM_COUNT] = {  3,  3,  3,  3,  3,  3,   3 };
#define DEF_K_CONV     0.9f
#define COST_FLOOR_K   0.5f   /* floor = 0.5 * cost_base (§7)            */
#define CAP_CEIL_ADD   3      /* ceiling = cap_base + 3 (§7)             */

static int valid(int item) { return item >= 0 && item < BIO_ITEM_COUNT; }

void bio_init(Bio *b) {
    b->tank = 0.0f;
    b->output = BIO_AMMO_LIGHT;
    b->wasted = 0.0f;
    b->k_conv = DEF_K_CONV;
    b->lv_cost = b->lv_cap = 0;
    b->produced = 0; b->produced_user = 0;
    for (int i = 0; i < BIO_ITEM_COUNT; i++) {
        b->store[i] = 0;
        b->cost[i] = b->cost_base[i] = DEF_COST[i];
        b->cap[i]  = b->cap_base[i]  = DEF_CAP[i];
    }
}

void bio_set_output(Bio *b, int item) { if (valid(item)) b->output = item; }
int  bio_output(const Bio *b)         { return b->output; }

void bio_add(Bio *b, float biomass) {
    if (biomass <= 0.0f) return;
    int o = b->output;
    if (b->store[o] >= b->cap[o]) { b->wasted += biomass; return; }  /* §4 */
    b->tank += biomass;
    while (b->store[o] < b->cap[o] && b->tank >= b->cost[o]) {
        b->tank -= b->cost[o];
        b->store[o]++;
        if (b->produced) b->produced(b->produced_user, o);
    }
}

int bio_take(Bio *b, int item) {
    if (!valid(item) || b->store[item] <= 0) return 0;
    b->store[item]--;
    return 1;
}

int   bio_count(const Bio *b, int item) { return valid(item) ? b->store[item] : 0; }
float bio_cost(const Bio *b, int item)  { return valid(item) ? b->cost[item] : 0.0f; }
int   bio_cap(const Bio *b, int item)   { return valid(item) ? b->cap[item] : 0; }
int   bio_full(const Bio *b)  { return b->store[b->output] >= b->cap[b->output]; }
float bio_tank(const Bio *b)   { return b->tank; }
float bio_wasted(const Bio *b) { return b->wasted; }

void bio_set_cost(Bio *b, int item, float cost) {
    if (!valid(item) || cost <= 0.0f) return;
    b->cost[item] = b->cost_base[item] = cost;
}
void bio_set_cap(Bio *b, int item, int cap) {
    if (!valid(item) || cap < 0) return;
    b->cap[item] = b->cap_base[item] = cap;
    if (b->store[item] > cap) b->store[item] = cap;
}
void bio_set_store(Bio *b, int item, int n) {
    if (!valid(item)) return;
    if (n < 0) n = 0;
    if (n > b->cap[item]) n = b->cap[item];
    b->store[item] = n;
}
void bio_set_k_conv(Bio *b, float k) { if (k > 0.0f && k < 1.0f) b->k_conv = k; }
void bio_set_produced_cb(Bio *b, BioProducedFn fn, void *user) {
    b->produced = fn; b->produced_user = user;
}

/* §7: a branch is upgradable while at least one item is off its bound —
 * apply clamps per item, report whether anything moved. */
int bio_can_upgrade_costs(const Bio *b) {
    for (int i = 0; i < BIO_ITEM_COUNT; i++)
        if (b->cost[i] > COST_FLOOR_K * b->cost_base[i]) return 1;
    return 0;
}
int bio_can_upgrade_caps(const Bio *b) {
    for (int i = 0; i < BIO_ITEM_COUNT; i++)
        if (b->cap[i] < b->cap_base[i] + CAP_CEIL_ADD) return 1;
    return 0;
}
int bio_upgrade_costs(Bio *b) {
    int moved = 0;
    for (int i = 0; i < BIO_ITEM_COUNT; i++) {
        float floor_c = COST_FLOOR_K * b->cost_base[i];
        float c = b->cost[i] * b->k_conv;
        if (c < floor_c) c = floor_c;
        if (c < b->cost[i]) { b->cost[i] = c; moved = 1; }
    }
    if (moved) b->lv_cost++;
    return moved;
}
int bio_upgrade_caps(Bio *b) {
    int moved = 0;
    for (int i = 0; i < BIO_ITEM_COUNT; i++)
        if (b->cap[i] < b->cap_base[i] + CAP_CEIL_ADD) { b->cap[i]++; moved = 1; }
    if (moved) b->lv_cap++;
    return moved;
}

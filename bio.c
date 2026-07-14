/* bio.c — biomass economy, v2 (BIOMASS_DESIGN.md). See bio.h. */
#include "bio.h"

void bio_init(Bio *b, float start, float cap) {
    b->cap = (cap > 0.0f) ? cap : BIO_CAP_DEFAULT;
    b->tank = (start > 0.0f) ? start : 0.0f;
    if (b->tank > b->cap) b->tank = b->cap;
    b->wasted = 0.0f;
}

float bio_add(Bio *b, float biomass) {
    if (biomass <= 0.0f) return 0.0f;
    float room = b->cap - b->tank;
    if (room < 0.0f) room = 0.0f;
    float taken = (biomass < room) ? biomass : room;
    float lost  = biomass - taken;                     /* §3: over the cap */
    b->tank += taken;
    b->wasted += lost;
    return lost;
}

int bio_take(Bio *b, float cost) {
    if (cost <= 0.0f) return 1;                        /* free action */
    if (b->tank < cost) return 0;
    b->tank -= cost;
    return 1;
}

float bio_tank(const Bio *b)   { return b->tank; }
float bio_cap(const Bio *b)    { return b->cap; }
float bio_wasted(const Bio *b) { return b->wasted; }
int   bio_full(const Bio *b)   { return b->tank >= b->cap; }

void bio_set_cap(Bio *b, float cap) {
    if (cap <= 0.0f) return;
    b->cap = cap;
    if (b->tank > cap) b->tank = cap;
}

/* test_bio.c — headless verification of the biomass economy v2
 * (BIOMASS_DESIGN §10).
 *
 * (1) add accumulates and clamps to the cap, returning the EXACT waste;
 * (2) take debits and refuses when the tank is short (tank untouched);
 * (3) cap knob (debrief upgrade) raises the ceiling / clamps the tank down;
 * (4) determinism: same call sequence => same state.
 */
#include "bio.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)
#define FEQ(a, b) (fabsf((a) - (b)) < 1e-4f)

int main(void) {
    /* (1) kills accrue; the cap is the buffer, over it biomass is WASTED */
    Bio b; bio_init(&b, 0.0f, 100.0f);
    CHECK(FEQ(bio_tank(&b), 0.0f), "empty start");
    CHECK(FEQ(bio_cap(&b), 100.0f), "cap from init");
    CHECK(!bio_full(&b), "not full at start");
    for (int k = 0; k < 24; k++) CHECK(bio_add(&b, 1.0f) == 0.0f, "no waste below cap");
    CHECK(FEQ(bio_tank(&b), 24.0f), "tank accrues (got %.3f)", (double)bio_tank(&b));
    CHECK(FEQ(bio_add(&b, 8.0f), 0.0f), "a tank kill fits");
    CHECK(FEQ(bio_tank(&b), 32.0f), "tank after tank kill");

    bio_add(&b, 60.0f);                                 /* 92 */
    CHECK(FEQ(bio_tank(&b), 92.0f), "tank at 92 (got %.3f)", (double)bio_tank(&b));
    CHECK(!bio_full(&b), "92/100 not full");
    float lost = bio_add(&b, 20.0f);                    /* 8 fit, 12 spill */
    CHECK(FEQ(lost, 12.0f), "exact waste returned (got %.3f)", (double)lost);
    CHECK(FEQ(bio_tank(&b), 100.0f), "tank clamped to cap");
    CHECK(bio_full(&b), "full flag at cap");
    CHECK(FEQ(bio_wasted(&b), 12.0f), "waste accounted");
    CHECK(FEQ(bio_add(&b, 5.0f), 5.0f), "full tank: whole kill wasted");
    CHECK(FEQ(bio_tank(&b), 100.0f), "waste: tank untouched");
    CHECK(FEQ(bio_wasted(&b), 17.0f), "waste accumulates");
    CHECK(FEQ(bio_add(&b, -3.0f), 0.0f), "negative credit ignored");
    CHECK(FEQ(bio_tank(&b), 100.0f), "negative credit: tank untouched");

    /* (2) spending: exact debit, refusal below cost, free actions */
    CHECK(bio_take(&b, 40.0f) == 1, "mortar shell paid");
    CHECK(FEQ(bio_tank(&b), 60.0f), "tank debited exactly");
    CHECK(bio_take(&b, 60.0f) == 1, "spend down to zero");
    CHECK(FEQ(bio_tank(&b), 0.0f), "tank zeroed at exact cost");
    CHECK(bio_take(&b, 25.0f) == 0, "refuses on an empty tank");
    CHECK(FEQ(bio_tank(&b), 0.0f), "refusal leaves the tank untouched");
    bio_add(&b, 10.0f);
    CHECK(bio_take(&b, 10.5f) == 0, "refuses when short by half a unit");
    CHECK(FEQ(bio_tank(&b), 10.0f), "short refusal: tank untouched");
    CHECK(bio_take(&b, 0.0f) == 1, "free action (adjust_cost 0) always passes");
    CHECK(FEQ(bio_tank(&b), 10.0f), "free action costs nothing");
    /* repair channel (§4): the host doses min(rate*dt, tank, hp missing) */
    CHECK(bio_take(&b, 4.0f) == 1 && bio_take(&b, 6.0f) == 1, "channel drains in sips");
    CHECK(FEQ(bio_tank(&b), 0.0f), "channel bottoms out at zero");

    /* (3) cap: the debrief upgrade raises it, a lower cap clamps the tank */
    Bio c; bio_init(&c, 700.0f, 0.0f);                  /* cap 0 -> default   */
    CHECK(FEQ(bio_cap(&c), BIO_CAP_DEFAULT), "cap 0 -> default 500");
    CHECK(FEQ(bio_tank(&c), 500.0f), "start clamped to cap");
    CHECK(FEQ(bio_wasted(&c), 0.0f), "the initial clamp is NOT waste");
    bio_set_cap(&c, 600.0f);
    CHECK(FEQ(bio_cap(&c), 600.0f) && FEQ(bio_tank(&c), 500.0f), "cap raised, tank kept");
    CHECK(!bio_full(&c), "room again after the upgrade");
    bio_set_cap(&c, 300.0f);
    CHECK(FEQ(bio_tank(&c), 300.0f), "lower cap clamps the tank down");
    CHECK(FEQ(bio_wasted(&c), 0.0f), "clamping down is NOT waste either");
    bio_set_cap(&c, -5.0f);
    CHECK(FEQ(bio_cap(&c), 300.0f), "invalid cap ignored");

    /* (4) determinism: same sequence twice, compare whole state */
    Bio d1, d2;
    for (int run = 0; run < 2; run++) {
        Bio *d = run ? &d2 : &d1;
        memset(d, 0, sizeof *d);        /* padding bytes too (memcmp below) */
        bio_init(d, 0.0f, 500.0f);
        unsigned rng = 0xB10Bu;
        for (int i = 0; i < 5000; i++) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            unsigned op = rng % 16u;
            if (op < 10)       bio_add(d, (float)(rng % 9u) + 1.0f);
            else if (op < 14)  bio_take(d, (float)(rng % 45u));
            else               bio_set_cap(d, 300.0f + (float)(rng % 400u));
        }
    }
    CHECK(memcmp(&d1, &d2, sizeof d1) == 0, "determinism: same sequence, same state");

    if (fails == 0) printf("test_bio: OK\n");
    else            printf("test_bio: %d FAILURES\n", fails);
    return fails ? 1 : 0;
}

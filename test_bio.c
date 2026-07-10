/* test_bio.c — headless verification of the biomass economy (BIOMASS_DESIGN §11).
 *
 * (1) yields: kills accrue in the tank, threshold -> item, tank debited exactly;
 * (2) waste: store at cap + kill -> tank untouched, biomass lost, HUD flag up;
 * (3) output switch mid-progress: tank persists, new item pops at ITS cost;
 * (4) take: consumes one, refuses at zero;
 * (5) upgrades: cost branch obeys k_conv and stops at the floor, cap branch
 *     steps to the ceiling; can_upgrade_* mirror the bounds;
 * (6) determinism: same call sequence => same state.
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

static int produced_n = 0, produced_last = -1;
static void on_produced(void *user, int item) {
    (void)user; produced_n++; produced_last = item;
}

int main(void) {
    /* (1) yields -> threshold -> item, exact debit */
    Bio b; bio_init(&b);
    bio_set_produced_cb(&b, on_produced, NULL);
    CHECK(bio_output(&b) == BIO_AMMO_LIGHT, "default output");
    CHECK(FEQ(bio_cost(&b, BIO_AMMO_LIGHT), 25.0f), "default cost 25");
    for (int k = 0; k < 24; k++) bio_add(&b, 1.0f);        /* 24 walker kills */
    CHECK(bio_count(&b, BIO_AMMO_LIGHT) == 0, "below threshold: no item yet");
    CHECK(FEQ(bio_tank(&b), 24.0f), "tank accrues (got %.3f)", (double)bio_tank(&b));
    bio_add(&b, 8.0f);                                     /* a tank kill */
    CHECK(bio_count(&b, BIO_AMMO_LIGHT) == 1, "threshold crossed -> 1 item");
    CHECK(FEQ(bio_tank(&b), 7.0f), "tank debited exactly (got %.3f)",
          (double)bio_tank(&b));
    CHECK(produced_n == 1 && produced_last == BIO_AMMO_LIGHT, "produced cb fired");
    bio_add(&b, 60.0f);                                    /* multi-conversion */
    CHECK(bio_count(&b, BIO_AMMO_LIGHT) == 3, "greedy conversion to 3 (cap)");
    CHECK(FEQ(bio_tank(&b), 17.0f), "tank after greedy run (got %.3f)",
          (double)bio_tank(&b));

    /* (2) waste at cap */
    CHECK(bio_full(&b), "store at cap -> full flag");
    float tank_before = bio_tank(&b), wasted_before = bio_wasted(&b);
    bio_add(&b, 5.0f);
    CHECK(FEQ(bio_tank(&b), tank_before), "waste: tank untouched");
    CHECK(FEQ(bio_wasted(&b), wasted_before + 5.0f), "waste accounted");
    CHECK(bio_count(&b, BIO_AMMO_LIGHT) == 3, "waste: store unchanged");

    /* (3) switch mid-progress: tank persists, new cost applies */
    bio_set_output(&b, BIO_MORTAR);                        /* cost 40 */
    CHECK(!bio_full(&b), "new output not full");
    CHECK(FEQ(bio_tank(&b), 17.0f), "tank persists across switch");
    bio_add(&b, 23.0f);                                    /* 17+23 = 40 */
    CHECK(bio_count(&b, BIO_MORTAR) == 1, "new item at ITS cost");
    CHECK(FEQ(bio_tank(&b), 0.0f), "tank zeroed at exact threshold");

    /* (4) take */
    CHECK(bio_take(&b, BIO_MORTAR) == 1, "take consumes");
    CHECK(bio_count(&b, BIO_MORTAR) == 0, "count after take");
    CHECK(bio_take(&b, BIO_MORTAR) == 0, "take refuses at zero");
    CHECK(bio_take(&b, -1) == 0 && bio_take(&b, BIO_ITEM_COUNT) == 0,
          "take rejects invalid items");

    /* (5) upgrades: cost branch (k_conv 0.9, floor 0.5*base) */
    Bio u; bio_init(&u);
    int steps = 0;
    while (bio_can_upgrade_costs(&u)) { CHECK(bio_upgrade_costs(&u), "cost step"); steps++;
        CHECK(steps < 100, "cost branch terminates"); if (steps >= 100) break; }
    CHECK(!bio_upgrade_costs(&u), "cost branch saturated");
    for (int i = 0; i < BIO_ITEM_COUNT; i++)
        CHECK(FEQ(u.cost[i], 0.5f * u.cost_base[i]),
              "cost floor at 0.5*base (item %d: %.3f)", i, (double)u.cost[i]);
    /* 0.9^7 = 0.478 < 0.5 <= 0.9^6 = 0.531 -> exactly 7 levels */
    CHECK(steps == 7 && u.lv_cost == 7, "cost levels = 7 (got %d)", steps);
    CHECK(FEQ(bio_cost(&u, BIO_AMMO_LIGHT), 12.5f), "light ammo floored at 12.5");
    /* cap branch (+1 per level, ceiling base+3) */
    steps = 0;
    while (bio_can_upgrade_caps(&u)) { CHECK(bio_upgrade_caps(&u), "cap step"); steps++;
        CHECK(steps < 100, "cap branch terminates"); if (steps >= 100) break; }
    CHECK(!bio_upgrade_caps(&u), "cap branch saturated");
    CHECK(steps == 3 && u.lv_cap == 3, "cap levels = 3 (got %d)", steps);
    for (int i = 0; i < BIO_ITEM_COUNT; i++)
        CHECK(bio_cap(&u, i) == u.cap_base[i] + 3, "cap ceiling at base+3");

    /* knobs: set_cost re-anchors base, set_cap clamps store, set_store clamps */
    Bio k; bio_init(&k);
    bio_set_cost(&k, BIO_REPAIR, 80.0f);
    CHECK(FEQ(k.cost_base[BIO_REPAIR], 80.0f), "set_cost re-anchors base");
    bio_set_store(&k, BIO_AMMO_ACID, 99);
    CHECK(bio_count(&k, BIO_AMMO_ACID) == 3, "set_store clamps to cap");
    bio_set_cap(&k, BIO_AMMO_ACID, 2);
    CHECK(bio_count(&k, BIO_AMMO_ACID) == 2, "set_cap clamps store down");

    /* (6) determinism: same sequence twice, compare whole state */
    Bio d1, d2;
    for (int run = 0; run < 2; run++) {
        Bio *d = run ? &d2 : &d1;
        memset(d, 0, sizeof *d);        /* padding bytes too (memcmp below) */
        bio_init(d);
        unsigned rng = 0xB10Bu;
        for (int i = 0; i < 5000; i++) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            unsigned op = rng % 16u;
            if (op < 10)      bio_add(d, (float)(rng % 9u) + 1.0f);
            else if (op < 12) bio_set_output(d, (int)(rng % BIO_ITEM_COUNT));
            else if (op < 14) bio_take(d, (int)(rng % BIO_ITEM_COUNT));
            else if (op == 14) bio_upgrade_costs(d);
            else               bio_upgrade_caps(d);
        }
    }
    d1.produced = d2.produced = NULL;   /* fn ptrs out of the memcmp (both NULL anyway) */
    CHECK(memcmp(&d1, &d2, sizeof d1) == 0, "determinism: same sequence, same state");

    if (fails == 0) printf("test_bio: OK\n");
    else            printf("test_bio: %d FAILURES\n", fails);
    return fails ? 1 : 0;
}

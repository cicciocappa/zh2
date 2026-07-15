/* test_balance.c — balance table + cfg parser (balance.h).
 *
 * Verifies: (1) defaults are deterministic and match the historical compiled
 * numbers spot-checked against def_tuning_defaults; (2) a synthetic cfg
 * overrides exactly the named keys, tolerates comments/whitespace, and counts
 * malformed/unknown lines without dying; (3) save -> load roundtrips to a
 * bit-identical struct; (4) a missing file leaves the struct untouched;
 * (5) the SHIPPED assets/balance.cfg parses with zero bad lines (it must
 * never rot out of sync with the key table).
 */
#include "balance.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

#define TMP "test_balance_tmp.cfg"

int main(void) {
    /* 1) deterministic defaults + spot checks */
    Balance a, b;
    balance_defaults(&a);
    balance_defaults(&b);
    CHECK(memcmp(&a, &b, sizeof a) == 0, "defaults not deterministic");
    DefTuning t;
    def_tuning_defaults(&t);
    CHECK(memcmp(&a.def, &t, sizeof t) == 0, "embedded DefTuning != defense defaults");
    CHECK(a.def.enemy[BT_TANK].hp_max == 600, "tank hp default");
    CHECK(a.tur[TUR_LIGHT].cost == 100 && a.tur[TUR_ACID].mag == 25, "turret defaults");
    CHECK(a.mortar.blast_damage == 180.0f && a.budget == 1000, "host defaults");

    /* 2) synthetic override file */
    FILE *f = fopen(TMP, "wb");
    fprintf(f,
        "# comment line\n"
        "\n"
        "enemy.man.hp = 140   # trailing comment\n"
        "  turret.light.damage=55.5\n"
        "director.tank_cap = 40\n"
        "no.such.key = 3\n"
        "garbage line without equals\n"
        "mortar.cost = abc\n");
    fclose(f);
    int bad = -1;
    int n = balance_load(&b, TMP, &bad);
    CHECK(n == 3, "applied %d keys, want 3", n);
    CHECK(bad == 3, "bad %d, want 3 (unknown+junk+non-numeric)", bad);
    CHECK(b.def.enemy[BT_MAN].hp_max == 140, "int override");
    CHECK(b.tur[TUR_LIGHT].damage == 55.5f, "float override");
    CHECK(b.def.mix_tank_cap == 40, "director mix override");
    CHECK(b.mortar.cost == a.mortar.cost, "bad value must not clobber");
    CHECK(b.def.enemy[BT_MAN].radius == a.def.enemy[BT_MAN].radius,
          "untouched key changed");

    /* 3) save -> load roundtrip is bit-identical */
    CHECK(balance_save(&b, TMP) == 0, "save failed");
    Balance c;
    balance_defaults(&c);
    n = balance_load(&c, TMP, &bad);
    CHECK(n > 0 && bad == 0, "roundtrip load: n=%d bad=%d", n, bad);
    CHECK(memcmp(&b, &c, sizeof b) == 0, "save->load not bit-identical");
    remove(TMP);

    /* 4) missing file: -1, struct untouched */
    Balance d;
    balance_defaults(&d);
    n = balance_load(&d, "no_such_file.cfg", &bad);
    CHECK(n == -1, "missing file must return -1");
    CHECK(memcmp(&d, &a, sizeof a) == 0, "missing file must not touch struct");

    /* 5) the shipped file parses clean (values are free to drift: tuning) */
    Balance s;
    balance_defaults(&s);
    n = balance_load(&s, "assets/balance.cfg", &bad);
    CHECK(n > 0, "assets/balance.cfg missing or empty (n=%d)", n);
    CHECK(bad == 0, "assets/balance.cfg has %d bad lines", bad);

    if (fails) { printf("test_balance: %d FAILURES\n", fails); return 1; }
    printf("test_balance: OK (defaults, parser %d chiavi nel file, roundtrip)\n", n);
    return 0;
}

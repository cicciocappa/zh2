/* test_anim.c — headless verification of the one-shot envelope pool. */
#include "anim.h"
#include <stdio.h>
#include <math.h>

static int fails = 0;
#define CHECK(cond, msg) do{ if(!(cond)){ \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } }while(0)
#define NEAR(a,b) (fabsf((a)-(b)) < 1e-5f)

int main(void){
    AnimSys as;
    anim_init(&as);
    CHECK(anim_count(&as) == 0, "empty");
    CHECK(anim_value(&as, ANIM_TURRET_RECOIL, 3) == 0.0f, "unfired = 0");

    /* fire, decay, expire */
    anim_fire(&as, ANIM_TURRET_RECOIL, 3, 0.2f);
    CHECK(anim_count(&as) == 1, "one channel");
    CHECK(NEAR(anim_value(&as, ANIM_TURRET_RECOIL, 3), 1.0f), "fresh = 1");
    anim_update(&as, 0.1f);
    CHECK(NEAR(anim_value(&as, ANIM_TURRET_RECOIL, 3), 0.5f), "half = 0.5");
    anim_update(&as, 0.1f);
    CHECK(anim_count(&as) == 0, "expired freed");
    CHECK(anim_value(&as, ANIM_TURRET_RECOIL, 3) == 0.0f, "expired = 0");

    /* restart resets the clock; ids independent */
    anim_fire(&as, ANIM_TURRET_RECOIL, 1, 1.0f);
    anim_fire(&as, ANIM_TURRET_RECOIL, 2, 1.0f);
    anim_update(&as, 0.5f);
    anim_fire(&as, ANIM_TURRET_RECOIL, 1, 1.0f);       /* restart id 1 */
    CHECK(NEAR(anim_value(&as, ANIM_TURRET_RECOIL, 1), 1.0f), "restarted");
    CHECK(NEAR(anim_value(&as, ANIM_TURRET_RECOIL, 2), 0.5f), "id 2 untouched");
    CHECK(anim_count(&as) == 2, "no duplicate channel");

    /* zero/negative duration is a no-op */
    anim_fire(&as, ANIM_TURRET_RECOIL, 9, 0.0f);
    CHECK(anim_count(&as) == 2, "dur 0 ignored");

    /* pool overflow recycles the channel closest to expiry */
    anim_init(&as);
    for (int i = 0; i < ANIM_MAX_CH; i++)
        anim_fire(&as, ANIM_TURRET_RECOIL, i, 1.0f + 0.001f * (float)i);
    anim_update(&as, 0.9995f);          /* id 0 is now closest to expiry */
    anim_fire(&as, ANIM_TURRET_RECOIL, 9999, 1.0f);
    CHECK(anim_count(&as) == ANIM_MAX_CH, "pool stays full");
    CHECK(anim_value(&as, ANIM_TURRET_RECOIL, 0) == 0.0f, "victim recycled");
    CHECK(NEAR(anim_value(&as, ANIM_TURRET_RECOIL, 9999), 1.0f), "newcomer live");
    CHECK(anim_value(&as, ANIM_TURRET_RECOIL, 1) > 0.0f, "others survive");

    if (fails){ printf("test_anim: %d FAILED\n", fails); return 1; }
    printf("test_anim: OK\n");
    return 0;
}

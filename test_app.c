/* test_app.c — headless verification of the application flow (app.c):
 * campaign parse, full state walk (title -> menu -> settings -> new game ->
 * briefing gate -> prep -> assault -> win/lose -> debrief -> next/retry),
 * continue from saved progress, save/load roundtrip, campaign completion.
 * Pure logic, no SDL. Part of `make test`.
 */
#include "app.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
#define CHECK(cond, msg) do{ if(!(cond)){ \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, msg); fails++; } }while(0)

static const char *CAMP = "test_campaign.tmp";
static const char *PROG = "test_progress.tmp";

static void write_campaign(void){
    FILE *f = fopen(CAMP, "w");
    fprintf(f,
        "# test campaign\n"
        "level scenes/arena4.scn\n"
        "name  Perimetro\n"
        "survive 90\n"
        "core 800\n"
        "brief Prima riga.\n"
        "brief Seconda riga.\n"
        "voice assets/brief0.wav\n"        /* future key: must be ignored */
        "\n"
        "level scenes/arena_weak.scn\n"
        "name  Sezione debole\n"
        "survive 120\n"
        "\n"
        "level scenes/arena_breach.scn\n"
        "name  Breccia\n"
        "survive 150\n"
        "core 1200\n"
        "brief Ultimo livello.\n");
    fclose(f);
}

int main(void){
    write_campaign();
    App a;

    /* ---- campaign parse ---- */
    app_init(&a);
    CHECK(a.state == APP_TITLE, "init -> TITLE");
    CHECK(app_campaign_load(&a, CAMP) == 0, "campaign load");
    CHECK(a.nlevels == 3, "3 levels");
    CHECK(strcmp(a.level[0].scene, "scenes/arena4.scn") == 0, "scene path");
    CHECK(strcmp(a.level[0].name, "Perimetro") == 0, "level name");
    CHECK(a.level[0].survive_s == 90.0f, "survive parsed");
    CHECK(a.level[0].core_hp == 800.0f, "core parsed");
    CHECK(strcmp(a.level[0].brief, "Prima riga.\nSeconda riga.") == 0,
          "brief lines joined");
    CHECK(a.level[1].core_hp == 0.0f, "core defaults to 0");
    CHECK(a.level[1].survive_s == 120.0f, "survive level 2");
    CHECK(app_campaign_load(&a, "does_not_exist.tmp") == -1, "missing file");
    app_campaign_load(&a, CAMP);            /* reload after the failed one */

    /* ---- title -> menu; CONTINUE disabled with no progress ---- */
    CHECK(app_input(&a, APP_IN_CONFIRM) == APP_ACT_NONE, "title confirm");
    CHECK(a.state == APP_MENU, "-> MENU");
    CHECK(app_menu_enabled(&a, APP_MENU_CONTINUE) == 0, "continue disabled");
    CHECK(a.menu_idx == APP_MENU_NEW, "cursor on NEW");
    app_input(&a, APP_IN_DOWN);              /* skips disabled CONTINUE */
    CHECK(a.menu_idx == APP_MENU_SETTINGS, "down skips continue");
    app_input(&a, APP_IN_UP);
    CHECK(a.menu_idx == APP_MENU_NEW, "up skips continue back");

    /* ---- settings: adjust + persist on exit ---- */
    a.menu_idx = APP_MENU_SETTINGS;
    app_input(&a, APP_IN_CONFIRM);
    CHECK(a.state == APP_SETTINGS && a.set_idx == APP_SET_SFX, "-> SETTINGS");
    int v0 = a.vol_sfx;
    CHECK(app_input(&a, APP_IN_RIGHT) == APP_ACT_APPLY_SETTINGS, "vol+ applies");
    CHECK(a.vol_sfx == v0 + 1, "sfx bumped");
    for (int k = 0; k < 20; k++) app_input(&a, APP_IN_RIGHT);
    CHECK(a.vol_sfx == 10, "sfx clamped at 10");
    CHECK(app_input(&a, APP_IN_RIGHT) == APP_ACT_NONE, "no-op at clamp");
    app_input(&a, APP_IN_DOWN);              /* music row */
    for (int k = 0; k < 20; k++) app_input(&a, APP_IN_LEFT);
    CHECK(a.vol_music == 0, "music clamped at 0");
    CHECK(app_input(&a, APP_IN_BACK) == APP_ACT_SAVE, "settings exit saves");
    CHECK(a.state == APP_MENU, "back to MENU");

    /* ---- new game -> briefing gate -> prep -> assault ---- */
    a.menu_idx = APP_MENU_NEW;
    CHECK(app_input(&a, APP_IN_CONFIRM) == APP_ACT_LOAD_LEVEL, "new game loads");
    CHECK(a.state == APP_BRIEFING && a.cur == 0 && !a.level_ready, "-> BRIEFING 0");
    CHECK(app_input(&a, APP_IN_CONFIRM) == APP_ACT_NONE, "confirm gated");
    CHECK(a.state == APP_BRIEFING, "still briefing");
    app_level_ready(&a);
    CHECK(a.level_ready, "ready");
    CHECK(app_input(&a, APP_IN_CONFIRM) == APP_ACT_START_PREP, "-> prep");
    CHECK(a.state == APP_PREP, "PREP");
    CHECK(app_input(&a, APP_IN_CONFIRM) == APP_ACT_START_ASSAULT, "-> assault");
    CHECK(a.state == APP_ASSAULT, "ASSAULT");

    /* ---- win -> debrief -> next level; progress persisted ---- */
    CHECK(app_report_result(&a, 1) == APP_ACT_SAVE, "win saves");
    CHECK(a.state == APP_DEBRIEF && a.won && a.unlocked == 1, "unlocked 1");
    CHECK(app_input(&a, APP_IN_CONFIRM) == APP_ACT_LOAD_LEVEL, "debrief -> load");
    CHECK(a.state == APP_BRIEFING && a.cur == 1, "briefing level 1");

    /* ---- lose -> retry same level ---- */
    app_level_ready(&a);
    app_input(&a, APP_IN_CONFIRM);           /* prep    */
    app_input(&a, APP_IN_CONFIRM);           /* assault */
    CHECK(app_report_result(&a, 0) == APP_ACT_NONE, "lose: nothing to save");
    CHECK(a.state == APP_DEBRIEF && !a.won && a.unlocked == 1, "lost, unlocked stays");
    CHECK(app_input(&a, APP_IN_CONFIRM) == APP_ACT_LOAD_LEVEL, "retry loads");
    CHECK(a.cur == 1, "same level retried");

    /* ---- abort to menu from prep/assault; briefing back ---- */
    app_level_ready(&a);
    app_input(&a, APP_IN_CONFIRM);           /* prep */
    CHECK(app_input(&a, APP_IN_BACK) == APP_ACT_NONE && a.state == APP_MENU,
          "prep ESC -> menu");

    /* ---- save/load roundtrip + continue ---- */
    CHECK(app_save_progress(&a, PROG) == 0, "save ok");
    App b;
    app_init(&b);
    app_campaign_load(&b, CAMP);
    CHECK(app_load_progress(&b, PROG) == 0, "load ok");
    CHECK(b.unlocked == 1 && b.vol_sfx == 10 && b.vol_music == 0, "roundtrip");
    CHECK(app_load_progress(&b, "no_such_progress.tmp") == -1, "missing = -1");
    app_input(&b, APP_IN_CONFIRM);           /* title -> menu */
    CHECK(app_menu_enabled(&b, APP_MENU_CONTINUE), "continue enabled");
    b.menu_idx = APP_MENU_CONTINUE;
    CHECK(app_input(&b, APP_IN_CONFIRM) == APP_ACT_LOAD_LEVEL, "continue loads");
    CHECK(b.cur == 1, "continue at unlocked level");

    /* ---- campaign completion: win the last level -> menu ---- */
    b.cur = b.nlevels - 1; b.state = APP_ASSAULT;
    CHECK(app_report_result(&b, 1) == APP_ACT_SAVE, "last win still saves vols");
    CHECK(b.unlocked == 1, "unlocked does not overflow");
    CHECK(app_input(&b, APP_IN_CONFIRM) == APP_ACT_NONE, "no next level");
    CHECK(b.state == APP_MENU && b.campaign_done, "campaign done -> menu");

    /* ---- exit ---- */
    b.menu_idx = APP_MENU_EXIT;
    CHECK(app_input(&b, APP_IN_CONFIRM) == APP_ACT_QUIT, "exit quits");
    CHECK(b.state == APP_QUIT, "QUIT state");

    remove(CAMP); remove(PROG);
    if (fails){ printf("test_app: %d FAILED\n", fails); return 1; }
    printf("test_app: OK\n");
    return 0;
}

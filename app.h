/* app.h — game application flow (GAME_APP_DESIGN.md): title -> menu ->
 * briefing -> prep -> assault -> debrief, campaign progression + saved
 * progress. PURE logic, zero deps beyond libc: the host (vat_horde built
 * with -DGAME_SHELL) feeds abstract inputs and EXECUTES the returned
 * actions (load level, start phases, save, quit). Headless: test_app.c.
 */
#ifndef APP_H
#define APP_H

#ifdef __cplusplus
extern "C" {
#endif

enum { APP_MAX_LEVELS = 16, APP_BRIEF_MAX = 1024 };

typedef enum {
    APP_TITLE, APP_MENU, APP_SETTINGS, APP_BRIEFING,
    APP_DEPLOY,   /* heli delivers the base container (skippable cutscene)   */
    APP_PREP, APP_ASSAULT,
    APP_EXTRACT,  /* won: heli picks the container back up (skippable)       */
    APP_DEBRIEF, APP_QUIT
} AppState;

/* device-agnostic inputs: the host maps keys (arrows/WASD, ENTER, ESC). */
typedef enum {
    APP_IN_UP, APP_IN_DOWN, APP_IN_LEFT, APP_IN_RIGHT,
    APP_IN_CONFIRM, APP_IN_BACK
} AppInput;

/* actions the HOST must perform after app_input / app_report_result. */
typedef enum {
    APP_ACT_NONE = 0,
    APP_ACT_LOAD_LEVEL,     /* load level[cur].scene, then app_level_ready */
    APP_ACT_START_DEPLOY,   /* play the delivery cutscene (or skip: host
                               feeds APP_IN_CONFIRM when done/no heli)     */
    APP_ACT_START_PREP,     /* enter fortification phase (sim runs, no waves) */
    APP_ACT_START_ASSAULT,  /* unleash the directors                        */
    APP_ACT_APPLY_SETTINGS, /* volumes changed: push to audio               */
    APP_ACT_SAVE,           /* progress/settings changed: save_progress     */
    APP_ACT_QUIT
} AppAction;

typedef struct {
    char  scene[128];
    char  name[64];
    char  brief[APP_BRIEF_MAX];  /* newline-joined briefing text            */
    float survive_s;             /* WIN: survive this long (v1, default 90) */
    float core_hp;               /* >0: host builds an is_core structure at
                                    the goal centroid; collapse = LOSE.
                                    Placeholder for GAME_PLAN fase A `lz`.  */
} AppLevel;

/* menu rows (APP_MENU). CONTINUE is disabled while unlocked == 0. */
enum { APP_MENU_NEW, APP_MENU_CONTINUE, APP_MENU_SETTINGS, APP_MENU_EXIT,
       APP_MENU_COUNT };
/* settings rows (APP_SETTINGS). */
enum { APP_SET_SFX, APP_SET_MUSIC, APP_SET_FULLSCREEN, APP_SET_BACK,
       APP_SET_COUNT };

typedef struct {
    AppLevel level[APP_MAX_LEVELS]; int nlevels;
    AppState state;
    int cur;            /* level being briefed / played                     */
    int unlocked;       /* highest level reached (persisted)                */
    int menu_idx;       /* APP_MENU_* selection                             */
    int set_idx;        /* APP_SET_* selection                              */
    int vol_sfx, vol_music;   /* 0..10 (persisted)                          */
    int fullscreen;     /* 0/1 (persisted): the host applies it on
                         * APP_ACT_APPLY_SETTINGS and at startup            */
    int level_ready;    /* BRIEFING: host finished loading                  */
    int won;            /* DEBRIEF: outcome of the last assault             */
    int campaign_done;  /* beat the last level                              */
} App;

void app_init(App *a);                            /* -> APP_TITLE, defaults */
int  app_campaign_load(App *a, const char *path); /* 0 = ok (>=1 level)     */
int  app_load_progress(App *a, const char *path); /* 0 = ok; missing file
                                                     is fine (returns -1,
                                                     state untouched)       */
int  app_save_progress(const App *a, const char *path);   /* 0 = ok        */

AppAction app_input(App *a, AppInput in);
void      app_level_ready(App *a);        /* host: level load finished      */
AppAction app_report_result(App *a, int won);  /* host: assault ended       */

/* HUD helpers (host draws the menus from these). */
const char *app_menu_label(int idx);
int         app_menu_enabled(const App *a, int idx);
const AppLevel *app_cur_level(const App *a);   /* NULL if no campaign       */

#ifdef __cplusplus
}
#endif
#endif /* APP_H */

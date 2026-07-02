/* app.c — application flow state machine (GAME_APP_DESIGN.md §1-§2).
 * Pure logic: no SDL, no GL, no audio. The host executes returned actions.
 */
#include "app.h"
#include <stdio.h>
#include <string.h>

void app_init(App *a){
    memset(a, 0, sizeof *a);
    a->state = APP_TITLE;
    a->vol_sfx = 7; a->vol_music = 5;
}

/* ---- campaign file --------------------------------------------------- */

/* first token of the line + rest (trimmed); returns 0 on blank/comment. */
static int split_kv(char *line, char **key, char **rest){
    char *p = line;
    while (*p==' '||*p=='\t') p++;
    if (*p=='#'||*p=='\n'||*p=='\0') return 0;
    *key = p;
    while (*p && *p!=' ' && *p!='\t' && *p!='\n') p++;
    if (*p) *p++ = '\0';
    while (*p==' '||*p=='\t') p++;
    char *end = p + strlen(p);
    while (end>p && (end[-1]=='\n'||end[-1]=='\r'||end[-1]==' ')) *--end='\0';
    *rest = p;
    return 1;
}

int app_campaign_load(App *a, const char *path){
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    a->nlevels = 0;
    AppLevel *L = NULL;
    char line[512];
    while (fgets(line, sizeof line, f)){
        char *k, *v;
        if (!split_kv(line, &k, &v)) continue;
        if (strcmp(k, "level") == 0){
            if (a->nlevels >= APP_MAX_LEVELS) break;
            L = &a->level[a->nlevels++];
            memset(L, 0, sizeof *L);
            snprintf(L->scene, sizeof L->scene, "%s", v);
            L->survive_s = 90.0f;
        } else if (!L) {
            continue;                      /* keys before the first `level` */
        } else if (strcmp(k, "name") == 0){
            snprintf(L->name, sizeof L->name, "%s", v);
        } else if (strcmp(k, "survive") == 0){
            sscanf(v, "%f", &L->survive_s);
        } else if (strcmp(k, "core") == 0){
            sscanf(v, "%f", &L->core_hp);
        } else if (strcmp(k, "brief") == 0){
            size_t used = strlen(L->brief);
            snprintf(L->brief + used, sizeof L->brief - used, "%s%s",
                     used ? "\n" : "", v);
        }
        /* future keys (`voice`, `video`) fall through silently: the format
         * is forward-compatible by construction (GAME_APP_DESIGN §2). */
    }
    fclose(f);
    return a->nlevels > 0 ? 0 : -1;
}

/* ---- progress file ---------------------------------------------------- */

int app_load_progress(App *a, const char *path){
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[128];
    while (fgets(line, sizeof line, f)){
        char *k, *v;
        if (!split_kv(line, &k, &v)) continue;
        int x = 0;
        if (sscanf(v, "%d", &x) != 1) continue;
        if      (strcmp(k, "unlocked")  == 0) a->unlocked  = x;
        else if (strcmp(k, "vol_sfx")   == 0) a->vol_sfx   = x;
        else if (strcmp(k, "vol_music") == 0) a->vol_music = x;
    }
    fclose(f);
    if (a->unlocked < 0) a->unlocked = 0;
    if (a->nlevels > 0 && a->unlocked >= a->nlevels) a->unlocked = a->nlevels - 1;
    if (a->vol_sfx   < 0) a->vol_sfx   = 0;
    if (a->vol_sfx   > 10) a->vol_sfx  = 10;
    if (a->vol_music < 0) a->vol_music = 0;
    if (a->vol_music > 10) a->vol_music = 10;
    return 0;
}

int app_save_progress(const App *a, const char *path){
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# zh2 progress\nunlocked %d\nvol_sfx %d\nvol_music %d\n",
            a->unlocked, a->vol_sfx, a->vol_music);
    fclose(f);
    return 0;
}

/* ---- state machine ---------------------------------------------------- */

const char *app_menu_label(int idx){
    static const char *L[APP_MENU_COUNT] =
        { "NEW GAME", "CONTINUE", "SETTINGS", "EXIT" };
    return (idx >= 0 && idx < APP_MENU_COUNT) ? L[idx] : "";
}

int app_menu_enabled(const App *a, int idx){
    if (idx == APP_MENU_CONTINUE) return a->unlocked > 0;
    return 1;
}

const AppLevel *app_cur_level(const App *a){
    if (a->nlevels <= 0) return NULL;
    int c = a->cur;
    if (c < 0) c = 0;
    if (c >= a->nlevels) c = a->nlevels - 1;
    return &a->level[c];
}

/* move the menu cursor skipping disabled rows (wraps; menu is never
 * fully disabled: NEW GAME/EXIT always live). */
static void menu_move(App *a, int dir){
    for (int k = 0; k < APP_MENU_COUNT; k++){
        a->menu_idx = (a->menu_idx + dir + APP_MENU_COUNT) % APP_MENU_COUNT;
        if (app_menu_enabled(a, a->menu_idx)) return;
    }
}

static AppAction start_briefing(App *a, int level){
    a->cur = level;
    a->level_ready = 0;
    a->state = APP_BRIEFING;
    return APP_ACT_LOAD_LEVEL;
}

AppAction app_input(App *a, AppInput in){
    switch (a->state){
    case APP_TITLE:
        if (in == APP_IN_CONFIRM){ a->state = APP_MENU; a->menu_idx = APP_MENU_NEW; }
        return APP_ACT_NONE;

    case APP_MENU:
        if (in == APP_IN_UP)   { menu_move(a, -1); return APP_ACT_NONE; }
        if (in == APP_IN_DOWN) { menu_move(a, +1); return APP_ACT_NONE; }
        if (in == APP_IN_BACK) { a->state = APP_TITLE; return APP_ACT_NONE; }
        if (in == APP_IN_CONFIRM){
            switch (a->menu_idx){
            case APP_MENU_NEW:      a->campaign_done = 0; return start_briefing(a, 0);
            case APP_MENU_CONTINUE:
                if (!app_menu_enabled(a, APP_MENU_CONTINUE)) return APP_ACT_NONE;
                a->campaign_done = 0; return start_briefing(a, a->unlocked);
            case APP_MENU_SETTINGS: a->state = APP_SETTINGS; a->set_idx = APP_SET_SFX;
                                    return APP_ACT_NONE;
            case APP_MENU_EXIT:     a->state = APP_QUIT; return APP_ACT_QUIT;
            }
        }
        return APP_ACT_NONE;

    case APP_SETTINGS: {
        int *vol = a->set_idx == APP_SET_SFX   ? &a->vol_sfx :
                   a->set_idx == APP_SET_MUSIC ? &a->vol_music : NULL;
        if (in == APP_IN_UP)   { a->set_idx = (a->set_idx + APP_SET_COUNT - 1) % APP_SET_COUNT; return APP_ACT_NONE; }
        if (in == APP_IN_DOWN) { a->set_idx = (a->set_idx + 1) % APP_SET_COUNT; return APP_ACT_NONE; }
        if (vol && (in == APP_IN_LEFT || in == APP_IN_RIGHT)){
            int nv = *vol + (in == APP_IN_RIGHT ? 1 : -1);
            if (nv < 0) nv = 0;
            if (nv > 10) nv = 10;
            if (nv == *vol) return APP_ACT_NONE;
            *vol = nv;
            return APP_ACT_APPLY_SETTINGS;
        }
        if (in == APP_IN_BACK ||
            (in == APP_IN_CONFIRM && a->set_idx == APP_SET_BACK)){
            a->state = APP_MENU;
            return APP_ACT_SAVE;                    /* persist the volumes */
        }
        return APP_ACT_NONE;
    }

    case APP_BRIEFING:
        if (in == APP_IN_BACK){ a->state = APP_MENU; return APP_ACT_NONE; }
        if (in == APP_IN_CONFIRM && a->level_ready){
            a->state = APP_PREP;
            return APP_ACT_START_PREP;
        }
        return APP_ACT_NONE;

    case APP_PREP:
        if (in == APP_IN_BACK){ a->state = APP_MENU; return APP_ACT_NONE; }
        if (in == APP_IN_CONFIRM){
            a->state = APP_ASSAULT;
            return APP_ACT_START_ASSAULT;
        }
        return APP_ACT_NONE;

    case APP_ASSAULT:
        if (in == APP_IN_BACK){ a->state = APP_MENU; return APP_ACT_NONE; }
        return APP_ACT_NONE;                /* outcome via app_report_result */

    case APP_DEBRIEF:
        if (in == APP_IN_CONFIRM || in == APP_IN_BACK){
            if (a->won){
                if (a->cur + 1 >= a->nlevels){          /* beat the campaign */
                    a->campaign_done = 1;
                    a->state = APP_MENU; a->menu_idx = APP_MENU_NEW;
                    return APP_ACT_NONE;
                }
                return start_briefing(a, a->cur + 1);
            }
            return start_briefing(a, a->cur);           /* retry             */
        }
        return APP_ACT_NONE;

    case APP_QUIT:
        return APP_ACT_QUIT;
    }
    return APP_ACT_NONE;
}

void app_level_ready(App *a){
    if (a->state == APP_BRIEFING) a->level_ready = 1;
}

AppAction app_report_result(App *a, int won){
    if (a->state != APP_ASSAULT) return APP_ACT_NONE;
    a->state = APP_DEBRIEF;
    a->won = won;
    if (won && a->cur + 1 > a->unlocked && a->cur + 1 < a->nlevels){
        a->unlocked = a->cur + 1;
        return APP_ACT_SAVE;
    }
    return won ? APP_ACT_SAVE : APP_ACT_NONE;   /* winning the last level
                                                   still persists volumes  */
}

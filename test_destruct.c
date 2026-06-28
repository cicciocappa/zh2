/* test_destruct.c — destructible decor props (DESTRUCT_DESIGN.md).
 *
 * The trigger LOGIC (which prop breaks, when) is deterministic and tested here;
 * the FX burst is visual (verified by eye in vat_horde). Verifies, causally:
 *   1) instant shatter — a topple=0 prop in the crowd's path bursts exactly once,
 *      at the step the first agent enters its radius; state ends GONE;
 *   2) topple-then-burst — a topple>0 prop enters TOPPLING on contact and bursts
 *      round(topple/dt) steps later, not immediately;
 *   3) inert — a non-destructible prop (and one out of the crowd's path) never
 *      bursts;
 *   4) direction — the captured push heading points along the crowd's motion;
 *   5) determinism — two runs produce identical burst steps/order.
 */
#include "destruct.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GW 200
#define GH 120
#define CELL 0.5f
#define DT (1.0f / 60.0f)

/* hex-staggered crowd block; all agents share the live goal. */
static int spawn_block(SimP *s, float x0, float x1, float y0, float y1, float sp) {
    int n = 0;
    for (int row = 0; ; row++) {
        float y = y0 + row * sp * 0.866f; if (y > y1) break;
        float xoff = (row & 1) ? sp * 0.5f : 0.0f;
        for (int col = 0; ; col++) {
            float x = x0 + xoff + col * sp; if (x > x1) break;
            if (simp_spawn(s, x, y) >= 0) n++;
        }
    }
    return n;
}

/* a hand-built catalog: a wood table (instant), a metal sign (topples), and a
 * statue (inert / not destructible). */
static void make_catalog(PropCatalog *c) {
    memset(c, 0, sizeof *c);
    PropDef *d;
    d = &c->defs[c->n++]; strcpy(d->key,"table"); d->scale=1; strcpy(d->label,"Table");
    d->destructible=1; d->trigger_radius=1.2f; strcpy(d->debris,"wood"); d->topple=0.0f;
    d = &c->defs[c->n++]; strcpy(d->key,"sign");  d->scale=1; strcpy(d->label,"Sign");
    d->destructible=1; d->trigger_radius=1.2f; strcpy(d->debris,"metal"); d->topple=0.5f;
    d = &c->defs[c->n++]; strcpy(d->key,"statue"); d->scale=1; strcpy(d->label,"Statue");
    /* destructible stays 0 -> inert */
}

/* a scene with the four props: table (path), sign (path), statue (path, inert),
 * and a table OFF the crowd's path (never reached). */
static void make_scene(Scene *sc) {
    memset(sc, 0, sizeof *sc);
    sc->n_prop = 4;
    strcpy(sc->prop[0].key,"table");  sc->prop[0].x=40; sc->prop[0].y=30; sc->prop[0].rot=0;
    strcpy(sc->prop[1].key,"sign");   sc->prop[1].x=55; sc->prop[1].y=30; sc->prop[1].rot=0;
    strcpy(sc->prop[2].key,"statue"); sc->prop[2].x=70; sc->prop[2].y=30; sc->prop[2].rot=0;
    strcpy(sc->prop[3].key,"table");  sc->prop[3].x=40; sc->prop[3].y=95; sc->prop[3].rot=0;
}

/* burst log */
typedef struct { int idx[16]; int n; } BurstLog;
static int g_step;
static float g_dir[SCENE_MAX_PROP];
static int   g_burst_step[SCENE_MAX_PROP];
static void on_burst(int i, const char *debris, float x, float y, float dir, void *ud) {
    (void)debris; (void)x; (void)y;
    BurstLog *b = (BurstLog *)ud;
    if (b->n < 16) b->idx[b->n++] = i;
    g_dir[i] = dir;
    g_burst_step[i] = g_step;
}

static int run(BurstLog *log, int *topple_start_step) {
    for (int i=0;i<SCENE_MAX_PROP;i++){ g_burst_step[i]=-1; g_dir[i]=999; }
    SimP *s = simp_create(GW, GH, CELL, 8000);
    for (int x=0;x<GW;x++){ simp_set_wall(s,x,0,true); simp_set_wall(s,x,GH-1,true); }
    for (int y=0;y<GH;y++){ simp_set_wall(s,0,y,true); simp_set_wall(s,GW-1,y,true); }
    for (int y=26;y<34;y++) simp_set_goal(s, GW-6, y, true);   /* far right */
    Scene sc; make_scene(&sc);
    PropCatalog cat; make_catalog(&cat);
    Destruct d; destruct_init(&d, &sc, &cat);
    spawn_block(s, 8.0f, 22.0f, 24.0f, 36.0f, 0.62f);
    log->n = 0;
    int sign_topple_start = -1;
    for (g_step = 0; g_step < 4000; g_step++) {
        simp_step(s, DT);
        destruct_update(&d, s, &sc, &cat, DT, on_burst, log);
        if (sign_topple_start < 0 && destruct_state(&d,1) == DESTRUCT_TOPPLING)
            sign_topple_start = g_step;
        /* stop once both path-destructibles are gone */
        if (destruct_state(&d,0)==DESTRUCT_GONE && destruct_state(&d,1)==DESTRUCT_GONE)
            { /* let a few more steps run to catch any spurious extra bursts */ }
    }
    if (topple_start_step) *topple_start_step = sign_topple_start;
    /* final states */
    int ok = 1;
    ok &= (destruct_state(&d,0)==DESTRUCT_GONE);   /* table on path -> gone */
    ok &= (destruct_state(&d,1)==DESTRUCT_GONE);   /* sign on path  -> gone */
    ok &= (destruct_state(&d,2)==DESTRUCT_INERT);  /* statue        -> inert */
    ok &= (destruct_state(&d,3)==DESTRUCT_ALIVE);  /* off-path table -> never reached */
    simp_destroy(s);
    return ok;
}

int main(void) {
    int ok = 1;
    BurstLog log; int topple_start = -1;
    int states_ok = run(&log, &topple_start);

    /* 1) instant shatter: table (idx 0) bursts exactly once */
    int table_bursts = 0; for (int k=0;k<log.n;k++) if (log.idx[k]==0) table_bursts++;
    int sign_bursts  = 0; for (int k=0;k<log.n;k++) if (log.idx[k]==1) sign_bursts++;
    int statue_bursts= 0; for (int k=0;k<log.n;k++) if (log.idx[k]==2) statue_bursts++;
    int off_bursts   = 0; for (int k=0;k<log.n;k++) if (log.idx[k]==3) off_bursts++;
    printf("[1] instant: table bursts=%d (step %d), state=GONE? %s\n",
           table_bursts, g_burst_step[0], states_ok?"yes":"no");
    int t1 = (table_bursts==1);

    /* 2) topple-then-burst: sign enters TOPPLING then bursts ~0.5s later */
    int want = (int)lroundf(0.5f / DT);
    int delay = (g_burst_step[1] >= 0 && topple_start >= 0) ? g_burst_step[1]-topple_start : -999;
    printf("[2] topple: sign topple@%d burst@%d delay=%d (want ~%d), bursts=%d\n",
           topple_start, g_burst_step[1], delay, want, sign_bursts);
    int t2 = (sign_bursts==1) && (abs(delay-want)<=1);

    /* 3) inert: statue + off-path table never burst */
    printf("[3] inert: statue bursts=%d, off-path table bursts=%d (want 0,0)\n",
           statue_bursts, off_bursts);
    int t3 = (statue_bursts==0) && (off_bursts==0);

    /* 4) direction: crowd flows +x => dir ~ 0 rad */
    float deg = g_dir[0]*57.2958f;
    printf("[4] direction: table push dir = %.1f deg (crowd flows +x, want ~0)\n", deg);
    int t4 = (fabsf(deg) < 35.0f);

    /* 5) determinism: capture run1 burst steps, re-run, compare */
    int b0=g_burst_step[0], b1=g_burst_step[1];
    BurstLog log2; int ts2=-1; run(&log2, &ts2);
    int t5 = (log2.n==log.n) && (g_burst_step[0]==b0) && (g_burst_step[1]==b1);
    for (int k=0;k<log.n && t5;k++) t5 = (log2.idx[k]==log.idx[k]);
    printf("[5] determinism: run1 bursts=%d run2 bursts=%d, steps(%d,%d)=(%d,%d), match=%s\n",
           log.n, log2.n, b0, b1, g_burst_step[0], g_burst_step[1], t5?"yes":"no");

    ok = states_ok && t1 && t2 && t3 && t4 && t5;
    printf("\n%s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}

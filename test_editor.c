/* test_editor.c — headless test of the level-editor mutation logic (editor.h):
 * rect drag, polygon close, delete-at, snap, point-in-poly, plus a save/reload
 * round-trip through scene.c (EDITOR_DESIGN §6 verification (a)).
 *
 * Pure logic, no GL/SDL: exercises the same functions the host calls on input.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "scene.h"
#include "vat/editor.h"

static int fails=0;
#define CHECK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",(msg)); fails++; } }while(0)

static void base_scene(Scene *sc){
    memset(sc,0,sizeof*sc);
    sc->cell=0.5f; sc->world_w=100; sc->world_h=70;
    sc->gw=200; sc->gh=140;
}

int main(void){
    Scene sc; base_scene(&sc);
    Editor ed; ed_init(&ed);
    CHECK(ed.tool==ED_SELECT && ed.snap==1, "init defaults");

    /* --- snap to cell --- */
    CHECK(fabsf(ed_snap(&ed,&sc,12.34f)-12.5f)<1e-4f, "snap 12.34->12.5");
    ed.snap=0; CHECK(fabsf(ed_snap(&ed,&sc,12.34f)-12.34f)<1e-4f, "snap off = identity");
    ed.snap=1;

    /* --- rect drag commits a goal (normalized + snapped) --- */
    ed.tool=ED_GOAL; ed.dragging=1;
    ed.ax=40.2f; ed.ay=30.1f; ed.bx=20.0f; ed.by=10.0f;   /* reversed corners */
    CHECK(ed_commit_drag(&ed,&sc)==1, "commit goal drag");
    CHECK(sc.n_goal==1, "n_goal=1");
    CHECK(sc.goal[0].x==20.0f && sc.goal[0].y==10.0f, "goal normalized to min corner");
    CHECK(fabsf(sc.goal[0].w-20.0f)<1e-4f && fabsf(sc.goal[0].h-20.0f)<1e-4f, "goal w/h");
    CHECK(ed.dirty==1, "drag set dirty");

    /* --- degenerate drag is ignored --- */
    ed.dragging=1; ed.ax=ed.bx=50; ed.ay=ed.by=50;
    CHECK(ed_commit_drag(&ed,&sc)==0 && sc.n_goal==1, "zero-area drag ignored");

    /* --- cost rect carries the weight --- */
    ed.tool=ED_COST; ed.rect_cost=7.0f; ed.dragging=1;
    ed.ax=60; ed.ay=20; ed.bx=70; ed.by=30;
    ed_commit_drag(&ed,&sc);
    CHECK(sc.n_cost==1 && fabsf(sc.cost[0].weight-7.0f)<1e-4f, "cost rect weight");

    /* --- spawn + pack tools route to the right list --- */
    ed.tool=ED_SPAWN; ed.dragging=1; ed.ax=2; ed.ay=1; ed.bx=98; ed.by=5; ed_commit_drag(&ed,&sc);
    ed.tool=ED_PACK;  ed.dragging=1; ed.ax=10;ed.ay=10;ed.bx=14;ed.by=14; ed_commit_drag(&ed,&sc);
    CHECK(sc.n_spawn==1 && sc.n_pack==1, "spawn/pack lists");

    /* --- polygon: 4 vertices -> solid wall with height --- */
    ed.tool=ED_WALL; ed.poly_h=4.0f; ed.npoly=0;
    ed_poly_vertex(&ed,&sc,30,20); ed_poly_vertex(&ed,&sc,40,20);
    ed_poly_vertex(&ed,&sc,40,35); ed_poly_vertex(&ed,&sc,30,35);
    CHECK(ed.npoly==4, "4 verts staged");
    CHECK(ed_poly_close(&ed,&sc)==1, "poly close");
    CHECK(sc.n_poly==1 && sc.poly[0].nverts==4, "n_poly=1, 4 verts");
    CHECK(sc.poly[0].solid==true && fabsf(sc.poly[0].height-4.0f)<1e-4f, "wall solid + height");
    CHECK(ed.npoly==0, "poly buffer cleared");

    /* under-3-vertex poly is rejected --- */
    ed.npoly=0; ed_poly_vertex(&ed,&sc,1,1); ed_poly_vertex(&ed,&sc,2,2);
    CHECK(ed_poly_close(&ed,&sc)==0 && sc.n_poly==1, "2-vert poly rejected");

    /* --- point-in-poly --- */
    CHECK(ed_pt_in_poly(&sc.poly[0],35,27)==1, "inside poly");
    CHECK(ed_pt_in_poly(&sc.poly[0],10,10)==0, "outside poly");

    /* --- delete-at: hits the goal rect, then the poly --- */
    CHECK(ed_delete_at(&sc,25,15)==1 && sc.n_goal==0, "delete goal at point");
    CHECK(ed_delete_at(&sc,35,27)==1 && sc.n_poly==0, "delete poly at point");
    CHECK(ed_delete_at(&sc,5,40)==0, "delete nothing on empty spot");

    /* --- overlay builder smoke: produces some geometry, stays within bounds --- */
    { static float buf[4096*9]; ed.have_cursor=1; ed.curx=50; ed.cury=30;
      int v=ed_overlay(&sc,&ed,buf,4096);
      CHECK(v>0 && v<=4096 && v%3==0, "overlay returns triangle verts");
      (void)ED_TOOL_NAME[ed.tool]; }

    /* --- save/reload round-trip (cost/spawn/pack survive) --- */
    const char *path="/tmp/test_editor_roundtrip.scn";
    CHECK(scene_save(path,&sc)==0, "scene_save");
    Scene rl; memset(&rl,0,sizeof rl);
    CHECK(scene_load(path,&rl)==0, "scene_load");
    CHECK(rl.n_cost==sc.n_cost && rl.n_spawn==sc.n_spawn && rl.n_pack==sc.n_pack, "counts survive roundtrip");
    CHECK(fabsf(rl.cost[0].weight-7.0f)<1e-4f, "cost weight survives");
    CHECK(rl.world_w==100 && rl.world_h==70 && fabsf(rl.cell-0.5f)<1e-4f, "world/cell survive");

    if(fails){ printf("FAIL (%d)\n",fails); return 1; }
    printf("PASS\n"); return 0;
}

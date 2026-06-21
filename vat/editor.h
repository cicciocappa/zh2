/* editor.h — level editor state + logic for vat_horde (EDITOR_DESIGN fase 1).
 *
 * Pure logic, NO GL/SDL: mutates the Scene (the single source of truth, §1),
 * builds an overlay vertex buffer for the flat shader, saves. The host
 * (vat_horde) owns picking (pick_y0), GL, input events; it calls these to turn
 * cursor/clicks into Scene edits and to draw the editable overlay.
 *
 * Tools (fase 1, terrain): rect goal/spawn/cost/pack + poly wall/cost + delete.
 * Snap-to-cell on by default (G toggles). Polygons already render as extruded
 * obstacle geometry, so the overlay only draws rects + the in-progress poly +
 * the drag rect + the cursor.
 */
#ifndef EDITOR_H
#define EDITOR_H

#include "scene.h"
#include <string.h>
#include <math.h>

typedef enum { ED_SELECT=0, ED_GOAL, ED_SPAWN, ED_COST, ED_PACK,
               ED_WALL, ED_COSTPOLY, ED_PROP, ED_NTOOLS } EdTool;
static const char *ED_TOOL_NAME[ED_NTOOLS] =
    { "select", "goal", "spawn", "cost", "pack", "wall", "costpoly", "prop" };

typedef struct {
    int    active;          /* edit mode on/off */
    EdTool tool;
    int    snap;            /* snap-to-cell */
    int    dragging;        /* rect drag in progress */
    float  ax, ay, bx, by;  /* drag corners (world meters) */
    int    npoly;           /* poly-in-progress vertex count */
    float  pvx[SCENE_POLY_MAX_VERTS], pvy[SCENE_POLY_MAX_VERTS];
    float  poly_h;          /* height for new wall/cost polys */
    float  poly_cost;       /* cost weight for new cost poly */
    float  rect_cost;       /* cost weight for new cost rect */
    int    prop_idx;        /* current prop type (index into host catalog) */
    char   prop_key[SCENE_PROP_KEY_LEN];  /* catalog key the host resolved */
    float  prop_rot;        /* Y-rotation (deg) for the next prop placed */
    float  curx, cury;      /* last picked cursor (world) */
    int    have_cursor;
    int    dirty;           /* scene changed since last instantiate/save */
} Editor;

static void ed_init(Editor *e) {
    memset(e, 0, sizeof *e);
    e->tool = ED_SELECT; e->snap = 1;
    e->poly_h = 4.0f; e->poly_cost = 5.0f; e->rect_cost = 5.0f;
}

static float ed_snap(const Editor *e, const Scene *sc, float v) {
    if (!e->snap) return v;
    float c = sc->cell > 0 ? sc->cell : 0.5f;
    return roundf(v / c) * c;
}

/* append a rect to the list matching the active rect tool. weight used by cost */
static void ed_add_rect(Scene *sc, EdTool t, float x, float y, float w, float h, float weight) {
    SceneRect r = { x, y, w, h, weight };
    switch (t) {
        case ED_GOAL:  if (sc->n_goal  < SCENE_MAX_RECT) sc->goal [sc->n_goal++]  = r; break;
        case ED_SPAWN: if (sc->n_spawn < SCENE_MAX_RECT) sc->spawn[sc->n_spawn++] = r; break;
        case ED_PACK:  if (sc->n_pack  < SCENE_MAX_RECT) sc->pack [sc->n_pack++]  = r; break;
        case ED_COST:  if (sc->n_cost  < SCENE_MAX_RECT) sc->cost [sc->n_cost++]  = r; break;
        default: break;
    }
}

/* finalize a rect drag into a Scene rect (normalized, snapped, min-size gated) */
static int ed_commit_drag(Editor *e, Scene *sc) {
    e->dragging = 0;
    float x0 = ed_snap(e, sc, e->ax < e->bx ? e->ax : e->bx);
    float y0 = ed_snap(e, sc, e->ay < e->by ? e->ay : e->by);
    float x1 = ed_snap(e, sc, e->ax > e->bx ? e->ax : e->bx);
    float y1 = ed_snap(e, sc, e->ay > e->by ? e->ay : e->by);
    float w = x1 - x0, h = y1 - y0;
    if (w < sc->cell*0.5f || h < sc->cell*0.5f) return 0;   /* too small: ignore */
    ed_add_rect(sc, e->tool, x0, y0, w, h, e->rect_cost);
    e->dirty = 1; return 1;
}

/* push a vertex onto the in-progress polygon (snapped) */
static void ed_poly_vertex(Editor *e, const Scene *sc, float x, float y) {
    if (e->npoly >= SCENE_POLY_MAX_VERTS) return;
    e->pvx[e->npoly] = ed_snap(e, sc, x);
    e->pvy[e->npoly] = ed_snap(e, sc, y);
    e->npoly++;
}

/* close the in-progress polygon into a Scene poly (>=3 verts). solid for wall */
static int ed_poly_close(Editor *e, Scene *sc) {
    if (e->npoly < 3 || sc->n_poly >= SCENE_MAX_POLY) { e->npoly = 0; return 0; }
    ScenePoly *p = &sc->poly[sc->n_poly++];
    memset(p, 0, sizeof *p);
    p->nverts = e->npoly;
    for (int i = 0; i < e->npoly; i++) { p->vx[i] = e->pvx[i]; p->vy[i] = e->pvy[i]; }
    p->height = e->poly_h;
    p->solid  = (e->tool == ED_WALL);
    p->cost   = e->poly_cost;
    e->npoly = 0; e->dirty = 1; return 1;
}

/* place a pure-decor prop at (x,y) with the editor's current key + rotation.
 * Decor is FREE-FORM (GFX_DESIGN §6: buildings snap, decor doesn't) -> the raw
 * cursor position is used, ignoring the cell snap. Returns 1 if appended. */
static int ed_place_prop(Editor *e, Scene *sc, float x, float y) {
    if (sc->n_prop >= SCENE_MAX_PROP || e->prop_key[0] == '\0') return 0;
    SceneProp *p = &sc->prop[sc->n_prop++];
    memcpy(p->key, e->prop_key, SCENE_PROP_KEY_LEN);   /* same-size buffers, NUL incl. */
    p->key[SCENE_PROP_KEY_LEN - 1] = '\0';
    p->x = x; p->y = y; p->rot = e->prop_rot;
    e->dirty = 1; return 1;
}

/* index of the nearest prop within `radius` meters of (x,y), or -1 */
static int ed_prop_hit(const Scene *sc, float x, float y, float radius) {
    int best = -1; float bd2 = radius * radius;
    for (int i = 0; i < sc->n_prop; i++) {
        float dx = sc->prop[i].x - x, dy = sc->prop[i].y - y, d2 = dx*dx + dy*dy;
        if (d2 <= bd2) { bd2 = d2; best = i; }
    }
    return best;
}

/* even-odd point-in-polygon (meters) */
static int ed_pt_in_poly(const ScenePoly *p, float x, float y) {
    int in = 0;
    for (int i = 0, j = p->nverts - 1; i < p->nverts; j = i++) {
        float xi = p->vx[i], yi = p->vy[i], xj = p->vx[j], yj = p->vy[j];
        if (((yi > y) != (yj > y)) &&
            (x < (xj - xi) * (y - yi) / (yj - yi + 1e-9f) + xi)) in = !in;
    }
    return in;
}

/* helpers to delete index i from a rect array (swap-with-last keeps it simple;
 * order in a scene file is irrelevant) */
static int ed_rect_hit(const SceneRect *a, int n, float x, float y) {
    for (int i = n-1; i >= 0; i--)             /* topmost (last drawn) first */
        if (x >= a[i].x && x <= a[i].x+a[i].w && y >= a[i].y && y <= a[i].y+a[i].h) return i;
    return -1;
}
static void ed_rect_del(SceneRect *a, int *n, int i) { a[i] = a[--(*n)]; }

/* delete the first entity under (x,y): rects (any list) then polygons. The drag
 * tool's RMB / Del. Returns 1 if something was removed. */
static int ed_delete_at(Scene *sc, float x, float y) {
    int i;
    if ((i = ed_prop_hit(sc, x, y, 0.6f)) >= 0) { sc->prop[i] = sc->prop[--sc->n_prop]; return 1; }
    if ((i = ed_rect_hit(sc->goal,  sc->n_goal,  x, y)) >= 0) { ed_rect_del(sc->goal,  &sc->n_goal,  i); return 1; }
    if ((i = ed_rect_hit(sc->spawn, sc->n_spawn, x, y)) >= 0) { ed_rect_del(sc->spawn, &sc->n_spawn, i); return 1; }
    if ((i = ed_rect_hit(sc->pack,  sc->n_pack,  x, y)) >= 0) { ed_rect_del(sc->pack,  &sc->n_pack,  i); return 1; }
    if ((i = ed_rect_hit(sc->cost,  sc->n_cost,  x, y)) >= 0) { ed_rect_del(sc->cost,  &sc->n_cost,  i); return 1; }
    for (i = sc->n_poly-1; i >= 0; i--)
        if (ed_pt_in_poly(&sc->poly[i], x, y)) { sc->poly[i] = sc->poly[--sc->n_poly]; return 1; }
    return 0;
}

/* ---- overlay mesh for the flat shader (pos3, normal3=up, color3) ---------- */
/* Each filled quad = 2 triangles = 6 verts * 9 floats. Drawn just above ground. */

#define ED_OVL_Y 0.06f

static int ed_push_quad(float *buf, int v, float x, float y, float w, float h,
                        float r, float g, float b) {
    float x0=x, x1=x+w, z0=y, z1=y+h;
    float V[6][3] = {{x0,z0},{x1,z0},{x1,z1}, {x0,z0},{x1,z1},{x0,z1}};
    for (int k=0;k<6;k++){ float *o=buf+(v+k)*9;
        o[0]=V[k][0]; o[1]=ED_OVL_Y; o[2]=V[k][1];
        o[3]=0;o[4]=1;o[5]=0; o[6]=r;o[7]=g;o[8]=b; }
    return v+6;
}
/* thin bar from (x0,y0) to (x1,y1), width wm — for poly edges / crosshair */
static int ed_push_bar(float *buf, int v, float x0, float y0, float x1, float y1,
                       float wm, float r, float g, float b) {
    float dx=x1-x0, dy=y1-y0, len=sqrtf(dx*dx+dy*dy); if(len<1e-5f) return v;
    float nx=-dy/len*wm*0.5f, ny=dx/len*wm*0.5f;
    float P[6][2] = {{x0-nx,y0-ny},{x1-nx,y1-ny},{x1+nx,y1+ny},
                     {x0-nx,y0-ny},{x1+nx,y1+ny},{x0+nx,y0+ny}};
    for (int k=0;k<6;k++){ float *o=buf+(v+k)*9;
        o[0]=P[k][0]; o[1]=ED_OVL_Y; o[2]=P[k][1];
        o[3]=0;o[4]=1;o[5]=0; o[6]=r;o[7]=g;o[8]=b; }
    return v+6;
}
static int ed_push_marker(float *buf, int v, float x, float y, float s,
                          float r, float g, float b) {
    return ed_push_quad(buf, v, x-s*0.5f, y-s*0.5f, s, s, r, g, b);
}

/* build the whole editor overlay; returns vertex count (<= maxv) */
static int ed_overlay(const Scene *sc, const Editor *e, float *buf, int maxv) {
    int v = 0;
    #define ROOM (v + 6 <= maxv)
    /* committed rects, color-coded */
    for (int i=0;i<sc->n_goal  && ROOM;i++) v=ed_push_quad(buf,v,sc->goal [i].x,sc->goal [i].y,sc->goal [i].w,sc->goal [i].h, 0.20f,0.85f,0.30f);
    for (int i=0;i<sc->n_spawn && ROOM;i++) v=ed_push_quad(buf,v,sc->spawn[i].x,sc->spawn[i].y,sc->spawn[i].w,sc->spawn[i].h, 0.20f,0.70f,0.95f);
    for (int i=0;i<sc->n_pack  && ROOM;i++) v=ed_push_quad(buf,v,sc->pack [i].x,sc->pack [i].y,sc->pack [i].w,sc->pack [i].h, 0.70f,0.35f,0.95f);
    for (int i=0;i<sc->n_cost  && ROOM;i++) v=ed_push_quad(buf,v,sc->cost [i].x,sc->cost [i].y,sc->cost [i].w,sc->cost [i].h, 0.95f,0.55f,0.15f);
    /* in-progress rect drag (yellow) */
    if (e->dragging && ROOM) {
        float x0=e->ax<e->bx?e->ax:e->bx, y0=e->ay<e->by?e->ay:e->by;
        v=ed_push_quad(buf,v,x0,y0,fabsf(e->bx-e->ax),fabsf(e->by-e->ay), 0.95f,0.90f,0.20f);
    }
    /* in-progress polygon: edges + vertex markers (white), edge to cursor */
    for (int i=0;i+1<e->npoly && ROOM;i++)
        v=ed_push_bar(buf,v,e->pvx[i],e->pvy[i],e->pvx[i+1],e->pvy[i+1], 0.25f, 0.95f,0.95f,0.95f);
    if (e->npoly>0 && e->have_cursor && ROOM)
        v=ed_push_bar(buf,v,e->pvx[e->npoly-1],e->pvy[e->npoly-1],e->curx,e->cury, 0.20f, 0.6f,0.95f,0.95f);
    for (int i=0;i<e->npoly && ROOM;i++)
        v=ed_push_marker(buf,v,e->pvx[i],e->pvy[i], 0.5f, 0.95f,0.95f,0.95f);
    /* pure-decor props: a small teal marker + a facing bar (Y-rotation) */
    for (int i=0;i<sc->n_prop && ROOM;i++) {
        float px=sc->prop[i].x, py=sc->prop[i].y, a=sc->prop[i].rot*0.01745329f;
        v=ed_push_marker(buf,v,px,py, 0.4f, 0.15f,0.85f,0.80f);
        if (ROOM) v=ed_push_bar(buf,v,px,py,px+cosf(a)*0.6f,py+sinf(a)*0.6f, 0.12f, 0.15f,0.85f,0.80f);
    }
    /* cursor crosshair */
    if (e->have_cursor && ROOM) v=ed_push_marker(buf,v,e->curx,e->cury, 0.4f, 1.0f,1.0f,1.0f);
    #undef ROOM
    return v;
}

#endif /* EDITOR_H */

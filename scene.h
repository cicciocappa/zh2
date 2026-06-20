/* scene.h — VECTOR scene files for the particle core.
 *
 * A scene is a list of entities in WORLD METERS (resolution-independent),
 * rasterized onto the sim grid on instantiate. Replaces the old ASCII
 * char-grid format. Obstacles are convex polygons carrying a render HEIGHT
 * (3D extrusion) and a nav effect (impassable wall, or added Dijkstra cost):
 * the renderer extrudes them, the sim rasterizes them into walls / cost.
 *
 * The sim core stays untouched: goals/walls/cost go through simp_* as before;
 * spawner regions are returned to the caller (it owns the emitters).
 *
 * FILE FORMAT (line-oriented; '#' starts a comment; tokens space-separated;
 * all coordinates in meters):
 *
 *     cell  0.5                       # nav/collision cell size (default 0.5)
 *     world 100 70                    # world extent W x H (meters) -> grid
 *     terrain meshes/level1.glb       # render-only ground mesh (+ .zhm baked
 *                                     #   alongside); EDITOR_DESIGN §9, no sim effect
 *     set   k_density 2.5             # any SimPParams field, repeatable
 *     goal  48 66 6 3                 # rect x y w h  (goal region)
 *     spawn 2  1 96 5                 # rect x y w h  (caller-side emitter)
 *     pack  10 10 4 4                 # rect x y w h  (dormant walkers)
 *     cost  20 30 8 8 5.0             # rect x y w h weight (Dijkstra cost)
 *     poly  4.0 solid     30 20 40 20 40 35 30 35    # building (wall, h=4m)
 *     poly  0.4 cost 6.0  60 40 70 40 65 50           # hazard (cost 6, h=0.4m)
 *
 * A `poly` line is: height, then `solid` OR `cost <weight>`, then >=3 vertex
 * pairs (convex, CCW or CW both fine). Grid size is round(world/cell).
 * scene_save writes the same format back.
 */
#ifndef SCENE_H
#define SCENE_H

#include "sim_particles.h"

#define SCENE_MAX_SET        32
#define SCENE_MAX_POLY       256
#define SCENE_POLY_MAX_VERTS 16
#define SCENE_MAX_RECT       64

typedef struct {
    float vx[SCENE_POLY_MAX_VERTS], vy[SCENE_POLY_MAX_VERTS];  /* meters */
    int   nverts;
    float height;   /* render extrusion height (m) */
    bool  solid;    /* true -> impassable wall; false -> added cost */
    float cost;     /* Dijkstra weight when !solid */
} ScenePoly;

typedef struct { float x, y, w, h, weight; } SceneRect;  /* meters; weight = cost only */

typedef struct {
    float cell;                 /* meters per cell */
    float world_w, world_h;     /* world extent in meters */
    int   gw, gh;               /* derived: round(world/cell) */
    char  terrain[128];         /* render-only ground mesh path ("" = none) */
    struct { char name[24]; float value; } set[SCENE_MAX_SET];
    int   n_set;
    ScenePoly poly[SCENE_MAX_POLY];   int n_poly;
    SceneRect goal[SCENE_MAX_RECT];   int n_goal;
    SceneRect spawn[SCENE_MAX_RECT];  int n_spawn;
    SceneRect pack[SCENE_MAX_RECT];   int n_pack;
    SceneRect cost[SCENE_MAX_RECT];   int n_cost;   /* rect cost patches */
} Scene;

/* Load/save/free. Return 0 on success, negative on error (file or format). */
int  scene_load(const char *path, Scene *sc);
int  scene_save(const char *path, const Scene *sc);
void scene_free(Scene *sc);   /* no heap today; zeroes the struct */

/* Create a sim from the scene: param overrides, rasterize obstacle polygons
 * (wall/cost) and cost rects, set goals, terrain commit, then dormant packs
 * (deterministic). Spawner rects are NOT consumed: read sc->spawn. Returns
 * NULL on allocation failure. */
SimP *scene_instantiate(const Scene *sc, int max_agents);

#endif /* SCENE_H */

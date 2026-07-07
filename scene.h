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
 *     terrain assets/terrain/level1.glb       # glb#1: render-only ground mesh (+ .zhm
 *                                     #   baked alongside); EDITOR_DESIGN §9
 *     statics assets/terrain/level1_st.glb    # glb#2: render-only static-props mesh
 *                                     #   (palazzi/rocce); visual only, no sim effect
 *     set   k_density 2.5             # any SimPParams field, repeatable
 *     goal  48 66 6 3                 # rect x y w h  (goal region)
 *     spawn 2  1 96 5                 # rect x y w h  (caller-side emitter)
 *     pack  10 10 4 4                 # rect x y w h  (dormant walkers)
 *     cost  20 30 8 8 5.0             # rect x y w h weight (Dijkstra cost)
 *     poly  4.0 solid     30 20 40 20 40 35 30 35    # building (wall, h=4m)
 *     poly  0.4 cost 6.0  60 40 70 40 65 50           # hazard (cost 6, h=0.4m)
 *     wall  800 1.0  48 20 24 1.5                     # destructible: hp cost_mult x y w h
 *     turret 60 30 30 0 250                           # turret: x y [range] [heavy] [hp]
 *     prop  bench 24 30 90                            # decor: catalog key x y rot(deg)
 *     exit  2 20 4 30 12 5 400        # fase A: spawn rect + director script:
 *                                     #   x y w h rate [delay] [pool 0=inf]
 *     lz    50 35                     # helicopter LZ (one per level): host makes
 *                                     #   it the core structure + central goal
 *     mission survive 300 prep 90 budget 500   # or: mission clear 0 [prep N]…
 *     budget 500                      # standalone budget (if no mission line)
 *
 * `exit`/`lz`/`mission`/`budget` are the GAME_PLAN fase A entities (names
 * reserved by BLENDER_LEVEL §8): stored here, applied by the host via
 * mission.c — one director per exit, started only in ASSAULT; `prep 0` (or
 * omitted) = unlimited PREP, the player launches the assault.
 *
 * `wall` is a DESTRUCTIBLE structure (game-side, defense.c): the rect is
 * rasterized to nav cells sharing one HP pool (hp), each cell carrying a
 * breakthrough cost of cost_mult x the base wall toll (>1 = strong/avoided,
 * <1 = weak/preferred siege target). A BREACH is just a gap left between two
 * `wall` segments; a WEAK SECTION is a short segment with low hp + cost_mult.
 * `turret` places a fixed sweeping turret (range default 30 m, heavy != 0 =
 * gibs, hp = destructible toughness vs the swarm, 0/omitted = host default).
 * Both are STORED in the scene but applied by the host (build_world), not by
 * scene_instantiate (which, like props, ignores them).
 *
 * A `poly` line is: height, then `solid` OR `cost <weight>`, then >=3 vertex
 * pairs (convex, CCW or CW both fine). Grid size is round(world/cell).
 * A `prop` line is a PURE-DECOR instance (EDITOR_DESIGN §10 stadio 5b): a
 * catalog key (props/catalog.txt, host-side), world position and Y-rotation in
 * degrees. Render + terrain seating ONLY: no SDF, no nav — the sim core never
 * sees props (scene_instantiate ignores them). scene_save writes the format back.
 */
#ifndef SCENE_H
#define SCENE_H

#include "sim_particles.h"

#define SCENE_MAX_SET        32
#define SCENE_MAX_POLY       256
#define SCENE_POLY_MAX_VERTS 16
#define SCENE_MAX_RECT       64
#define SCENE_MAX_PROP       256
#define SCENE_PROP_KEY_LEN   24

typedef struct {
    float vx[SCENE_POLY_MAX_VERTS], vy[SCENE_POLY_MAX_VERTS];  /* meters */
    int   nverts;
    float height;   /* render extrusion height (m) */
    bool  solid;    /* true -> impassable wall; false -> added cost */
    float cost;     /* Dijkstra weight when !solid */
} ScenePoly;

typedef struct { float x, y, w, h, weight; } SceneRect;  /* meters; weight = cost only */

/* Destructible wall structure (applied by the host via defense.c, not by
 * scene_instantiate): rect of nav cells sharing one HP pool. cost_mult scales
 * the per-cell breakthrough toll (>1 strong/avoided, <1 weak/preferred). */
typedef struct { float x, y, w, h, hp, cost_mult; } SceneWall;   /* meters + hp + mult */

/* Fixed sweeping turret (applied by the host). range in m; heavy != 0 = gibs;
 * hp = destructible-emplacement toughness vs the swarm (0 = host default). */
typedef struct { float x, y, range; int heavy; float hp; } SceneTurret;

/* Pure-decor prop instance: catalog key + world position + Y-rotation (deg).
 * Render-only (no sim effect); resolved against props/catalog.txt by the host. */
typedef struct { char key[SCENE_PROP_KEY_LEN]; float x, y, rot; } SceneProp;

/* Scripted exit (GAME_PLAN fase A): a spawn rect PLUS the script of its own
 * director — rate (agents/s), delay (s of silence after assault start), pool
 * (total agents; 0 = unlimited). Applied by the host (mission.c), like walls. */
typedef struct { float x, y, w, h, rate, delay; int pool; } SceneExit;

/* Mission declaration (fase A). kind NONE = legacy demo scene (no mission
 * machine; the host keeps its old behavior). prep_s 0 = unlimited PREP (the
 * player launches the assault); budget < 0 = not declared (host default). */
typedef enum { SCENE_MISSION_NONE = 0, SCENE_MISSION_SURVIVE,
               SCENE_MISSION_CLEAR } SceneMissionKind;
typedef struct {
    SceneMissionKind kind;
    float survive_s;            /* SURVIVE: win after this long in ASSAULT */
    float prep_s;               /* 0 = unlimited PREP                      */
    float budget;               /* placement budget; < 0 = not declared    */
} SceneMission;

typedef struct {
    float cell;                 /* meters per cell */
    float world_w, world_h;     /* world extent in meters */
    int   gw, gh;               /* derived: round(world/cell) */
    char  terrain[128];         /* render-only ground mesh path, glb#1 ("" = none) */
    char  statics[128];         /* render-only static-props mesh, glb#2 ("" = none) */
    struct { char name[24]; float value; } set[SCENE_MAX_SET];
    int   n_set;
    ScenePoly poly[SCENE_MAX_POLY];   int n_poly;
    SceneRect goal[SCENE_MAX_RECT];   int n_goal;
    SceneRect spawn[SCENE_MAX_RECT];  int n_spawn;
    SceneRect pack[SCENE_MAX_RECT];   int n_pack;
    SceneRect cost[SCENE_MAX_RECT];   int n_cost;   /* rect cost patches */
    SceneWall wall[SCENE_MAX_RECT];   int n_wall;   /* destructible structures (host-side) */
    SceneTurret turret[SCENE_MAX_RECT]; int n_turret; /* fixed turrets (host-side) */
    SceneProp prop[SCENE_MAX_PROP];   int n_prop;   /* pure-decor instances (render-only) */
    SceneExit exits[SCENE_MAX_RECT];  int n_exit;   /* scripted exits (host-side, fase A) */
    SceneMission mission;             /* kind NONE = legacy demo scene      */
    float lz_x, lz_y, lz_yaw; int has_lz; /* base LZ: pos + yaw(deg) (host: container core + goal) */
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

/* Scanline-rasterize a simple polygon (world meters, even-odd rule, at most
 * SCENE_POLY_MAX_VERTS verts): cb(ud, cx, cy) for every grid cell whose CENTER
 * falls inside. The same kernel scene_instantiate uses for poly entities,
 * exposed for host-side footprints (prop_world.c, ENTITY_DESIGN §8.5). */
void scene_raster_cells(const float *vx, const float *vy, int nverts,
                        float cell, int gw, int gh,
                        void (*cb)(void *ud, int cx, int cy), void *ud);

#endif /* SCENE_H */

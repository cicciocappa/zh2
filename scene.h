/* scene.h — ASCII scene files for the particle core.
 *
 * Starting configurations (terrain, goals, spawners, dormant packs, param
 * overrides, user-cost rects) loaded from a plain-text file instead of being
 * painted by hand every time. Used by the sandbox (CLI arg + save/reload)
 * and by headless tests. No dependencies beyond libc; the sim core stays
 * untouched — spawner cells are returned to the caller, which owns them.
 *
 * FILE FORMAT (line-oriented; '#' starts a comment before the map):
 *
 *     cell 0.5              # cell size in meters (default 0.5)
 *     set k_density 2.5     # any SimPParams field by name, repeatable
 *     cost 50 1 110 40 5.0  # simp_add_cost over cell rect x0 y0 x1 y1, w
 *     map                   # grid follows: one row per line, one char/cell
 *     ###############
 *     #..S...#...G..#
 *
 * Map characters (case-insensitive):
 *     #  wall          .  (or space) floor
 *     G  goal          S  spawner cell (caller-side emitter)
 *     P  dormant pack cell (filled with sleeping walkers on instantiate)
 *
 * Grid size is taken from the map block itself (longest row x row count).
 * Rows shorter than the longest are padded with floor. scene_save writes
 * the same format back (painted user cost is NOT saved — only the rect
 * directives of hand-written files express cost).
 */
#ifndef SCENE_H
#define SCENE_H

#include "sim_particles.h"

#define SCENE_MAX_SET  32
#define SCENE_MAX_COST 64

typedef struct {
    int gw, gh;
    float cell;
    uint8_t *wall, *goal, *spawn, *pack;            /* gw*gh each */
    struct { char name[24]; float value; } set[SCENE_MAX_SET];
    int n_set;
    struct { int x0, y0, x1, y1; float w; } cost[SCENE_MAX_COST];
    int n_cost;
} Scene;

/* Load/save/free. Return 0 on success, negative on error (file or format). */
int  scene_load(const char *path, Scene *sc);
int  scene_save(const char *path, const Scene *sc);
void scene_free(Scene *sc);

/* Allocate an empty gw x gh scene (all floor), for building one in code. */
int  scene_alloc(Scene *sc, int gw, int gh, float cell);

/* Create a sim from the scene: param overrides, walls, goals, cost rects,
 * terrain commit, then dormant packs (deterministic: same scene -> same
 * spawn positions). Spawner cells are NOT consumed here: read sc->spawn.
 * Returns NULL on allocation failure or scene/sim size mismatch. */
SimP *scene_instantiate(const Scene *sc, int max_agents);

#endif /* SCENE_H */

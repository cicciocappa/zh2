/* terrain.h — render-only heightmap sampling (.zhm).
 *
 * A `.zhm` is a baked elevation grid over the level's world AABB. It is PURE
 * RENDER: the simulation stays 2D planar on z=0 (zero sim cost). The height is
 * used only to *seat* what the sim moves in-plane — sprite anchors, structures,
 * shadows, the visual apex of ballistic flight — so the horde reads as "climbing
 * onto the sidewalk" / "emerging from the subway stairs" (EDITOR_DESIGN.md §9).
 *
 * Source: gfx/terrain_bake.py raycasts a Blender ground mesh; the runtime loads
 * the `.zhm` and samples terrain_z(x,y) bilinearly. This module is dependency-
 * free (no SDL/GL, no libm): the render host links it next to its math.
 *
 * FILE FORMAT (little-endian; follows the .zspr convention):
 *
 *     "ZHM1" | "ZHM2"               magic, 4 bytes
 *     u32:  w h                     sample grid dimensions
 *     f32:  origin_x origin_y       world meters of sample (0,0)
 *     f32:  px_per_m                samples per meter (render res, e.g. 4)
 *     w*h f32                       Z heights, row-major: z[j*w + i] is the
 *                                   height at world (origin_x + i/px_per_m,
 *                                   origin_y + j/px_per_m). i along +x, j +y.
 *     w*h u8   (ZHM2 only)          HOLE mask: 1 = impassable static footprint
 *                                   (building/rock, EDITOR_DESIGN.md §9). The Z
 *                                   under a hole is meaningless (back-filled for
 *                                   a smooth render edge); the mask is what marks
 *                                   the permanent nav walls at instantiate.
 *                                   ZHM1 = no mask (t->hole == NULL).
 */
#ifndef TERRAIN_H
#define TERRAIN_H
#include <stdint.h>

typedef struct {
    int   w, h;                 /* sample grid dimensions */
    float origin_x, origin_y;   /* world meters of sample (0,0) */
    float px_per_m;             /* samples per meter (render resolution) */
    float *z;                   /* w*h heap, row-major z[j*w + i] */
    uint8_t *hole;              /* w*h, 1 = impassable static (ZHM2); NULL = ZHM1 */
} Terrain;

/* Load/save a .zhm. Return 0 on success, negative on error (file or format).
 * terrain_load allocates t->z (free with terrain_free). */
int  terrain_load(const char *path, Terrain *t);
int  terrain_save(const char *path, const Terrain *t);
void terrain_free(Terrain *t);   /* frees t->z and zeroes the struct */

/* Bilinear height at world (x, y). Samples outside the grid clamp to the edge
 * (never extrapolates). Returns 0 if t has no data. */
float terrain_z(const Terrain *t, float x, float y);

/* 1 if world (x, y) lands on an impassable static footprint (building/rock),
 * else 0. Nearest-sample on the ZHM2 hole mask; always 0 when the terrain has
 * none (ZHM1, or no holes baked). The caller turns these into permanent nav
 * walls (simp_set_wall + a high wall-cost tier) at instantiate. */
int terrain_hole(const Terrain *t, float x, float y);

/* Visit every NAV cell (grid gw x gh, square `cell` metres, origin aligned with
 * the terrain world origin) whose CENTRE lands on a hole, calling cb(user,cx,cy).
 * Lets the host mark permanent walls without coupling this module to the sim.
 * No-op if the terrain has no hole mask. Centre-sampling: a cell is a wall iff
 * its centre is inside a footprint (cell-resolution edge, tunable later). */
void terrain_each_hole_cell(const Terrain *t, float cell, int gw, int gh,
                            void (*cb)(void *user, int cx, int cy), void *user);

#endif /* TERRAIN_H */

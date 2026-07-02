# Terrain heightmap bake (EDITOR_DESIGN.md §9). Runs INSIDE Blender:
#
#   blender --background --python gfx/terrain_bake.py -- \
#           --in assets/terrain/level1.glb --out assets/terrain/level1.zhm [--ppm 4]
#
# Imports a ground mesh (.glb/.gltf/.obj), raycasts straight DOWN over its
# horizontal (X,Y) bounding box on a regular grid, and writes a .zhm binary:
# the baked elevation Z(x,y) used at runtime ONLY for render (seating sprites/
# structures on slopes). The simulation stays 2D planar on z=0 — this file
# never touches it.
#
# Coordinate mapping: game ground (x,y) == Blender (X,Y); height == Blender Z.
# Model the ground so its XY matches the scene's metric origin (the bake stores
# the AABB min as the .zhm origin, but aligning it to the scene is the modeler's
# job). Render resolution defaults to 4 samples/m (NOT the nav cell — this is
# render, not collision).
#
# .zhm FORMAT (little-endian, see terrain.h):
#   "ZHM1" | u32: w h | f32: origin_x origin_y px_per_m | w*h f32 Z row-major
#   (i along +X, j along +Y; z[j*w+i]).

import bpy
import math
import os
import struct
import sys
from mathutils import Vector


def parse_args():
    import argparse
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser(prog="terrain_bake.py")
    p.add_argument("--in", dest="inp", required=True,
                   help="ground mesh (.glb/.gltf/.obj)")
    p.add_argument("--out", required=True, help="output .zhm path")
    p.add_argument("--ppm", type=float, default=4.0,
                   help="samples per meter (render resolution, default 4)")
    return p.parse_args(argv)


def import_mesh(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=path)
    else:
        sys.exit(f"[terrain_bake] unsupported mesh format: {ext}")


def world_aabb(objs):
    lo = Vector((math.inf,) * 3)
    hi = Vector((-math.inf,) * 3)
    for ob in objs:
        for corner in ob.bound_box:
            w = ob.matrix_world @ Vector(corner)
            lo = Vector((min(lo[i], w[i]) for i in range(3)))
            hi = Vector((max(hi[i], w[i]) for i in range(3)))
    return lo, hi


def main():
    args = parse_args()

    # start from an empty scene so the default cube/camera don't shadow rays
    bpy.ops.wm.read_factory_settings(use_empty=True)
    import_mesh(args.inp)

    meshes = [ob for ob in bpy.context.scene.objects if ob.type == "MESH"]
    if not meshes:
        sys.exit("[terrain_bake] no mesh objects imported")

    lo, hi = world_aabb(meshes)
    ppm = args.ppm
    w = max(2, int(round((hi.x - lo.x) * ppm)) + 1)
    h = max(2, int(round((hi.y - lo.y) * ppm)) + 1)
    origin_x, origin_y = lo.x, lo.y
    z_top = hi.z + 1.0
    z_span = (hi.z - lo.z) + 2.0
    z_floor = lo.z   # fallback for ray misses (holes in the mesh)

    depsgraph = bpy.context.evaluated_depsgraph_get()
    scene = bpy.context.scene
    down = Vector((0.0, 0.0, -1.0))

    zvals = [0.0] * (w * h)
    valid = [False] * (w * h)
    misses = 0
    for j in range(h):
        wy = origin_y + j / ppm
        for i in range(w):
            wx = origin_x + i / ppm
            origin = Vector((wx, wy, z_top))
            hit, loc, _n, _idx, _ob, _m = scene.ray_cast(depsgraph, origin, down,
                                                         distance=z_span)
            if hit:
                zvals[j * w + i] = loc.z
                valid[j * w + i] = True
            else:
                misses += 1

    # HOLE mask (ZHM2, EDITOR_DESIGN.md §9): a ray miss = no ground there = an
    # impassable static footprint (building/rock). Snapshot it NOW, before the
    # backfill below mutates `valid`. Exclude the outer ring: boundary rays graze
    # the AABB max edges and miss without being real holes (a building shouldn't
    # sit exactly on the level boundary — there's a border). The Z is still
    # back-filled (continuous render edge); the mask carries the impassability.
    hole = bytearray(w * h)
    holes = 0
    for j in range(1, h - 1):
        for i in range(1, w - 1):
            if not valid[j * w + i]:
                hole[j * w + i] = 1
                holes += 1

    # Backfill misses (mesh holes, and the AABB max edges where boundary rays
    # graze and miss) from their nearest valid neighbours: iterate until stable
    # so plateau/edge heights propagate outward instead of snapping to z_floor.
    remaining = misses
    while remaining:
        progressed = False
        for j in range(h):
            for i in range(w):
                k = j * w + i
                if valid[k]:
                    continue
                acc, cnt = 0.0, 0
                if i > 0 and valid[k - 1]:        acc += zvals[k - 1]; cnt += 1
                if i + 1 < w and valid[k + 1]:    acc += zvals[k + 1]; cnt += 1
                if j > 0 and valid[k - w]:        acc += zvals[k - w]; cnt += 1
                if j + 1 < h and valid[k + w]:    acc += zvals[k + w]; cnt += 1
                if cnt:
                    zvals[k] = acc / cnt
                    valid[k] = True
                    remaining -= 1
                    progressed = True
        if not progressed:   # fully isolated (empty mesh region): floor it
            for k in range(w * h):
                if not valid[k]:
                    zvals[k] = z_floor
            break

    # ZHM2 = Z grid + hole mask. (ZHM1 would just omit the trailing mask.)
    with open(args.out, "wb") as fp:
        fp.write(b"ZHM2")
        fp.write(struct.pack("<2I", w, h))
        fp.write(struct.pack("<3f", origin_x, origin_y, ppm))
        fp.write(struct.pack(f"<{w * h}f", *zvals))
        fp.write(bytes(hole))

    print(f"[terrain_bake] {args.out}  {w}x{h} @ {ppm} px/m  "
          f"origin=({origin_x:.2f},{origin_y:.2f})  "
          f"z=[{lo.z:.2f},{hi.z:.2f}]  misses={misses}  holes={holes}")


main()

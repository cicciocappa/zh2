# Builds the export_scn.py demo/regression fixture:
#
#   blender --background --python gfx/test_level_make.py -- levels/test_level.blend
#
# The level exercises every rule of BLENDER_LEVEL.md:
# terrain grid, statics (one with footprint, one nav="none"), rect entities,
# wall/turret, explicit polys (rotated blockout + cost hazard), props (both a
# prop_<key> empty and a collection instance), `_`-ignored object, camera+light.
import bpy
import math
import os
import sys
from mathutils import Vector

OUT = sys.argv[sys.argv.index("--") + 1]

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
sc["cell"] = 0.5
sc["set_k_density"] = 2.5
sc["set_k_jam"] = 8.0

root = sc.collection


def mesh_obj(name, verts, faces, col, loc=(0, 0, 0), rot_z=0.0):
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    me.update()
    ob = bpy.data.objects.new(name, me)
    ob.location = loc
    ob.rotation_euler = (0, 0, math.radians(rot_z))
    col.objects.link(ob)
    return ob


def plane(name, x0, y0, x1, y1, col, z=0.05):
    return mesh_obj(name, [(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)],
                    [(0, 1, 2, 3)], col)


def box(name, hx, hy, z0, z1, col, loc=(0, 0, 0), rot_z=0.0):
    v = [(-hx, -hy, z0), (hx, -hy, z0), (hx, hy, z0), (-hx, hy, z0),
         (-hx, -hy, z1), (hx, -hy, z1), (hx, hy, z1), (-hx, hy, z1)]
    f = [(0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4),
         (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    return mesh_obj(name, v, f, col, loc, rot_z)


def empty(name, x, y, col, rot_z=0.0):
    ob = bpy.data.objects.new(name, None)
    ob.location = (x, y, 0)
    ob.rotation_euler = (0, 0, math.radians(rot_z))
    col.objects.link(ob)
    return ob


# ---- terrain: 60x40 m grid, gentle hills, AABB min at (0,0) ----------------
ter_col = bpy.data.collections.new("terrain")
root.children.link(ter_col)
NX, NY, STEP = 31, 21, 2.0
verts = [(i * STEP, j * STEP, 0.5 * math.sin(i * STEP / 9.0)
          * math.sin(j * STEP / 7.0))
         for j in range(NY) for i in range(NX)]
faces = [(j * NX + i, j * NX + i + 1, (j + 1) * NX + i + 1, (j + 1) * NX + i)
         for j in range(NY - 1) for i in range(NX - 1)]
mesh_obj("ground", verts, faces, ter_col)

# ---- statics: a building (footprint expected) + an arch (nav="none") -------
st_col = bpy.data.collections.new("statics")
root.children.link(st_col)
box("palazzo", 4, 3, 0, 9, st_col, loc=(40, 27, 0))
arch = box("arco", 1, 4, 4, 5, st_col, loc=(51, 30, 0))
arch["nav"] = "none"

# ---- rect entities ----------------------------------------------------------
plane("goal", 54, 16, 58, 24, root)
plane("spawn_north", 1, 4, 3, 16, root)
plane("spawn_south", 1, 24, 3, 36, root)
plane("pack", 10, 28, 14, 32, root)
mud = plane("cost_mud", 24, 4, 30, 10, root)
mud["weight"] = 5.0

# ---- wall + turret ----------------------------------------------------------
weak = plane("wall_weak", 30, 14, 42, 15.5, root)
weak["hp"] = 350.0
weak["cost_mult"] = 0.6
tur = empty("turret", 45, 20, root)
tur["range"] = 25.0

# ---- explicit polys: rotated blockout box + flat cost hazard ---------------
box("poly_block", 4, 3, 0, 4, root, loc=(22, 27, 0), rot_z=30.0)
hexv = [(12 + 4 * math.cos(a), 8 + 4 * math.sin(a), 0.0)
        for a in (math.radians(k * 60) for k in range(6))]
haz = mesh_obj("poly_hazard", hexv, [tuple(range(6))], root)
haz["cost"] = 6.0
haz["height"] = 0.4

# ---- props: naming fallback + collection instance ---------------------------
empty("prop_bench", 50, 10, root, rot_z=45.0)
cart_col = bpy.data.collections.new("cart")          # library-style, NOT in scene
box("cart_mesh", 0.6, 0.4, 0, 0.8, cart_col)
ci = bpy.data.objects.new("ci_cart", None)
ci.instance_type = "COLLECTION"
ci.instance_collection = cart_col
ci.location = (48, 31, 0)
ci.rotation_euler = (0, 0, math.radians(90))
root.objects.link(ci)

# ---- ignored / skipped ------------------------------------------------------
box("_ref_box", 1, 1, 0, 1, root, loc=(5, 5, 0))     # '_' prefix -> ignored
cam = bpy.data.objects.new("Camera", bpy.data.cameras.new("Camera"))
cam.location = (30, -30, 40)
root.objects.link(cam)
sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
sun.location = (30, 20, 50)
root.objects.link(sun)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
print(f"[make_test_level] saved {OUT}")

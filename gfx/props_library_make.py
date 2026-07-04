# Builds the prop library for level authoring (BLENDER_LEVEL.md §6):
#
#   blender --background --python gfx/props_library_make.py -- blend/props.blend
#
# One collection per catalog key (assets/props/catalog.txt), placeholder
# meshes at real-world scale, base at z=0. Collections are laid out in a row
# in the scene for editing; each collection's instance_offset points at its
# prop's base, so a Collection Instance in a level lands base-on-origin.
# Replacing a placeholder with a real mesh inside its collection updates
# every level that links it.
import bpy
import math
import os
import sys

OUT = sys.argv[sys.argv.index("--") + 1]

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
root = sc.collection


def material(name, rgba):
    mat = bpy.data.materials.new(name)
    mat.diffuse_color = rgba
    if mat.use_nodes:
        bsdf = mat.node_tree.nodes.get("Principled BSDF")
        if bsdf:
            bsdf.inputs["Base Color"].default_value = rgba
    return mat


def mesh_obj(name, verts, faces, col, mat, loc=(0, 0, 0)):
    me = bpy.data.meshes.new(name)
    me.from_pydata(verts, [], faces)
    me.update()
    me.materials.append(mat)
    ob = bpy.data.objects.new(name, me)
    ob.location = loc
    col.objects.link(ob)
    return ob


def box(name, hx, hy, z0, z1, col, mat, loc=(0, 0, 0)):
    v = [(-hx, -hy, z0), (hx, -hy, z0), (hx, hy, z0), (-hx, hy, z0),
         (-hx, -hy, z1), (hx, -hy, z1), (hx, hy, z1), (-hx, hy, z1)]
    f = [(3, 2, 1, 0), (4, 5, 6, 7), (0, 1, 5, 4),
         (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    return mesh_obj(name, v, f, col, mat, loc)


def prism(name, r, z0, z1, col, mat, loc=(0, 0, 0), n=12):
    ring = [(r * math.cos(2 * math.pi * k / n),
             r * math.sin(2 * math.pi * k / n)) for k in range(n)]
    v = [(x, y, z0) for x, y in ring] + [(x, y, z1) for x, y in ring]
    f = [tuple(reversed(range(n))), tuple(range(n, 2 * n))]
    f += [(k, (k + 1) % n, n + (k + 1) % n, n + k) for k in range(n)]
    return mesh_obj(name, v, f, col, mat, loc)


WOOD  = material("ph_wood",  (0.45, 0.30, 0.15, 1.0))
METAL = material("ph_metal", (0.55, 0.58, 0.62, 1.0))
GREEN = material("ph_green", (0.15, 0.40, 0.18, 1.0))
RED   = material("ph_red",   (0.70, 0.12, 0.10, 1.0))
DARK  = material("ph_dark",  (0.12, 0.12, 0.14, 1.0))
YELLW = material("ph_yellow", (0.75, 0.55, 0.10, 1.0))
CONCR = material("ph_concrete", (0.52, 0.50, 0.47, 1.0))


def build_bench(col):
    box("bench_seat", 0.90, 0.25, 0.40, 0.47, col, WOOD)
    box("bench_back", 0.90, 0.03, 0.47, 0.85, col, WOOD, (0, 0.22, 0))
    box("bench_leg_l", 0.05, 0.22, 0.0, 0.40, col, METAL, (-0.75, 0, 0))
    box("bench_leg_r", 0.05, 0.22, 0.0, 0.40, col, METAL, (0.75, 0, 0))


def build_sign(col):
    prism("sign_pole", 0.04, 0.0, 2.0, col, METAL)
    box("sign_panel", 0.35, 0.02, 1.40, 2.00, col, GREEN)


def build_cart(col):
    box("cart_body", 0.60, 0.35, 0.25, 0.85, col, METAL)
    box("cart_wheel_l", 0.16, 0.03, 0.0, 0.32, col, DARK, (0, -0.38, 0))
    box("cart_wheel_r", 0.16, 0.03, 0.0, 0.32, col, DARK, (0, 0.38, 0))
    box("cart_handle", 0.03, 0.30, 0.85, 1.00, col, METAL, (-0.55, 0, 0))


def build_bin(col):
    prism("bin_body", 0.28, 0.0, 0.85, col, GREEN)


def build_crate(col):
    box("crate_body", 0.40, 0.40, 0.0, 0.80, col, WOOD)


def build_table(col):
    box("table_top", 0.60, 0.40, 0.70, 0.75, col, WOOD)
    for i, (sx, sy) in enumerate(((-1, -1), (1, -1), (1, 1), (-1, 1))):
        box(f"table_leg_{i}", 0.035, 0.035, 0.0, 0.70, col, WOOD,
            (sx * 0.52, sy * 0.32, 0))


def build_chair(col):
    box("chair_seat", 0.21, 0.21, 0.42, 0.46, col, WOOD)
    box("chair_back", 0.21, 0.02, 0.46, 0.92, col, WOOD, (0, 0.19, 0))
    for i, (sx, sy) in enumerate(((-1, -1), (1, -1), (1, 1), (-1, 1))):
        box(f"chair_leg_{i}", 0.02, 0.02, 0.0, 0.42, col, WOOD,
            (sx * 0.17, sy * 0.17, 0))


def build_trafsign(col):
    prism("trafsign_pole", 0.035, 0.0, 2.3, col, METAL)
    box("trafsign_panel", 0.30, 0.02, 1.70, 2.30, col, RED)


def build_traflight(col):
    prism("traflight_pole", 0.06, 0.0, 3.2, col, METAL)
    box("traflight_head", 0.14, 0.11, 3.0, 3.9, col, DARK)


# Solid props (ENTITY_DESIGN §6+§8.5): mesh matches the catalog WxD footprint.
def build_fence(col):                       # 2.0 x 0.5 footprint, h 1.8
    box("fence_panel", 1.00, 0.04, 0.15, 1.80, col, METAL)
    box("fence_post_l", 0.06, 0.06, 0.0, 1.80, col, DARK, (-0.94, 0, 0))
    box("fence_post_r", 0.06, 0.06, 0.0, 1.80, col, DARK, (0.94, 0, 0))


def build_bus(col):                         # 11 x 2.5 footprint, h 2.6
    box("bus_body", 5.50, 1.25, 0.45, 2.60, col, YELLW)
    box("bus_stripe", 5.52, 1.26, 1.10, 1.60, col, DARK)
    for i, (sx, sy) in enumerate(((-1, -1), (1, -1), (1, 1), (-1, 1))):
        box(f"bus_wheel_{i}", 0.45, 0.10, 0.0, 0.75, col, DARK,
            (sx * 3.6, sy * 1.18, 0))


def build_building(col):                    # 10 x 8 footprint, h 8
    box("building_body", 5.0, 4.0, 0.0, 7.8, col, CONCR)
    box("building_roof", 5.2, 4.2, 7.8, 8.0, col, DARK)


# Catalog order (assets/props/catalog.txt) — collection name IS the key.
PROPS = [
    ("bench", build_bench), ("sign", build_sign), ("cart", build_cart),
    ("bin", build_bin), ("crate", build_crate), ("table", build_table),
    ("chair", build_chair), ("trafsign", build_trafsign),
    ("traflight", build_traflight),
    ("fence", build_fence), ("bus", build_bus), ("building", build_building),
]

for i, (key, build) in enumerate(PROPS):
    col = bpy.data.collections.new(key)
    root.children.link(col)
    build(col)
    dx = i * 3.0
    for ob in col.objects:
        ob.location.x += dx
    col.instance_offset = (dx, 0.0, 0.0)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
print(f"[props_library_make] saved {OUT}: "
      + ", ".join(k for k, _ in PROPS))

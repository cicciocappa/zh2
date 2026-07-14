# Level 1 greybox SEED — "Stabilimento" (industrial estate siege):
#
#   blender --background --python gfx/level1_make.py -- levels/level1.blend
#
# Generates the BLOCKED-OUT level so authoring never starts from a blank
# canvas: open the generated .blend in Blender and reshape/dress it there.
# Re-running this script OVERWRITES the file — it is a seed, not a build step;
# once you start editing in Blender, the .blend is the source of truth
# (BLENDER_LEVEL.md). Export:
#
#   blender --background levels/level1.blend --python gfx/export_scn.py -- \
#           --out assets/scenes/level1.scn
#
# Layout (140x100 m, flat ground):
#   - walled compound [80,130]x[25,75]: perimeter `wall` segments (hp 900,
#     cost_mult 1.2), a WEAK north section (hp 350, mult 0.5) and the main
#     west GATE: an 8 m opening barred by `fence` props (siegeable, opac 0.3
#     -> the inner turrets shoot through it);
#   - two warehouses (statics) inside form the gate->LZ corridor; LZ east.
#     NO starting defenses: the level provides terrain and walls, the DEFENSE
#     is the player's job (PREP budget) — never pre-place turrets in a level;
#   - outside: building pairs funnel the west approach to the gate and the
#     north approach onto the weak section; a bus wreck splits the west funnel;
#   - exits: west (immediate), north (delayed, feeds the weak wall), south
#     (late); dormant pack outside the east wall (mortar misfire wakes it);
#   - mission: survive 180 s, unlimited PREP, budget 800 (first-guess knob:
#     ~2 light + 1 heavy + barricades; tune at playtest).
import bpy
import math
import os
import sys

OUT = sys.argv[sys.argv.index("--") + 1]

bpy.ops.wm.read_factory_settings(use_empty=True)
sc = bpy.context.scene
sc["cell"] = 0.5
sc["set_k_density"] = 2.5
sc["set_k_jam"] = 8.0
sc["mission"] = "survive 180 prep 0"
sc["budget"] = 800.0

root = sc.collection

WORLD_W, WORLD_H = 140.0, 100.0


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


def box_min(name, x0, y0, x1, y1, h, col, z0=0.0):
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
         (x0, y0, h), (x1, y0, h), (x1, y1, h), (x0, y1, h)]
    f = [(0, 1, 2, 3), (4, 5, 6, 7), (0, 1, 5, 4),
         (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    return mesh_obj(name, v, f, col)


def empty(name, x, y, col, rot_z=0.0):
    ob = bpy.data.objects.new(name, None)
    ob.location = (x, y, 0)
    ob.rotation_euler = (0, 0, math.radians(rot_z))
    col.objects.link(ob)
    return ob


def wall_seg(name, x0, y0, x1, y1, hp, mult):
    ob = plane(name, x0, y0, x1, y1, root, z=0.1)
    ob["hp"] = float(hp)
    ob["cost_mult"] = float(mult)
    return ob


def exit_rect(name, x0, y0, x1, y1, rate, delay=0.0, pool=0):
    ob = plane(name, x0, y0, x1, y1, root)
    ob["rate"] = float(rate)
    ob["delay"] = float(delay)
    ob["pool"] = int(pool)
    return ob


# ---- terrain: flat ground, AABB min at (0,0) --------------------------------
ter_col = bpy.data.collections.new("terrain")
root.children.link(ter_col)
mesh_obj("ground", [(0, 0, 0), (WORLD_W, 0, 0),
                    (WORLD_W, WORLD_H, 0), (0, WORLD_H, 0)],
         [(0, 1, 2, 3)], ter_col)

# ---- compound perimeter (destructible walls, thickness 1.5) -----------------
# west wall x in [79.25,80.75], GATE opening y in [46,54]
wall_seg("wall_west_s", 79.25, 24.25, 80.75, 46.0, 900, 1.2)
wall_seg("wall_west_n", 79.25, 54.0, 80.75, 75.75, 900, 1.2)
# north wall y in [74.25,75.75], WEAK section x in [100,110]
wall_seg("wall_north_w", 80.75, 74.25, 100.0, 75.75, 900, 1.2)
wall_seg("wall_north_weak", 100.0, 74.25, 110.0, 75.75, 350, 0.5)
wall_seg("wall_north_e", 110.0, 74.25, 130.75, 75.75, 900, 1.2)
# east + south, split for HP locality
wall_seg("wall_east_s", 129.25, 24.25, 130.75, 50.0, 900, 1.2)
wall_seg("wall_east_n", 129.25, 50.0, 130.75, 75.75, 900, 1.2)
wall_seg("wall_south_w", 80.75, 24.25, 105.0, 25.75, 900, 1.2)
wall_seg("wall_south_e", 105.0, 24.25, 129.25, 25.75, 900, 1.2)

# main gate: fence props bar the 8 m opening (shoot-through, siegeable)
for i, fy in enumerate((47.0, 49.0, 51.0, 53.0)):
    empty(f"prop_fence.gate{i}", 80.0, fy, root, rot_z=90.0)

# ---- statics: warehouses inside, funnel buildings outside -------------------
st_col = bpy.data.collections.new("statics")
root.children.link(st_col)
box_min("capannone_s", 88, 30, 104, 42, 6, st_col)
box_min("capannone_n", 88, 58, 104, 70, 6, st_col)
box_min("palazzo_w1", 48, 30, 64, 44, 7, st_col)
box_min("palazzo_w2", 48, 56, 64, 70, 7, st_col)
box_min("palazzo_n1", 84, 84, 98, 92, 7, st_col)
box_min("palazzo_n2", 112, 84, 126, 92, 7, st_col)
box_min("palazzo_s", 96, 8, 110, 16, 7, st_col)

# ---- LZ (defenses are the player's job: no pre-placed turrets) --------------
empty("lz", 112, 50, root)

# ---- exits (assault directors) ----------------------------------------------
exit_rect("exit_west", 1, 40, 4, 60, rate=7.0)
exit_rect("exit_north", 98, 96, 112, 99, rate=5.0, delay=40.0)
exit_rect("exit_south", 96, 1, 110, 4, rate=5.0, delay=80.0)

# ---- dormant pack outside the east wall -------------------------------------
plane("pack_east", 133, 44, 138, 56, root)

# ---- decor props ------------------------------------------------------------
empty("prop_bus", 56, 50, root, rot_z=15.0)   # wreck splitting the west funnel
empty("prop_crate", 116, 58, root)
empty("prop_crate.001", 117.2, 59.6, root, rot_z=30.0)
empty("prop_bin", 114, 42, root)

# ---- work camera + sun (ignored by export) ----------------------------------
cam = bpy.data.objects.new("Camera", bpy.data.cameras.new("Camera"))
cam.location = (105, -20, 90)
cam.rotation_euler = (math.radians(50), 0, 0)
root.objects.link(cam)
sun = bpy.data.objects.new("Sun", bpy.data.lights.new("Sun", "SUN"))
sun.location = (70, 50, 60)
sun.rotation_euler = (math.radians(35), math.radians(15), 0)
root.objects.link(sun)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
bpy.ops.wm.save_as_mainfile(filepath=os.path.abspath(OUT))
print(f"[level1_make] saved {OUT}")

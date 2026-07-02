# Builds the demo terrain mesh for assets/scenes/terrain.scn (EDITOR_DESIGN §9). Runs
# INSIDE Blender, then bake the .zhm alongside:
#
#   blender --background --python gfx/terrain_demo_make.py
#   blender --background --python gfx/terrain_bake.py -- \
#           --in assets/terrain/terrain_demo.glb --out assets/terrain/terrain_demo.zhm
#
# 100x70 m world (matches assets/scenes/terrain.scn): a gentle slope along +y plus a
# 0.5 m sidewalk step at x>=50, to show sprites/structures seating on elevation.
import bpy

bpy.ops.wm.read_factory_settings(use_empty=True)

def zf(x, y):
    return 0.012 * y + (0.5 if x >= 50.0 else 0.0)

nx, ny = 101, 71  # 1 m spacing
verts, faces = [], []
for j in range(ny):
    for i in range(nx):
        verts.append((float(i), float(j), zf(i, j)))
for j in range(ny - 1):
    for i in range(nx - 1):
        a = j * nx + i
        faces.append((a, a + 1, a + nx + 1, a + nx))

m = bpy.data.meshes.new("terrain")
m.from_pydata(verts, [], faces)
m.update()
o = bpy.data.objects.new("terrain", m)
bpy.context.collection.objects.link(o)
bpy.ops.export_scene.gltf(filepath="assets/terrain/terrain_demo.glb",
                          export_format="GLB", use_selection=False)
print("[terrain_demo_make] assets/terrain/terrain_demo.glb")

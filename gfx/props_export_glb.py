# Exports every prop collection of the library to a runtime glb:
#
#   blender --background blend/props.blend --python gfx/props_export_glb.py \
#           -- assets/models/props
#
# One <key>.glb per collection (key = collection name = catalog key). The
# library lays collections out in a row with instance_offset pointing at each
# prop's base: objects are shifted back by that offset before export (and
# restored after), so every glb has the prop base at the origin — same
# convention as the collection instances in the levels. Flat materials only
# (baseColorFactor); vat_horde's load_prop_models reads pos+normal+material
# color, no textures.
import bpy
import os
import sys

OUTDIR = sys.argv[sys.argv.index("--") + 1]
os.makedirs(OUTDIR, exist_ok=True)

for col in sorted(bpy.data.collections, key=lambda c: c.name):
    if col.name.startswith("_") or not col.objects:
        continue
    off = tuple(col.instance_offset)
    for ob in col.objects:
        ob.location = (ob.location.x - off[0], ob.location.y - off[1],
                       ob.location.z - off[2])
    bpy.ops.object.select_all(action="DESELECT")
    for ob in col.objects:
        ob.select_set(True)
    path = os.path.join(OUTDIR, f"{col.name}.glb")
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB",
                              use_selection=True)
    for ob in col.objects:
        ob.location = (ob.location.x + off[0], ob.location.y + off[1],
                       ob.location.z + off[2])
    print(f"[props_export_glb] {path}: {len(col.objects)} objects")

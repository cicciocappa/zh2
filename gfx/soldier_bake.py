# soldier_bake.py — bake the original soldier's 5 per-part PBR sets into ONE
# diffuse atlas usable by the Mixamo-rigged mesh (soldier_glb_make.py).
#
# Run headless:
#   ~/Scaricati/blender-5.2.0-linux-x64/blender --background --python gfx/soldier_bake.py
#
# Input:  ~/Scaricati/soldier/soldier.fbx        (original, 5 materials)
#         ~/Scaricati/soldier/textures/soldier_<Part>_Diffuse.png
# Output: blend/soldier_diffuse.png              (1024x1024 atlas, committed)
#         blend/soldier_atlas_uv.bin             (float32 u,v per loop, committed)
#
# Why: the rigged FBX from Mixamo lost the materials but kept topology AND UVs
# bit-identical to the original (verified: 13671v/13721f/53560 loops, max UV
# deviation 0.0). Each part's UVs fill their own 0-1 square though, so they
# overlap across parts — a single texture needs a repack. We remap islands
# into fixed atlas regions (deterministic, no pack_islands luck), Emit-bake
# the diffuses there, and dump the remapped per-loop UVs; soldier_glb_make.py
# rewrites the rigged mesh's UV layer from the .bin by loop index.
#
# The gloss/normal/specular/opacity maps are deliberately dropped: the soldier
# is ~40 px tall on screen, flat diffuse is all that survives at that size.

import bpy
import os
import struct
import sys

SRC_DIR = os.path.expanduser("~/Scaricati/soldier")
SRC_FBX = os.path.join(SRC_DIR, "soldier.fbx")
TEX_DIR = os.path.join(SRC_DIR, "textures")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_PNG = os.path.join(ROOT, "blend", "soldier_diffuse.png")
OUT_UV = os.path.join(ROOT, "blend", "soldier_atlas_uv.bin")

ATLAS = 1024
MARGIN = 8  # bake margin, px

# Fixed atlas regions (u0, v0, w, h). Body carries 76% of the loops and all
# the skin/face detail -> the big slab; the small parts share the right strip.
REGIONS = {
    "Body":   (0.00, 0.00, 0.75, 1.00),
    "Top":    (0.75, 0.75, 0.25, 0.25),
    "Bottom": (0.75, 0.50, 0.25, 0.25),
    "Shoes":  (0.75, 0.25, 0.25, 0.25),
    "Hat":    (0.75, 0.00, 0.25, 0.25),
}


def die(msg):
    print("FATAL: " + msg)
    sys.exit(1)


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if not os.path.isfile(SRC_FBX):
        die("missing " + SRC_FBX)
    bpy.ops.import_scene.fbx(filepath=SRC_FBX)
    obj = next((o for o in bpy.data.objects if o.type == 'MESH'), None)
    if not obj:
        die("no mesh in " + SRC_FBX)
    me = obj.data
    if len(me.uv_layers) != 1:
        die("expected 1 uv layer, found %d" % len(me.uv_layers))

    # --- atlas UV layer: per-part affine remap into the fixed regions ---
    src_uv = me.uv_layers[0]
    atlas_uv = me.uv_layers.new(name="atlas")
    for poly in me.polygons:
        mat = me.materials[poly.material_index]
        if mat.name not in REGIONS:
            die("material %r has no atlas region" % mat.name)
        u0, v0, w, h = REGIONS[mat.name]
        for li in range(poly.loop_start, poly.loop_start + poly.loop_total):
            u, v = src_uv.data[li].uv
            atlas_uv.data[li].uv = (u0 + u * w, v0 + v * h)

    # --- materials -> Emission(diffuse texture via ORIGINAL UVs) ---
    for mat in me.materials:
        tex_path = os.path.join(TEX_DIR, "soldier_%s_Diffuse.png" % mat.name)
        if not os.path.isfile(tex_path):
            die("missing " + tex_path)
        mat.use_nodes = True
        nt = mat.node_tree
        nt.nodes.clear()
        out = nt.nodes.new("ShaderNodeOutputMaterial")
        emit = nt.nodes.new("ShaderNodeEmission")
        tex = nt.nodes.new("ShaderNodeTexImage")
        tex.image = bpy.data.images.load(tex_path)
        uvn = nt.nodes.new("ShaderNodeUVMap")
        uvn.uv_map = src_uv.name
        nt.links.new(uvn.outputs["UV"], tex.inputs["Vector"])
        nt.links.new(tex.outputs["Color"], emit.inputs["Color"])
        nt.links.new(emit.outputs["Emission"], out.inputs["Surface"])

    # --- bake target image on the atlas layer ---
    img = bpy.data.images.new("soldier_atlas", ATLAS, ATLAS, alpha=False)
    for mat in me.materials:
        nt = mat.node_tree
        node = nt.nodes.new("ShaderNodeTexImage")
        node.image = img
        nt.nodes.active = node
    atlas_uv.active = True          # bake writes through the active UV layer
    me.uv_layers.active = atlas_uv

    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.samples = 1        # EMIT needs no sampling
    scene.render.bake.margin = MARGIN
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.bake(type='EMIT')

    img.filepath_raw = OUT_PNG
    img.file_format = 'PNG'
    img.save()
    print("wrote %s" % OUT_PNG)

    # --- dump atlas UVs per loop (float32 u,v; loop order == rigged FBX) ---
    with open(OUT_UV, "wb") as f:
        f.write(struct.pack("<II", 0x56555341, len(me.loops)))  # 'ASUV', count
        for d in atlas_uv.data:
            f.write(struct.pack("<ff", d.uv[0], d.uv[1]))
    print("wrote %s (%d loops)" % (OUT_UV, len(me.loops)))


main()

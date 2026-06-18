#!/usr/bin/env python3
"""Esporta in FBX un .blend GIA' RIGGATO (mesh + armatura mixamorig) pronto per
vat/bake_zombie.sh. A differenza di export_mixamo_fbx.py (solo mesh, per l'upload
all'auto-rig Mixamo) qui si esportano mesh E armatura insieme, perche' il rig esiste
gia' nel file di modeling e non passa da Mixamo.

Stessi assi della pipeline (axis_up='Y', axis_forward='-Z'): il baker e gli FBX
d'animazione assumono quella convenzione. bake_anim=False: dell'animazione del file
non importa nulla, il baker applica le sue 13 clip al rig.

Uso (Blender headless):
  blender --background MODEL.blend --python vat/export_rigged_fbx.py -- OUT.fbx
Poi: vat/bake_zombie.sh OUT.fbx vat/assets/<nome>
"""
import bpy, sys

out = sys.argv[sys.argv.index("--") + 1:][0]

meshes = [o for o in bpy.data.objects if o.type == 'MESH']
arms = [o for o in bpy.data.objects if o.type == 'ARMATURE']
print(f"EXPORT mesh={[o.name for o in meshes]} armature={[o.name for o in arms]}")
assert meshes, "nessuna mesh nel file"
assert arms, "nessuna armatura: il file non sembra riggato"

bpy.ops.export_scene.fbx(filepath=out, use_selection=False,
    object_types={'MESH', 'ARMATURE'}, add_leaf_bones=False, bake_anim=False,
    axis_up='Y', axis_forward='-Z')
print("EXPORTED", out)

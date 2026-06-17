#!/usr/bin/env python3
"""Export a Blender mesh to an FBX ready for Mixamo auto-rig.

Riproduce ESATTAMENTE il preset che funziona (verificato contro
blend/male_version.fbx, il modello "normale" che rigga e baka con stride giusto):
GlobalSettings UpAxis=Y, FrontAxisSign=+1, CoordAxisSign=+1, UnitScaleFactor=1.0,
geometria Y-up con la conversione di unita' cotta nei vertici (raw ~17.6k, wscale
0.01 al reimport). Vedi vat/RIG_MIXAMO_NOTES.md (Bug 1 + Bug 2).

I parametri che contano e perche' (sweep documentato in RIG_MIXAMO_NOTES):
  axis_up='Y', axis_forward='-Z'  -> FrontAxisSign/CoordAxisSign = +1 (NON 'Z': da' -1,
                                     il facing invertito del Bug 1)
  bake_space_transform=True       -> "Apply Transform": raddrizza su Y e cuoce la
                                     conversione di unita' nella geometria
  apply_scale_options='FBX_SCALE_NONE' -> UnitScaleFactor=1.0 (NON 'FBX_SCALE_ALL'/
                                     'FBX_SCALE_UNITS': danno 100 -> Mixamo vede un
                                     gigante da 175 m e l'auto-rig FALLISCE)
  global_scale=1.0                -> mai 0.01 (rimpicciolisce -> stride 100x, Bug 2)

Uso (Blender headless):
  blender --background MODEL.blend --python vat/export_mixamo_fbx.py -- OUT.fbx
"""
import bpy, sys

out = sys.argv[sys.argv.index("--") + 1:][0]

# Applica i transform alla mesh (deform-safe: nessuna armatura nel file di modeling)
for o in bpy.data.objects:
    if o.type == 'MESH':
        o.select_set(True)
        bpy.context.view_layer.objects.active = o
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)

bpy.ops.export_scene.fbx(
    filepath=out,
    use_selection=False,
    object_types={'MESH'},
    apply_unit_scale=True,
    apply_scale_options='FBX_SCALE_NONE',
    global_scale=1.0,
    bake_space_transform=True,
    axis_up='Y',
    axis_forward='-Z',
    mesh_smooth_type='OFF',
)
print("EXPORTED", out)

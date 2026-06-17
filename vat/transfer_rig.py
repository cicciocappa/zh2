#!/usr/bin/env python3
"""Trasferisce il rig Mixamo da un modello riggato a una VARIANTE con topologia
identica (stesso numero/ordine di vertici), spostando i vertici per indice.

Per i 5 zombie (vat/RIG_MIXAMO_NOTES.md, [[modelli-zombie-topologia]]): l'auto-rig
di Mixamo sul lowpoly a 297 vert è una lotteria. Si rigga UNA volta il normale, poi
le varianti a topologia identica (OBESI, BAMBINI) ereditano armatura+pesi spostando
solo i vertici della forma. Verificato: Mixamo PRESERVA l'ordine dei vertici (Procrustes
normale.blend vs rigged: rotazione identità, scala 1, residuo ~0) → la copia per indice
è esatta. Le DONNE hanno un loop cut in più (topologia diversa) → NON usabile qui, serve
il trasferimento per prossimità (Data Transfer).

Come funziona: importa il rigged (armatura+skin+UV intatti), legge i vertici di BASE e
VARIANTE dai rispettivi .blend (append, stesso ordine), e per ogni vertice del rigged
applica il delta world (variante - base). La forma risultante è quella della variante
allineata al frame del rig; i vertex group (pesi) non vengono toccati → preservati.

Uso (Blender headless):
  blender --background --python vat/transfer_rig.py -- \
      <rigged.fbx> <base.blend> <variante.blend> <out_rigged.fbx>

Poi: vat/bake_zombie.sh <out_rigged.fbx> vat/assets/<nome>
"""
import bpy, sys
from mathutils import Vector

a = sys.argv[sys.argv.index("--") + 1:]
rigged, base_blend, var_blend, out = a[0], a[1], a[2], a[3]

def append_mesh_world(path):
    """Append il primo mesh object da un .blend, ritorna i vertici in coord world."""
    with bpy.data.libraries.load(path, link=False) as (src, dst):
        dst.objects = list(src.objects)
    obj = None
    for o in dst.objects:
        if o and o.type == 'MESH':
            bpy.context.scene.collection.objects.link(o); obj = o; break
    coords = [obj.matrix_world @ v.co for v in obj.data.vertices]
    bpy.data.objects.remove(obj, do_unlink=True)
    return coords

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=rigged)
mesh = next(o for o in bpy.data.objects if o.type == 'MESH')

base = append_mesh_world(base_blend)
var  = append_mesh_world(var_blend)
nv = len(mesh.data.vertices)
assert nv == len(base) == len(var), f"topologia diversa: rigged={nv} base={len(base)} var={len(var)}"

mw = mesh.matrix_world.copy(); mwi = mw.inverted()
moved = 0; maxd = 0.0
for i, v in enumerate(mesh.data.vertices):
    d = var[i] - base[i]
    if d.length > 1e-6:
        moved += 1; maxd = max(maxd, d.length)
    v.co = mwi @ (mw @ v.co + d)
mesh.data.update()
print(f"TRANSFER verts={nv} moved={moved} maxdelta_world={maxd:.3f}")

bpy.ops.export_scene.fbx(filepath=out, use_selection=False,
    object_types={'MESH', 'ARMATURE'}, add_leaf_bones=False, bake_anim=False,
    axis_up='Y', axis_forward='-Z')
print("EXPORTED", out)

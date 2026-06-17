#!/usr/bin/env python3
"""Trasferisce il rig Mixamo a una variante con topologia DIVERSA (es. donne con
loop cut per il seno) tramite Data Transfer per PROSSIMITA'.

A differenza di transfer_rig.py (copia pesi per indice, richiede topologia identica),
qui i pesi vengono interpolati dalla superficie piu' vicina della mesh sorgente →
funziona anche con conteggio/ordine di vertici diversi. La mesh destinazione DEVE essere
spazialmente allineata e a scala simile alla sorgente riggata (stessa posa/orientamento),
altrimenti la prossimita' pesca i pesi sbagliati. Le UV della destinazione NON vengono
toccate (si trasferiscono solo i VGROUP_WEIGHTS) → resta la sua uvmap.

Uso (Blender headless):
  blender --background --python vat/transfer_rig_proximity.py -- \
      <rigged.fbx> <variante.blend> <out_rigged.fbx> [nome_mesh_dst]

Se la variante .blend contiene piu' mesh, di default sceglie quella UNRIGGED (0 vertex
group, nessun modifier armature) piu' alta; passare [nome_mesh_dst] per forzarla.
Poi: vat/bake_zombie.sh <out_rigged.fbx> vat/assets/<nome>
"""
import bpy, sys
from mathutils import Vector

a = sys.argv[sys.argv.index("--") + 1:]
rigged, var_blend, out = a[0], a[1], a[2]
want_name = a[3] if len(a) > 3 else None

def bbox(o):
    vs = [o.matrix_world @ v.co for v in o.data.vertices]
    mn = Vector((min(p[i] for p in vs) for i in range(3)))
    mx = Vector((max(p[i] for p in vs) for i in range(3)))
    return mn, mx

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=rigged)
src = next(o for o in bpy.data.objects if o.type == 'MESH')
arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
smn, smx = bbox(src)

# append la variante, scegli la mesh destinazione (unrigged, allineata)
with bpy.data.libraries.load(var_blend, link=False) as (s, d):
    d.objects = list(s.objects)
cands = []
for o in d.objects:
    if o and o.type == 'MESH':
        bpy.context.scene.collection.objects.link(o)
        cands.append(o)
if want_name:
    dst = next(o for o in cands if o.name == want_name)
else:
    clean = [o for o in cands if len(o.vertex_groups) == 0 and
             not any(m.type == 'ARMATURE' for m in o.modifiers)]
    pool = clean or cands
    dst = max(pool, key=lambda o: (o.matrix_world @ Vector((0, 0, 0))).z * 0 +
              (bbox(o)[1].z - bbox(o)[0].z))  # piu' alta
# rimuovi le altre mesh appese
for o in cands:
    if o is not dst:
        bpy.data.objects.remove(o, do_unlink=True)

dmn, dmx = bbox(dst)
print(f"SRC verts={len(src.data.vertices)} bbox=({smn.x:.1f},{smn.y:.1f},{smn.z:.1f})-({smx.x:.1f},{smx.y:.1f},{smx.z:.1f})")
print(f"DST {dst.name!r} verts={len(dst.data.vertices)} uv={len(dst.data.uv_layers)} "
      f"bbox=({dmn.x:.1f},{dmn.y:.1f},{dmn.z:.1f})-({dmx.x:.1f},{dmx.y:.1f},{dmx.z:.1f})")

# Data Transfer pesi: ATTIVO=src (sorgente), SELEZIONATO=dst (destinazione)
bpy.ops.object.select_all(action='DESELECT')
src.select_set(True); dst.select_set(True)
bpy.context.view_layer.objects.active = src
bpy.ops.object.data_transfer(use_create=True, data_type='VGROUP_WEIGHTS',
    vert_mapping='POLYINTERP_NEAREST', layers_select_src='ALL', layers_select_dst='NAME')
print(f"DST vgroups dopo transfer: {len(dst.vertex_groups)}")

# lega la dst alla stessa armatura
amod = dst.modifiers.new("Armature", 'ARMATURE'); amod.object = arm
# rimuovi la mesh sorgente: esportiamo solo dst + armatura
bpy.data.objects.remove(src, do_unlink=True)

bpy.ops.object.select_all(action='SELECT')
bpy.ops.export_scene.fbx(filepath=out, use_selection=False,
    object_types={'MESH', 'ARMATURE'}, add_leaf_bones=False, bake_anim=False,
    axis_up='Y', axis_forward='-Z')
print("EXPORTED", out)

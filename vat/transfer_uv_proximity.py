#!/usr/bin/env python3
"""Trasferisce la UV MAP da un modello riggato sorgente a una variante riggata a
topologia DIVERSA, per PROSSIMITA' (Data Transfer, loop data).

Caso d'uso: la donna obesa (`fem_version_obese_rigged.fbx`, 300 vert, loop cut del
seno) ha una uvmap sbagliata; la uvmap corretta e' quella della donna normale
(`fem_version_rigged.fbx`, 297 vert). Le due mesh sono quasi identiche e allineate
in rest pose, quindi la UV della sorgente si interpola dalla superficie piu' vicina
sulla destinazione. Vengono trasferite SOLO le UV (loop data): pesi vertex-group e
modificatore armatura della destinazione restano INTATTI.

Uso (Blender headless):
  blender --background --python vat/transfer_uv_proximity.py -- \
      <src_uv.fbx> <dst.fbx> <out.fbx>

  src_uv = riggato con la UV CORRETTA (fem normale)
  dst    = riggato da correggere (fem obesa): pesi+armatura preservati, UV sostituita
  out    = FBX di uscita (mesh dst + sua armatura, UV corretta)

Poi: vat/bake_zombie.sh <out.fbx> assets/zombies/zombie_fem_obese
"""
import bpy, sys

a = sys.argv[sys.argv.index("--") + 1:]
src_fbx, dst_fbx, out = a[0], a[1], a[2]

bpy.ops.wm.read_factory_settings(use_empty=True)

# --- importa DEST (da correggere) per prima, cosi' la distinguo dalla sorgente ---
bpy.ops.import_scene.fbx(filepath=dst_fbx)
dst = next(o for o in bpy.data.objects if o.type == 'MESH')
dst_arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')
dst.name = 'DST'; dst_arm.name = 'ARM_DST'

# --- importa SRC (UV corretta) ---
bpy.ops.import_scene.fbx(filepath=src_fbx)
src = next(o for o in bpy.data.objects if o.type == 'MESH' and o is not dst)
src_arms = [o for o in bpy.data.objects if o.type == 'ARMATURE' and o is not dst_arm]

print(f"DST {dst.name!r} verts={len(dst.data.vertices)} "
      f"uv={[l.name for l in dst.data.uv_layers]} vgroups={len(dst.vertex_groups)}")
print(f"SRC {src.name!r} verts={len(src.data.vertices)} "
      f"uv={[l.name for l in src.data.uv_layers]}")

# uv attiva su entrambe (indice 0)
dst.data.uv_layers.active_index = 0
src.data.uv_layers.active_index = 0

# --- Data Transfer UV: ATTIVO=src (sorgente), SELEZIONATO=dst (destinazione) ---
# loop_mapping POLYINTERP_NEAREST = interpola dalla faccia piu' vicina (UV continua
# dentro le isole; possibile smear solo sulle cuciture -> verifica visiva).
bpy.ops.object.select_all(action='DESELECT')
src.select_set(True); dst.select_set(True)
bpy.context.view_layer.objects.active = src
bpy.ops.object.data_transfer(use_create=True, data_type='UV',
    loop_mapping='POLYINTERP_NEAREST',
    layers_select_src='ACTIVE', layers_select_dst='ACTIVE')
print(f"UV trasferita su dst: {[l.name for l in dst.data.uv_layers]}")

# --- pulizia: via la sorgente, esporto solo dst + la sua armatura ---
bpy.data.objects.remove(src, do_unlink=True)
for o in src_arms:
    bpy.data.objects.remove(o, do_unlink=True)

bpy.ops.object.select_all(action='SELECT')
bpy.ops.export_scene.fbx(filepath=out, use_selection=False,
    object_types={'MESH', 'ARMATURE'}, add_leaf_bones=False, bake_anim=False,
    axis_up='Y', axis_forward='-Z')
print("EXPORTED", out)

#!/usr/bin/env python3
"""Copia la UV MAP per INDICE VERTICE da una mesh sorgente (UV corretta) a una
variante che condivide la topologia salvo un loop cut locale (indici preservati +
pochi vertici nuovi in coda). A differenza del transfer per prossimita' NON interpola
e NON spalma il layout: le facce con lo stesso set di indici ricevono l'UV ESATTA
corner-per-corner. Le poche facce del loop cut (vertici nuovi) vengono riempite
interpolando dai vertici condivisi adiacenti (pancia = interno isola, niente cucitura).

Applica l'UV direttamente sul FBX GIA' RIGGATO della variante: pesi e armatura
restano INTATTI, si sostituisce solo la UV.

  blender --background --python vat/transfer_uv_byindex.py -- \
      <src_uv.blend|fbx> <dst_rigged.fbx> <out.fbx>

  src_uv = .blend O .fbx con la mesh a UV CORRETTA (fem normale = il FBX riggato,
           il blend ha l'UV sbagliata)
  dst    = FBX riggato da correggere (fem obesa)
  out    = FBX di uscita (UV corretta, rig preservato)
"""
import bpy, sys

a = sys.argv[sys.argv.index("--") + 1:]
src_path, dst_fbx, out = a[0], a[1], a[2]

bpy.ops.wm.read_factory_settings(use_empty=True)

def load(path):
    """Carica un file, ritorna (mesh_modello, [oggetti_da_rimuovere_dopo])."""
    before = set(bpy.data.objects)
    if path.lower().endswith(".fbx"):
        bpy.ops.import_scene.fbx(filepath=path)
        objs = [o for o in bpy.data.objects if o not in before]
    else:
        with bpy.data.libraries.load(path, link=False) as (s, d):
            d.objects = list(s.objects)
        objs = [o for o in d.objects if o]
        for o in objs:
            bpy.context.scene.collection.objects.link(o)
    meshes = [o for o in objs if o.type == 'MESH']
    mesh = max(meshes, key=lambda o: len(o.data.vertices))
    extra = [o for o in objs if o is not mesh]  # armatura sorgente ecc.
    return mesh, extra

# --- destinazione: FBX riggato (pesi+armatura da PRESERVARE) ---
dst, _ = load(dst_fbx)
dm = dst.data
duvl = dm.uv_layers.active.data

# --- sorgente: mesh a UV corretta (dal FBX riggato) ---
src, src_extra = load(src_path)
sm = src.data
suvl = sm.uv_layers.active.data

print(f"SRC {src.name} v={len(sm.vertices)} f={len(sm.polygons)}")
print(f"DST {dst.name} v={len(dm.vertices)} f={len(dm.polygons)}")

# src: set-di-indici faccia -> {vertex_index: loop_index}
sface = {}
for p in sm.polygons:
    key = frozenset(p.vertices)
    corner = {sm.loops[li].vertex_index: li for li in p.loop_indices}
    sface[key] = corner

assigned = [False] * len(dm.loops)
# accumulo UV per indice vertice (per il riempimento della pancia)
vsum = {}   # vidx -> [u_sum, v_sum, n]

def acc(vidx, uv):
    s = vsum.setdefault(vidx, [0.0, 0.0, 0])
    s[0] += uv[0]; s[1] += uv[1]; s[2] += 1

# --- pass 1: facce combacianti per indici -> copia ESATTA per corner ---
nmatch = 0
for p in dm.polygons:
    corner = sface.get(frozenset(p.vertices))
    if corner is None:
        continue
    nmatch += 1
    for li in p.loop_indices:
        vi = dm.loops[li].vertex_index
        sl = corner.get(vi)
        if sl is None:
            continue
        uv = suvl[sl].uv
        duvl[li].uv = (uv[0], uv[1])
        assigned[li] = True
        acc(vi, uv)
print(f"facce copiate per indici: {nmatch}/{len(dm.polygons)}")

# --- pass 2a: corner non assegnati su vertici CONDIVISI -> media UV di quel vertice ---
# --- pass 2b: corner su vertici NUOVI -> media dei corner gia' assegnati della faccia ---
def mean_v(vidx):
    s = vsum.get(vidx)
    return (s[0]/s[2], s[1]/s[2]) if s and s[2] else None

filled = 0
pending_new = []  # (loop_index, poly) da fare dopo aver riempito i condivisi
for p in dm.polygons:
    for li in p.loop_indices:
        if assigned[li]:
            continue
        vi = dm.loops[li].vertex_index
        m = mean_v(vi)
        if m is not None:
            duvl[li].uv = m; assigned[li] = True; filled += 1
        else:
            pending_new.append((li, p))
for li, p in pending_new:
    # media dei corner della stessa faccia gia' assegnati
    acc_u = acc_v = 0.0; n = 0
    for lj in p.loop_indices:
        if assigned[lj]:
            uv = duvl[lj].uv; acc_u += uv[0]; acc_v += uv[1]; n += 1
    if n:
        duvl[li].uv = (acc_u/n, acc_v/n); assigned[li] = True; filled += 1
print(f"corner riempiti (loop cut): {filled}   non assegnati residui: {assigned.count(False)}")

# --- pulizia + export (solo dst + sua armatura) ---
bpy.data.objects.remove(src, do_unlink=True)
for o in src_extra:
    try: bpy.data.objects.remove(o, do_unlink=True)
    except Exception: pass
bpy.ops.object.select_all(action='SELECT')
bpy.ops.export_scene.fbx(filepath=out, use_selection=False,
    object_types={'MESH', 'ARMATURE'}, add_leaf_bones=False, bake_anim=False,
    axis_up='Y', axis_forward='-Z')
print("EXPORTED", out)

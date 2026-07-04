#!/usr/bin/env python3
"""Diagnostico corrispondenza per INDICE/TOPOLOGIA (pose-invariante) tra due mesh:
quante facce dell'obesa coincidono per SET DI INDICI VERTICE con una faccia della
normale? Se quasi tutte, l'obesa = normale + loop cut a indici preservati e l'UV si
copia ESATTO per corner (via indice vertice), niente interpolazione.

  blender --background --python vat/uv_topo_diag.py -- <src.blend> <dst.blend>
"""
import bpy, sys

a = sys.argv[sys.argv.index("--") + 1:]
srcf, dstf = a[0], a[1]
bpy.ops.wm.read_factory_settings(use_empty=True)

def load_mesh(path, tag):
    with bpy.data.libraries.load(path, link=False) as (s, d):
        d.objects = list(s.objects)
    ms = [o for o in d.objects if o and o.type == 'MESH']
    o = max(ms, key=lambda o: len(o.data.vertices))
    return o

src = load_mesh(srcf, "S")
dst = load_mesh(dstf, "D")
sm, dm = src.data, dst.data
print(f"SRC {src.name} v={len(sm.vertices)} f={len(sm.polygons)} uv={[l.name for l in sm.uv_layers]}")
print(f"DST {dst.name} v={len(dm.vertices)} f={len(dm.polygons)} uv={[l.name for l in dm.uv_layers]}")

# facce come frozenset di indici vertice
sset = {}
for p in sm.polygons:
    sset.setdefault(frozenset(p.vertices), []).append(p.index)
exact = 0
for p in dm.polygons:
    if frozenset(p.vertices) in sset:
        exact += 1
print(f"facce DST identiche per indici a una faccia SRC: {exact}/{len(dm.polygons)}")

# quanti vertici indice-per-indice hanno lo STESSO fan di facce (proxy topologia)
# confronto rozzo: per i primi min(v) vertici, uguale valenza (n. facce adiacenti)
sval = [0]*len(sm.vertices); dval = [0]*len(dm.vertices)
for p in sm.polygons:
    for vi in p.vertices: sval[vi]+=1
for p in dm.polygons:
    for vi in p.vertices: dval[vi]+=1
n = min(len(sval), len(dval))
same_val = sum(1 for i in range(n) if sval[i]==dval[i])
print(f"primi {n} vertici con stessa valenza (indice-per-indice): {same_val}/{n}")

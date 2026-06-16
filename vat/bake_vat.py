"""
VAT baker headless per zh2 (migrazione_3d.md). Multi-clip incrementale + UV.

Gira DENTRO Blender headless:
  blender --background --python vat/bake_vat.py -- <model.fbx> <out_prefix> <clipname> [--append]

- Senza --append: crea un asset NUOVO (scrive anche <prefix>_mesh.bin e la scala
  globale nel meta). CON --append: carica l'asset esistente e ACCODA la clip
  (stack di frame nelle stesse texture pos/norm, merge del meta) senza rifare le
  altre — workflow incrementale.
- FIX SCALA (§3): bake in coordinate WORLD + normalizzazione altezza in POST. La
  SCALA è calcolata sulla PRIMA clip e RIUSATA per tutte (sennò lo zombie cambia
  taglia fra le animazioni). Grounding piedi a y=0 e centratura XZ per-clip.
- Bake IN-PLACE: toglie l'XZ delle hips per frame. Misura stride_m dal root: con
  clip esportate "In Place" viene ~0 (la fase si guida dal tempo / stride nominale).
- UV-aware: split dei vertici per (orig_idx, u, v) alle cuciture UV (necessario
  con texture). Smooth shading: normali per-vertice (niente split per-normale).

Formato (definito da noi, multi-clip):
  <prefix>_mesh.bin : int nv, int ni; nv*(3f vertID,0,0); nv*(2f u,v); ni*(ushort)
  <prefix>_pos.raw  : RGBA f32, texW x (rowsPerFrame*totalFrames)  [stack di tutte le clip]
  <prefix>_norm.raw : idem
  <prefix>_meta.txt : texW,texH,rowsPerFrame,fps,scale,totalFrames + righe "clip=..."
"""

import bpy, bmesh, sys, os, struct
from mathutils import Vector

TEX_W = 256
TARGET_HEIGHT_M = 1.8
ROOT_BONE = "mixamorig:Hips"


def log(m): print(f"[VAT] {m}")
def to_gl(v): return (v.x, v.z, -v.y)


def patch_fbx_light_import():
    """Blender 5.1 io_scene_fbx crasha importando FBX che contengono LUCI:
    blen_read_light fa `lamp.cycles.cast_shadow = lamp.use_shadow`, ma
    CyclesLightSettings non ha piu' quell'attributo (rimosso). I modelli
    esportati dal giocatore possono portarsi dietro la luce di scena.
    Sostituiamo la funzione con uno stub che ritorna una luce valida senza
    toccare l'attributo morto: tanto al baker servono solo mesh+armatura."""
    try:
        from io_scene_fbx import import_fbx
    except Exception:
        return
    def safe_light(fbx_tmpl, fbx_obj, settings):
        return bpy.data.lights.new(name="ImportedLight", type='POINT')
    import_fbx.blen_read_light = safe_light


def read_meta(path):
    if not os.path.exists(path): return None
    m = {"clips": []}
    for line in open(path):
        line = line.strip()
        if not line: continue
        if line.startswith("clip="):
            d = dict(kv.split("=", 1) for kv in line.split())
            d["name"] = d.pop("clip")
            m["clips"].append(d)
        else:
            k, v = line.split("=", 1)
            m[k] = v
    return m


def write_meta(path, texW, texH, rpf, fps, scale, total, clips):
    with open(path, "w") as f:
        f.write(f"texW={texW}\ntexH={texH}\nrowsPerFrame={rpf}\nfps={fps}\n"
                f"scale={scale:.8f}\ntotalFrames={total}\n")
        for c in clips:
            f.write(f"clip={c['name']} startFrame={c['startFrame']} "
                    f"numFrames={c['numFrames']} duration_s={c['duration_s']} "
                    f"stride_m={c['stride_m']}\n")


def bind_action(arm, action):
    """Assegna la Action all'armatura, gestendo le slotted actions di Blender 4.4+
    (l'azione importata da un FBX d'animazione va ri-bindata all'armatura del modello)."""
    if not arm.animation_data: arm.animation_data_create()
    arm.animation_data.action = action
    ad = arm.animation_data
    if hasattr(action, "slots") and len(action.slots) > 0:
        try: ad.action_slot = action.slots[0]      # Blender 4.4+
        except Exception:
            try: action.slots[0].handle = arm       # variante più vecchia
            except Exception: pass


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    flags = [a for a in argv if a.startswith("--")]
    pos = [a for a in argv if not a.startswith("--")]
    append = "--append" in flags
    def opt(name):
        if name in argv:
            i = argv.index(name)
            return argv[i + 1] if i + 1 < len(argv) else None
        return None
    model_fbx = opt("--model")
    anim_fbx = opt("--anim")
    # opzioni: --model X [--anim Y] <prefix> <clip>   oppure (legacy) <fbx> <prefix> <clip>
    used = {model_fbx, anim_fbx}
    rest = [p for p in pos if p not in used]
    if model_fbx is None:
        model_fbx, prefix, clipname = rest[0], rest[1], rest[2]
    else:
        prefix, clipname = rest[0], rest[1]
    meshp = prefix + "_mesh.bin"; posp = prefix + "_pos.raw"
    normp = prefix + "_norm.raw"; metap = prefix + "_meta.txt"

    bpy.ops.wm.read_factory_settings(use_empty=True)
    patch_fbx_light_import()
    bpy.ops.import_scene.fbx(filepath=model_fbx)
    mesh = next(o for o in bpy.data.objects if o.type == 'MESH')
    arm = next(o for o in bpy.data.objects if o.type == 'ARMATURE')

    if anim_fbx:
        # importa la SOLA animazione (anche senza skin), prende la sua Action e
        # la applica all'armatura del modello (stesso scheletro mixamorig), poi
        # cancella gli oggetti importati dall'anim.
        before_o = set(bpy.data.objects); before_a = set(bpy.data.actions)
        bpy.ops.import_scene.fbx(filepath=anim_fbx)
        new_a = [a for a in bpy.data.actions if a not in before_a]
        action = new_a[0] if new_a else bpy.data.actions[0]
        for o in [o for o in bpy.data.objects if o not in before_o]:
            bpy.data.objects.remove(o, do_unlink=True)
        log(f"anim {anim_fbx.split('/')[-1]!r} -> action {action.name!r} sul rig del modello")
    else:
        action = bpy.data.actions[0]
    bind_action(arm, action)
    start, end = int(action.frame_range[0]), int(action.frame_range[1])
    num_frames = end - start + 1
    scene = bpy.context.scene
    fps = scene.render.fps
    log(f"clip {clipname!r}: action {action.name!r} {start}..{end} ({num_frames}f) @ {fps}fps  append={append}")

    deps = bpy.context.evaluated_depsgraph_get()
    scene.frame_set(start); deps.update()

    # --- topologia con split per-UV (primo frame) ---
    # L'ordine dei vat-index DEVE essere indipendente dalla posa: ogni clip si
    # baka in un processo separato, ma mesh.bin (l'index buffer) lo scrive solo
    # la clip 1 — le clip appese impacchettano pos/norm nel LORO ordine. Se gli
    # ordini divergono, l'index buffer collega vertici sbagliati nelle clip
    # appese -> triangoli spuri. La triangolazione 'BEAUTY' sceglie le diagonali
    # dalla geometria DEFORMATA, quindi a pose diverse riordina i loop_triangles.
    # FIX: assegnare i vat-index dai LOOP della mesh NON triangolata (ordine
    # topologico, identico fra processi e pose); triangolare solo per gli indici.
    em = mesh.evaluated_get(deps); md = em.to_mesh()
    uvl = md.uv_layers.active.data if md.uv_layers.active else None
    uniq = {}              # (orig_idx, u, v) -> vat index
    vat_orig = []          # vat index -> orig blender vert index
    vat_uv = []            # vat index -> (u, v)
    for li in range(len(md.loops)):     # ordine topologico, pose-independent
        oi = md.loops[li].vertex_index
        u, v = (uvl[li].uv.x, uvl[li].uv.y) if uvl else (0.0, 0.0)
        key = (oi, round(u, 5), round(v, 5))
        if key not in uniq:
            uniq[key] = len(vat_orig); vat_orig.append(oi); vat_uv.append((u, v))
    # index buffer: triangola e mappa ogni loop sul vat-index gia' assegnato.
    # La scelta della diagonale dipende dalla posa, ma e' scritta UNA volta sola
    # (clip 1) ed e' connettivita' valida per tutte le pose.
    bm = bmesh.new(); bm.from_mesh(md); bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bm.to_mesh(md); bm.free()
    md.calc_loop_triangles()
    uvl = md.uv_layers.active.data if md.uv_layers.active else None
    indices = []
    for tri in md.loop_triangles:
        for li in tri.loops:
            oi = md.loops[li].vertex_index
            u, v = (uvl[li].uv.x, uvl[li].uv.y) if uvl else (0.0, 0.0)
            indices.append(uniq[(oi, round(u, 5), round(v, 5))])
    num_verts = len(vat_orig)
    em.to_mesh_clear()
    rows = (num_verts + TEX_W - 1) // TEX_W
    log(f"split verts={num_verts} (orig 163) tris={len(indices)//3} rows/frame={rows}")

    # --- bake frame: posizioni/normali world (GL), RAW (centratura in POST) ---
    # Niente in-place via hips qui. Le hips lette da arm.matrix_world possono
    # vivere in una scala diversa dalla mesh (Mixamo: mesh 100 / arm 0.01): su
    # rig mal-scalati il root motion finisce AMPLIFICATO sulla mesh (sia XZ che
    # il "bob" verticale) e le hips non lo riflettono in modo coerente. Quindi
    # togliamo il root motion PER-FRAME dalla geometria stessa — centratura XZ +
    # grounding Y per frame — e la scala dall'altezza PER-FRAME (lo y-span su
    # tutti i frame e' contaminato dal bob amplificato -> scala troppo piccola).
    raw_pos = []; raw_nrm = []
    for fr in range(start, end + 1):
        scene.frame_set(fr); deps.update()
        em = mesh.evaluated_get(deps); md = em.to_mesh()
        nmat = mesh.matrix_world.to_3x3()
        fp = []; fn = []
        for oi in vat_orig:
            v = md.vertices[oi]
            wx, wy, wz = to_gl(mesh.matrix_world @ v.co)
            fp.append((wx, wy, wz))
            n = to_gl(nmat @ v.normal); ln = (n[0]**2+n[1]**2+n[2]**2) ** 0.5 or 1.0
            fn.append((n[0]/ln, n[1]/ln, n[2]/ln))
        raw_pos.append(fp); raw_nrm.append(fn)
        em.to_mesh_clear()

    # offset per-frame: centro XZ del bounding + piede (min Y); span = altezza
    cxf = []; czf = []; yminf = []; spanf = []
    for f in raw_pos:
        xs = [p[0] for p in f]; ys = [p[1] for p in f]; zs = [p[2] for p in f]
        cxf.append((min(xs)+max(xs))*0.5); czf.append((min(zs)+max(zs))*0.5)
        yminf.append(min(ys)); spanf.append(max(ys)-min(ys))

    # --- scala: dalla PRIMA clip (poi riusata da meta), altezza PER-FRAME ---
    meta = read_meta(metap) if append else None
    if meta and "scale" in meta:
        scale = float(meta["scale"])
        if int(meta["rowsPerFrame"]) != rows or int(meta["texW"]) != TEX_W:
            log("ERRORE: mesh/rows incompatibili con l'asset esistente"); sys.exit(1)
    else:
        scale = TARGET_HEIGHT_M / max(spanf)

    def pack(frames):
        out = []
        for fi, f in enumerate(frames):
            cx, cz, ymin = cxf[fi], czf[fi], yminf[fi]
            row = [0.0]*(TEX_W*4*rows)
            for vi, (px, py, pz) in enumerate(f):
                px=(px-cx)*scale; py=(py-ymin)*scale; pz=(pz-cz)*scale
                base=((vi//TEX_W)*TEX_W+(vi%TEX_W))*4
                row[base]=px; row[base+1]=py; row[base+2]=pz; row[base+3]=1.0
            out.append(bytes(struct.pack(f"{len(row)}f", *row)))
        return b"".join(out)
    def packn(frames):
        out=[]
        for f in frames:
            row=[0.0]*(TEX_W*4*rows)
            for vi,(nx,ny,nz) in enumerate(f):
                base=((vi//TEX_W)*TEX_W+(vi%TEX_W))*4
                row[base]=nx;row[base+1]=ny;row[base+2]=nz;row[base+3]=1.0
            out.append(bytes(struct.pack(f"{len(row)}f", *row)))
        return b"".join(out)

    new_pos = pack(raw_pos); new_nrm = packn(raw_nrm)

    # stride: spostamento netto XZ del centroide mesh (start->end), in metri.
    # Robusto per rig ben scalati; su rig mal-scalati il root motion e'
    # amplificato e lo stride risulta inaffidabile -> il layer puo' ripiegare
    # sulla fase-da-tempo (duration_s) come per idle/attack.
    stride = (((cxf[-1]-cxf[0])**2 + (czf[-1]-czf[0])**2) ** 0.5) * scale
    duration = num_frames / fps

    os.makedirs(os.path.dirname(prefix) or ".", exist_ok=True)

    if append and meta:
        clips = meta["clips"]
        prev_total = int(meta["totalFrames"])
        sf = prev_total
        with open(posp, "ab") as f: f.write(new_pos)
        with open(normp, "ab") as f: f.write(new_nrm)
        total = prev_total + num_frames
    else:
        # mesh.bin (una volta sola)
        with open(meshp, "wb") as f:
            f.write(struct.pack("ii", num_verts, len(indices)))
            for i in range(num_verts): f.write(struct.pack("3f", float(i), 0.0, 0.0))
            for (u, v) in vat_uv: f.write(struct.pack("2f", u, v))
            for idx in indices: f.write(struct.pack("H", idx))
        with open(posp, "wb") as f: f.write(new_pos)
        with open(normp, "wb") as f: f.write(new_nrm)
        clips = []; sf = 0; total = num_frames

    clips.append({"name": clipname, "startFrame": str(sf), "numFrames": str(num_frames),
                  "duration_s": f"{duration:.4f}", "stride_m": f"{stride:.4f}"})
    write_meta(metap, TEX_W, total*rows, rows, fps, scale, total, clips)

    log(f"OK clip {clipname!r} -> startFrame={sf} total={total} texH={total*rows} scale={scale:.5f} stride={stride:.4f}")


main()

# Prop cubic projection bake (the prop analog of outfit_bake.py).
# Input: a prop glb + up to 6 side images painted on the prop_template.py
# canvases. The script rebuilds the 6 projection UVs on the fly (same
# Framing6 as the template -> pixel-aligned), blends the sides weighted by
# the surface normal (w_i = max(dot(N, toward camera_i), 0)^sharp, then
# normalized: crisp per-face pick on boxy props, soft blend across 45°
# bevels), and Emit-bakes onto the prop's REAL UV layer. If the glb has no
# UVs (procedural placeholders) a Smart UV Project is generated on the spot.
#
#   blender --background --python gfx/prop_bake.py -- \
#       --glb assets/models/props/bus.glb --images gfx/prop_src/bus \
#       --out gfx/out/props/bus_diffuse.png \
#       [--res 512] [--supersample 2] [--sharp 8] [--scale 1.0] \
#       [--export-glb gfx/out/props/bus.glb] [--preview]
#
# The images dir holds <side>.png or <anything>_<side>.png with side in
# px nx py ny pz nz. Missing side -> falls back to the OPPOSITE side's image
# (projected through, i.e. mirrored: fine for generic surfaces), both
# missing -> flat mid-gray. --export-glb re-exports the mesh WITH the bake
# UVs and a Principled material referencing the baked texture: the glb the
# textured runtime prop loader will consume (load_glb_soup oggi legge solo
# pos+nrm+colore: il path texturato e' il prerequisito annotato in
# CORPSE_DESIGN §10.8 / memoria prop-texture-pipeline).
# --preview renders <out>_prev_<side>.png for eyeballing.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prop_common import (clean_scene, import_glb, world_bbox, Framing6,
                         parse_argv, SIDES, SIDE_ORDER, OPPOSITE)

import bpy


def add_proj_uvs(obj, fr):
    md = obj.data
    mw = obj.matrix_world
    for side in SIDE_ORDER:
        name = 'proj_' + side
        layer = md.uv_layers.get(name) or md.uv_layers.new(name=name)
        for loop in md.loops:
            w = mw @ md.vertices[loop.vertex_index].co
            layer.data[loop.index].uv = fr.uv(w, side)


def ensure_target_uv(obj):
    """Real (non proj_) UV layer, creating a Smart UV Project if absent."""
    md = obj.data
    for l in md.uv_layers:
        if not l.name.startswith('proj_'):
            md.uv_layers.active = l
            l.active_render = True
            return l.name, False
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    # smart project wants edit mode; keep proj_ layers out of the way by
    # creating the target FIRST so it becomes the active receiving layer
    layer = md.uv_layers.new(name='bake_uv')
    md.uv_layers.active = layer
    layer.active_render = True
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(island_margin=0.02)
    bpy.ops.object.mode_set(mode='OBJECT')
    return 'bake_uv', True


def scan_images(idir):
    """dir -> {side: path} accepting <side>.png or *_<side>.png."""
    found = {}
    for f in sorted(os.listdir(idir)):
        low = f.lower()
        if not low.endswith('.png'):
            continue
        stem = low[:-4]
        for side in SIDE_ORDER:
            if stem == side or stem.endswith('_' + side):
                found[side] = os.path.join(idir, f)
    return found


def load_side_images(idir):
    """Six images with opposite-side / gray fallback."""
    found = scan_images(idir)
    if not found:
        raise SystemExit("nessuna immagine <side>.png in " + idir)
    gray = None
    imgs = {}
    for side in SIDE_ORDER:
        path = found.get(side) or found.get(OPPOSITE[side])
        if path:
            imgs[side] = bpy.data.images.load(os.path.abspath(path),
                                              check_existing=True)
            tag = '' if side in found else ' (mirror di %s)' % OPPOSITE[side]
            print("[BAKE] %s <- %s%s" % (side, os.path.basename(path), tag))
        else:
            if gray is None:
                gray = bpy.data.images.new("side_gray", 8, 8, alpha=False)
                gray.generated_color = (0.5, 0.5, 0.5, 1.0)
            imgs[side] = gray
            print("[BAKE] %s <- grigio (nessuna immagine)" % side)
    return imgs


def build_bake_material(imgs, sharp, target_img):
    """Normal-weighted 6-way blend -> Emission; bake target = active node."""
    mat = bpy.data.materials.new("prop_bake")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    n = nt.nodes.new
    L = nt.links.new

    geo = n('ShaderNodeNewGeometry'); geo.location = (-1600, 600)
    col_sum = None
    w_sum = None
    for i, side in enumerate(SIDE_ORDER):
        d, r, u = SIDES[side]
        y = 500 - i * 350
        uv = n('ShaderNodeUVMap'); uv.uv_map = 'proj_' + side
        uv.location = (-1400, y)
        tex = n('ShaderNodeTexImage'); tex.image = imgs[side]
        tex.extension = 'EXTEND'; tex.interpolation = 'Linear'
        tex.location = (-1200, y)
        L(uv.outputs['UV'], tex.inputs['Vector'])
        # w = max(dot(N, -d), 0)^sharp   (-d = toward this side's camera)
        dot = n('ShaderNodeVectorMath'); dot.operation = 'DOT_PRODUCT'
        dot.inputs[1].default_value = (-d.x, -d.y, -d.z)
        dot.location = (-1200, y - 150)
        L(geo.outputs['Normal'], dot.inputs[0])
        mx = n('ShaderNodeMath'); mx.operation = 'MAXIMUM'
        mx.inputs[1].default_value = 0.0; mx.location = (-1000, y - 150)
        L(dot.outputs['Value'], mx.inputs[0])
        pw = n('ShaderNodeMath'); pw.operation = 'POWER'
        pw.inputs[1].default_value = sharp; pw.location = (-850, y - 150)
        L(mx.outputs['Value'], pw.inputs[0])
        # weighted color
        sc = n('ShaderNodeVectorMath'); sc.operation = 'SCALE'
        sc.location = (-700, y)
        L(tex.outputs['Color'], sc.inputs[0])
        L(pw.outputs['Value'], sc.inputs['Scale'])
        if col_sum is None:
            col_sum, w_sum = sc.outputs['Vector'], pw.outputs['Value']
        else:
            ac = n('ShaderNodeVectorMath'); ac.operation = 'ADD'
            ac.location = (-500, y)
            L(col_sum, ac.inputs[0]); L(sc.outputs['Vector'], ac.inputs[1])
            col_sum = ac.outputs['Vector']
            aw = n('ShaderNodeMath'); aw.operation = 'ADD'
            aw.location = (-500, y - 150)
            L(w_sum, aw.inputs[0]); L(pw.outputs['Value'], aw.inputs[1])
            w_sum = aw.outputs['Value']

    inv = n('ShaderNodeMath'); inv.operation = 'DIVIDE'
    inv.inputs[0].default_value = 1.0; inv.location = (-300, -200)
    L(w_sum, inv.inputs[1])
    norm = n('ShaderNodeVectorMath'); norm.operation = 'SCALE'
    norm.location = (-150, 0)
    L(col_sum, norm.inputs[0]); L(inv.outputs['Value'], norm.inputs['Scale'])

    emit = n('ShaderNodeEmission'); emit.location = (50, 0)
    L(norm.outputs['Vector'], emit.inputs['Color'])
    outn = n('ShaderNodeOutputMaterial'); outn.location = (250, 0)
    L(emit.outputs['Emission'], outn.inputs['Surface'])

    tgt = n('ShaderNodeTexImage'); tgt.image = target_img
    tgt.location = (50, -400)
    tgt.select = True
    nt.nodes.active = tgt
    return mat


def preview_render(fr, obj, baked_img, out_prefix, res=512):
    mat = bpy.data.materials.new("prop_preview")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    tex = nt.nodes.new('ShaderNodeTexImage'); tex.image = baked_img
    emit = nt.nodes.new('ShaderNodeEmission')
    outn = nt.nodes.new('ShaderNodeOutputMaterial')
    nt.links.new(tex.outputs['Color'], emit.inputs['Color'])
    nt.links.new(emit.outputs['Emission'], outn.inputs['Surface'])
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    scn = bpy.context.scene
    scn.render.film_transparent = True
    scn.render.image_settings.file_format = 'PNG'
    scn.render.resolution_x = res
    scn.render.resolution_y = res
    for side in SIDE_ORDER:
        scn.camera = fr.camera_for_side(side)
        scn.render.filepath = "%s_prev_%s.png" % (out_prefix, side)
        bpy.ops.render.render(write_still=True)
        print("[BAKE] preview %s -> %s" % (side, scn.render.filepath))


def export_glb(obj, baked_img, path):
    """Re-export the prop WITH bake UVs + a material holding the texture."""
    mat = bpy.data.materials.new("prop_textured")
    mat.use_nodes = True
    nt = mat.node_tree
    bsdf = nt.nodes.get('Principled BSDF')
    tex = nt.nodes.new('ShaderNodeTexImage'); tex.image = baked_img
    nt.links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    # drop the proj_ helper layers from the export
    md = obj.data
    for l in [l for l in md.uv_layers if l.name.startswith('proj_')]:
        md.uv_layers.remove(l)
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    bpy.ops.export_scene.gltf(filepath=os.path.abspath(path),
                              export_format='GLB', use_selection=True)
    print("[BAKE] glb texturato -> %s" % path)


def main():
    opts = parse_argv(sys.argv)
    glb = opts.get('glb')
    idir = opts.get('images')
    out = opts.get('out')
    if not glb or not idir or not out:
        raise SystemExit(
            "uso: --glb prop.glb --images dir --out diffuse.png [--res N]"
            " [--supersample N] [--sharp F] [--scale F]"
            " [--export-glb p.glb] [--preview]")
    res = int(opts.get('res', 512))
    ss = int(opts.get('supersample', 2))
    sharp = float(opts.get('sharp', 8.0))
    scale = float(opts.get('scale', 1.0))

    clean_scene()
    obj = import_glb(glb, scale)
    deps = bpy.context.evaluated_depsgraph_get()
    lo, hi = world_bbox([obj], deps)
    fr = Framing6(lo, hi)
    add_proj_uvs(obj, fr)
    uv_name, generated = ensure_target_uv(obj)
    print("[BAKE] %s  UV di destinazione: %s%s"
          % (os.path.basename(glb), uv_name,
             " (Smart UV Project generata)" if generated else ""))

    imgs = load_side_images(idir)

    scn = bpy.context.scene
    scn.render.engine = 'CYCLES'
    scn.cycles.device = 'CPU'
    scn.cycles.samples = 4              # EMIT bake is noise-free
    scn.render.bake.use_clear = True
    scn.render.bake.margin = 4 * ss

    bres = res * ss
    target = bpy.data.images.new(os.path.basename(out), bres, bres, alpha=False)
    target.generated_color = (0, 0, 0, 1)
    mat = build_bake_material(imgs, sharp, target)
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.bake(type='EMIT')

    if ss > 1:
        target.scale(res, res)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    target.filepath_raw = os.path.abspath(out)
    target.file_format = 'PNG'
    target.save()
    print("[BAKE] OK -> %s (%dx%d)" % (out, res, res))

    if opts.get('preview'):
        preview_render(fr, obj, target, os.path.splitext(out)[0])
    if opts.get('export-glb'):
        export_glb(obj, target, opts['export-glb'])


main()

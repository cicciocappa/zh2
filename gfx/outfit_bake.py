# Outfit projection bake (OUTFIT_DESIGN.md par.3.4, the core of the pipeline).
# Input: a rigged FBX (the VAT-bake source of truth) + front/back collage
# PNGs painted on the outfit_template.py canvases. The script builds the
# projection UVs on the fly (same Framing as the template -> pixel-aligned),
# samples the collages masked by surface normal (soft front/back overlap on
# the sides), and Emit-bakes onto the model's REAL UV layer -> 256x256
# outfit tiles ready for the 4x8 atlas (vat/atlas_tiles.py join).
#
# Single design:
#   blender --background --python gfx/outfit_bake.py -- \
#       --fbx blend/fem_version_obese_rigged.fbx \
#       --front-img design_front.png --back-img design_back.png \
#       --out gfx/out/outfits_fem_obese/outfit_03.png \
#       [--blood-front b_f.png --blood-back b_b.png]   # alpha-over overlay
#
# Batch (one Blender launch, every design of a body):
#   blender --background --python gfx/outfit_bake.py -- \
#       --fbx blend/fem_version_obese_rigged.fbx \
#       --designs gfx/outfit_src/fem_obese --out-dir gfx/out/outfits_fem_obese
#   The designs dir holds NN_fronte.png + NN_retro.png (NN = 00..13, the
#   atlas tile index); each pair bakes outfit_NN.png. If the dir also holds
#   sangue_fronte.png + sangue_retro.png (shared blood overlay, RGBA), every
#   design ALSO bakes the bloodied variant outfit_(NN+16).png (atlas rule:
#   wounded = outfit+16). Tiles 14/15 (charred/corroded status bodies) are
#   normal designs if you provide 14_/15_ collages, or drop hand-made tiles
#   straight into the out dir before the atlas join.
#
# Common options: [--res 256] [--supersample 2] [--soft 0.25] [--front -y|+y]
# [--preview] (renders <tile>_prevF/_prevB.png for eyeballing).
# Bake runs at res*supersample and downscales (cheap anti-aliasing on island
# edges); margin/dilation is on so bilinear sampling does not bleed background.

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from outfit_common import (clean_scene, import_fbx, world_bbox, Framing,
                           parse_argv, front_sign_from_opt)

import bpy


def add_proj_uvs(mesh, fr):
    """Create proj_front/proj_back UV layers from world-space positions."""
    md = mesh.data
    mw = mesh.matrix_world
    for side in ('front', 'back'):
        name = 'proj_' + side
        layer = md.uv_layers.get(name) or md.uv_layers.new(name=name)
        for loop in md.loops:
            w = mw @ md.vertices[loop.vertex_index].co
            layer.data[loop.index].uv = fr.uv(w.x, w.z, side)


def load_img(path):
    return bpy.data.images.load(os.path.abspath(path), check_existing=True)


def build_bake_material(fr, front_img, back_img, soft, target_img,
                        blood_front=None, blood_back=None):
    mat = bpy.data.materials.new("outfit_bake")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    n = nt.nodes.new
    L = nt.links.new

    def side_chain(side, img, blood, x):
        uv = n('ShaderNodeUVMap'); uv.uv_map = 'proj_' + side; uv.location = (x, 0)
        tex = n('ShaderNodeTexImage'); tex.image = img
        tex.extension = 'EXTEND'; tex.interpolation = 'Linear'
        tex.location = (x + 200, 0)
        L(uv.outputs['UV'], tex.inputs['Vector'])
        out = tex.outputs['Color']
        if blood is not None:
            btex = n('ShaderNodeTexImage'); btex.image = blood
            btex.extension = 'EXTEND'; btex.location = (x + 200, -300)
            L(uv.outputs['UV'], btex.inputs['Vector'])
            mix = n('ShaderNodeMix'); mix.data_type = 'RGBA'
            mix.blend_type = 'MIX'; mix.location = (x + 500, 0)
            L(btex.outputs['Alpha'], mix.inputs['Factor'])
            L(out, mix.inputs['A'])
            L(btex.outputs['Color'], mix.inputs['B'])
            out = mix.outputs['Result']
        return out

    front_out = side_chain('front', front_img, blood_front, -1200)
    back_out = side_chain('back', back_img, blood_back, -1200)

    # front/back mask from the world-space shading normal: dot with the
    # front direction, smoothed over [-soft, +soft] (overlap on the sides)
    geo = n('ShaderNodeNewGeometry'); geo.location = (-600, 400)
    dot = n('ShaderNodeVectorMath'); dot.operation = 'DOT_PRODUCT'
    dot.inputs[1].default_value = (0.0, float(fr.front_sign), 0.0)
    dot.location = (-400, 400)
    L(geo.outputs['Normal'], dot.inputs[0])
    ramp = n('ShaderNodeMapRange')
    ramp.inputs['From Min'].default_value = -soft
    ramp.inputs['From Max'].default_value = soft
    ramp.inputs['To Min'].default_value = 1.0   # dot<0 (away from front) -> back
    ramp.inputs['To Max'].default_value = 0.0
    ramp.clamp = True
    ramp.location = (-200, 400)
    L(dot.outputs['Value'], ramp.inputs['Value'])

    mixfb = n('ShaderNodeMix'); mixfb.data_type = 'RGBA'; mixfb.location = (-200, 0)
    L(ramp.outputs['Result'], mixfb.inputs['Factor'])
    L(front_out, mixfb.inputs['A'])
    L(back_out, mixfb.inputs['B'])

    emit = n('ShaderNodeEmission'); emit.location = (100, 0)
    L(mixfb.outputs['Result'], emit.inputs['Color'])
    outn = n('ShaderNodeOutputMaterial'); outn.location = (300, 0)
    L(emit.outputs['Emission'], outn.inputs['Surface'])

    # bake target: the ACTIVE image texture node receives the bake
    tgt = n('ShaderNodeTexImage'); tgt.image = target_img; tgt.location = (100, -400)
    tgt.select = True
    nt.nodes.active = tgt
    return mat


def preview_render(fr, meshes, baked_img, out_prefix, res=512):
    """Render front/back of the model textured with the bake (eyeball check)."""
    mat = bpy.data.materials.new("outfit_preview")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    tex = nt.nodes.new('ShaderNodeTexImage'); tex.image = baked_img
    emit = nt.nodes.new('ShaderNodeEmission')
    outn = nt.nodes.new('ShaderNodeOutputMaterial')
    nt.links.new(tex.outputs['Color'], emit.inputs['Color'])
    nt.links.new(emit.outputs['Emission'], outn.inputs['Surface'])
    for mesh in meshes:
        mesh.data.materials.clear()
        mesh.data.materials.append(mat)
    scn = bpy.context.scene
    scn.render.film_transparent = True
    scn.render.image_settings.file_format = 'PNG'
    scn.render.resolution_x = res
    scn.render.resolution_y = res
    for side, tag in (('front', '_prevF'), ('back', '_prevB')):
        scn.camera = fr.camera_for_side(side)
        scn.render.filepath = out_prefix + tag + ".png"
        bpy.ops.render.render(write_still=True)
        print("[BAKE] preview %s -> %s" % (side, scn.render.filepath))


def setup_body(fbx, front_sign):
    """Import the body once: rest pose, framing, proj UVs, bake-ready UVs."""
    clean_scene()
    meshes, _ = import_fbx(fbx)
    deps = bpy.context.evaluated_depsgraph_get()
    lo, hi = world_bbox(meshes, deps)
    fr = Framing(lo, hi, front_sign=front_sign)
    print("[BAKE] %s  S=%.3f  front_sign=%+d" % (os.path.basename(fbx), fr.S, front_sign))
    for mesh in meshes:
        add_proj_uvs(mesh, fr)
        # bake reads the ACTIVE UV layer -> must be the model's real UV
        real_uv = None
        for l in mesh.data.uv_layers:
            if not l.name.startswith('proj_'):
                real_uv = l; break
        if real_uv is None:
            raise RuntimeError("no real UV layer on %s" % mesh.name)
        mesh.data.uv_layers.active = real_uv
        real_uv.active_render = True

    scn = bpy.context.scene
    scn.render.engine = 'CYCLES'
    scn.cycles.device = 'CPU'
    scn.cycles.samples = 4              # EMIT bake is noise-free
    scn.render.bake.use_clear = False   # multi-mesh bodies share the target
    return meshes, fr


def bake_one(meshes, fr, f_path, b_path, out, res, ss, soft,
             blood_f_path=None, blood_b_path=None, preview=False):
    front_img = load_img(f_path)
    back_img = load_img(b_path)
    blood_f = load_img(blood_f_path) if blood_f_path else None
    blood_b = load_img(blood_b_path) if blood_b_path else None

    bres = res * ss
    target = bpy.data.images.new(os.path.basename(out), bres, bres, alpha=False)
    target.generated_color = (0, 0, 0, 1)
    mat = build_bake_material(fr, front_img, back_img, soft, target,
                              blood_f, blood_b)
    bpy.context.scene.render.bake.margin = 4 * ss

    for mesh in meshes:
        mesh.data.materials.clear()
        mesh.data.materials.append(mat)
        bpy.ops.object.select_all(action='DESELECT')
        mesh.select_set(True)
        bpy.context.view_layer.objects.active = mesh
        bpy.ops.object.bake(type='EMIT')

    if ss > 1:
        target.scale(res, res)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    target.filepath_raw = os.path.abspath(out)
    target.file_format = 'PNG'
    target.save()
    print("[BAKE] OK -> %s (%dx%d)" % (out, res, res))

    if preview:
        preview_render(fr, meshes, target, os.path.splitext(out)[0])
    bpy.data.materials.remove(mat)


def scan_designs(ddir):
    """NN_fronte.png + NN_retro.png pairs -> sorted [(idx, front, back)]."""
    pairs = []
    for f in sorted(os.listdir(ddir)):
        m = re.match(r'^(\d\d)_fronte\.png$', f)
        if not m:
            continue
        idx = int(m.group(1))
        back = os.path.join(ddir, "%02d_retro.png" % idx)
        if not os.path.isfile(back):
            print("[BAKE] ! %s senza %02d_retro.png, salto" % (f, idx))
            continue
        pairs.append((idx, os.path.join(ddir, f), back))
    return pairs


def main():
    opts = parse_argv(sys.argv)
    fbx = opts.get('fbx')
    ddir = opts.get('designs')
    single = opts.get('front-img') and opts.get('back-img') and opts.get('out')
    if not fbx or not (ddir or single):
        raise SystemExit(
            "uso: --fbx m.fbx (--front-img f --back-img b --out tile.png |"
            " --designs dir --out-dir dir) [--res N] [--supersample N]"
            " [--soft F] [--front -y|+y] [--blood-front p --blood-back p]"
            " [--preview]")
    res = int(opts.get('res', 256))
    ss = int(opts.get('supersample', 2))
    soft = float(opts.get('soft', 0.25))
    preview = bool(opts.get('preview'))

    meshes, fr = setup_body(fbx, front_sign_from_opt(opts))

    if ddir:
        out_dir = opts.get('out-dir')
        if not out_dir:
            raise SystemExit("--designs vuole --out-dir")
        blood_f = os.path.join(ddir, "sangue_fronte.png")
        blood_b = os.path.join(ddir, "sangue_retro.png")
        has_blood = os.path.isfile(blood_f) and os.path.isfile(blood_b)
        pairs = scan_designs(ddir)
        if not pairs:
            raise SystemExit("nessun NN_fronte.png/NN_retro.png in " + ddir)
        print("[BAKE] batch: %d design%s" %
              (len(pairs), " + sangue (+16)" if has_blood else ""))
        for idx, f_path, b_path in pairs:
            bake_one(meshes, fr, f_path, b_path,
                     os.path.join(out_dir, "outfit_%02d.png" % idx),
                     res, ss, soft, preview=preview)
            if has_blood and idx + 16 < 32:
                bake_one(meshes, fr, f_path, b_path,
                         os.path.join(out_dir, "outfit_%02d.png" % (idx + 16)),
                         res, ss, soft, blood_f, blood_b, preview=preview)
    else:
        bake_one(meshes, fr, opts['front-img'], opts['back-img'], opts['out'],
                 res, ss, soft, opts.get('blood-front'), opts.get('blood-back'),
                 preview)


main()

# Outfit ControlNet map renderer (companion of outfit_template.py).
# Renders ground-truth DEPTH and camera-space NORMAL maps of a rigged body,
# front and back, with the SAME Framing used by outfit_template.py and
# outfit_bake.py: whatever SDXL+ControlNet generates on top of these maps
# stays pixel-aligned with the projection bake.
#
#   blender --background --python gfx/outfit_control.py -- \
#       --fbx blend/tank_rigged.fbx \
#       --out gfx/out/outfit_tpl/tank [--res 1024] [--front -y]
#
# Output: <out>_depth_front.png, <out>_depth_back.png (white = near, black =
# far/background, ControlNet depth convention) and <out>_normal_front.png,
# <out>_normal_back.png (OpenGL-style camera-space normals, facing-camera =
# (128,128,255); smooth shading is forced so the low-poly facets do not
# leak into the map). Silhouette outlines are NOT rendered here: derive
# them from the template alpha (see the PIL one-liner in OUTFIT_DESIGN.md
# or just run canny on the depth map).

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from outfit_common import (clean_scene, import_fbx, world_bbox, Framing,
                           parse_argv, front_sign_from_opt)

import bpy


def set_engine_eevee(scn):
    for eid in ('BLENDER_EEVEE_NEXT', 'BLENDER_EEVEE'):
        try:
            scn.render.engine = eid
            return
        except TypeError:
            continue
    raise SystemExit("no EEVEE engine available")


def force_smooth(meshes):
    """Smooth shading + drop custom split normals (flat FBX normals would
    make the normal map faceted)."""
    for obj in meshes:
        try:
            bpy.context.view_layer.objects.active = obj
            bpy.ops.mesh.customdata_custom_splitnormals_clear()
        except RuntimeError:
            pass
        for p in obj.data.polygons:
            p.use_smooth = True


def normal_material():
    """Emission = camera-space normal remapped to [0,1] (OpenGL encoding)."""
    mat = bpy.data.materials.new("ctrl_normal")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    geo = nt.nodes.new('ShaderNodeNewGeometry')
    vt = nt.nodes.new('ShaderNodeVectorTransform')
    vt.vector_type = 'NORMAL'
    vt.convert_from = 'WORLD'
    vt.convert_to = 'CAMERA'
    vm = nt.nodes.new('ShaderNodeVectorMath')
    vm.operation = 'MULTIPLY_ADD'
    # Blender's camera-space Z points away from the viewer (facing-camera
    # normal comes out as -Z): negate it so facing-camera = (128,128,255),
    # the OpenGL encoding ControlNet normal expects. X/Y verified correct
    # (image-right = R, up = G).
    vm.inputs[1].default_value = (0.5, 0.5, -0.5)
    vm.inputs[2].default_value = (0.5, 0.5, 0.5)
    em = nt.nodes.new('ShaderNodeEmission')
    out = nt.nodes.new('ShaderNodeOutputMaterial')
    nt.links.new(geo.outputs['Normal'], vt.inputs['Vector'])
    nt.links.new(vt.outputs['Vector'], vm.inputs[0])
    nt.links.new(vm.outputs['Vector'], em.inputs['Color'])
    nt.links.new(em.outputs['Emission'], out.inputs['Surface'])
    return mat


def setup_depth_compositor(scn, dmin, dmax):
    """Composite: Z pass -> linear ramp, dmin -> white, dmax -> black.
    Transparent background has huge Z -> clamps to black. Blender 5.x
    compositor API: node group on scene.compositing_node_group, result on
    the Group Output node, shader-style generic nodes."""
    scn.view_layers[0].use_pass_z = True
    nt = bpy.data.node_groups.new("ctrl_depth_comp", 'CompositorNodeTree')
    nt.interface.new_socket('Image', in_out='OUTPUT',
                            socket_type='NodeSocketColor')
    rl = nt.nodes.new('CompositorNodeRLayers')
    mr = nt.nodes.new('ShaderNodeMapRange')
    mr.clamp = True
    mr.inputs['From Min'].default_value = dmin
    mr.inputs['From Max'].default_value = dmax
    mr.inputs['To Min'].default_value = 1.0
    mr.inputs['To Max'].default_value = 0.0
    go = nt.nodes.new('NodeGroupOutput')
    nt.links.new(rl.outputs['Depth'], mr.inputs['Value'])
    nt.links.new(mr.outputs['Result'], go.inputs['Image'])
    scn.compositing_node_group = nt
    scn.use_nodes = True


def render_side(fr, side, path, res, color_mode):
    scn = bpy.context.scene
    scn.camera = fr.camera_for_side(side)
    scn.render.resolution_x = res
    scn.render.resolution_y = res
    scn.render.film_transparent = True
    scn.render.image_settings.file_format = 'PNG'
    scn.render.image_settings.color_mode = color_mode
    scn.view_settings.view_transform = 'Raw'
    scn.render.filepath = path
    bpy.ops.render.render(write_still=True)
    print("[CTRL] %s -> %s" % (side, path))


def main():
    opts = parse_argv(sys.argv)
    fbx = opts.get('fbx')
    out = opts.get('out')
    if not fbx or not out:
        raise SystemExit("uso: --fbx model.fbx --out prefix [--res N] [--front -y|+y]")
    res = int(opts.get('res', 1024))
    front_sign = front_sign_from_opt(opts)

    clean_scene()
    meshes, _ = import_fbx(fbx)
    deps = bpy.context.evaluated_depsgraph_get()
    lo, hi = world_bbox(meshes, deps)
    fr = Framing(lo, hi, front_sign=front_sign)
    print("[CTRL] bbox x[%.3f %.3f] y[%.3f %.3f] z[%.3f %.3f]  S=%.3f"
          % (lo.x, hi.x, lo.y, hi.y, lo.z, hi.z, fr.S))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    scn = bpy.context.scene
    set_engine_eevee(scn)
    force_smooth(meshes)

    # Depth: camera_for_side sits at dist = bbox diagonal + 1 from center;
    # body surface spans dist -/+ half the extent along the view axis (Y).
    dist = (hi - lo).length + 1.0
    half = (hi.y - lo.y) * 0.5
    pad = max(half * 0.05, 1e-3)
    setup_depth_compositor(scn, dist - half - pad, dist + half + pad)
    render_side(fr, 'front', out + "_depth_front.png", res, 'BW')
    render_side(fr, 'back', out + "_depth_back.png", res, 'BW')

    # Normals: material override, no compositor.
    scn.use_nodes = False
    scn.compositing_node_group = None
    mat = normal_material()
    for obj in meshes:
        obj.data.materials.clear()
        obj.data.materials.append(mat)
    render_side(fr, 'front', out + "_normal_front.png", res, 'RGBA')
    render_side(fr, 'back', out + "_normal_back.png", res, 'RGBA')


main()

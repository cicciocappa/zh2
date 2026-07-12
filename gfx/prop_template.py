# Prop cubic template renderer (the prop analog of outfit_template.py).
# Renders the 6 ortho side views of a prop glb as shaded PNGs with alpha:
# the canvases you paint (or feed to SDXL+ControlNet) the prop textures on.
# The framing is Framing6 from prop_common.py, the same one prop_bake.py
# uses to project the images back -> paint on the template, the pixels land
# on the prop exactly where you put them.
#
#   blender --background --python gfx/prop_template.py -- \
#       --glb assets/models/props/bus.glb \
#       --out gfx/out/prop_tpl/bus [--res 1024] [--scale 1.0]
#
# Output: <out>_px.png _nx _py _ny _pz _nz (+ one line of framing per side).
# --scale = the catalog scale column of the prop (placeholders are 1.0).
# You rarely need all 6 painted: prop_bake falls back to the OPPOSITE side's
# image (mirrored by projection) or to flat gray for missing ones.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prop_common import (clean_scene, import_glb, world_bbox, Framing6,
                         parse_argv, SIDE_ORDER)

import bpy


def render_side(fr, side, path, res):
    scn = bpy.context.scene
    cam = fr.camera_for_side(side)
    scn.camera = cam
    scn.render.resolution_x = res
    scn.render.resolution_y = res
    scn.render.film_transparent = True
    scn.render.image_settings.file_format = 'PNG'
    scn.render.image_settings.color_mode = 'RGBA'
    scn.render.engine = 'BLENDER_WORKBENCH'
    sh = scn.display.shading
    sh.light = 'STUDIO'
    sh.color_type = 'SINGLE'
    sh.single_color = (0.6, 0.6, 0.6)
    sh.show_cavity = True
    sh.cavity_type = 'BOTH'
    sh.show_object_outline = True
    scn.render.filepath = path
    bpy.ops.render.render(write_still=True)
    print("[TPL] %s  S=%.3f m  %.1f px/m -> %s"
          % (side, fr.S[side], res / fr.S[side], path))


def main():
    opts = parse_argv(sys.argv)
    glb = opts.get('glb')
    out = opts.get('out')
    if not glb or not out:
        raise SystemExit("uso: --glb prop.glb --out prefix [--res N] [--scale F]")
    res = int(opts.get('res', 1024))
    scale = float(opts.get('scale', 1.0))

    clean_scene()
    import_glb(glb, scale)
    deps = bpy.context.evaluated_depsgraph_get()
    lo, hi = world_bbox([bpy.context.view_layer.objects.active], deps)
    fr = Framing6(lo, hi)
    print("[TPL] bbox x[%.3f %.3f] y[%.3f %.3f] z[%.3f %.3f]"
          % (lo.x, hi.x, lo.y, hi.y, lo.z, hi.z))

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    for side in SIDE_ORDER:
        render_side(fr, side, "%s_%s.png" % (out, side), res)


main()

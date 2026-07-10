# Outfit collage template renderer (OUTFIT_DESIGN.md par.3.3).
# Renders the FRONT and BACK ortho views of a rigged body (rest pose) as
# shaded PNGs with alpha: the canvas you collage clothes onto in GIMP.
# The framing is Framing() from outfit_common.py, the same one
# outfit_bake.py uses to project the collages back -> paint on the
# template, the pixels land on the body exactly where you put them.
#
#   blender --background --python gfx/outfit_template.py -- \
#       --fbx blend/fem_version_obese_rigged.fbx \
#       --out gfx/out/outfit_tpl/fem_obese [--res 1024] [--front -y]
#
# Output: <out>_front.png, <out>_back.png (+ one line of framing info).
# Both views are rendered "as seen": the back view is the character turned
# around, so a design painted on the LEFT sleeve in both images stays on
# the same arm.

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from outfit_common import (clean_scene, import_fbx, world_bbox, Framing,
                           parse_argv, front_sign_from_opt)

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
    print("[TPL] %s -> %s" % (side, path))


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
    print("[TPL] bbox x[%.3f %.3f] y[%.3f %.3f] z[%.3f %.3f]  S=%.3f m  %.1f px/m"
          % (lo.x, hi.x, lo.y, hi.y, lo.z, hi.z, fr.S, res / fr.S))

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    render_side(fr, 'front', out + "_front.png", res)
    render_side(fr, 'back', out + "_back.png", res)


main()

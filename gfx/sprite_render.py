# Sprite prerender pipeline (GFX_DESIGN.md §4). Runs INSIDE Blender:
#
#   blender --background --python gfx/sprite_render.py -- [options]
#
# Builds the single shared scene rig (orthographic camera ~45° from the
# horizon, key sun from screen NW, cool rim light from behind) and renders
# a character to per-frame PNGs: 16 directions x N frames x actions, with
# transparent background. gfx/sheet_pack.py then packs them into sheets.
#
# Character source:
#   --fbx model.fbx          Mixamo "FBX Binary, with skin" character
#   --anim walk=walk.fbx     extra Mixamo animations, "without skin"
#                            (repeatable; name before '=' becomes the action
#                            name; frame count after ':' optional, e.g.
#                            walk=walk.fbx:8 — default 8)
#   (no --fbx)               built-in animated placeholder pawn, validates
#                            the whole chain without any asset
#
# Conventions (the game renderer relies on these):
#   - direction row k = character heading k*22.5deg CLOCKWISE from "facing
#     the camera" (screen-down). Row 0 faces the viewer.
#   - world origin (0,0,0) = character ground anchor; its pixel position is
#     written to meta.json as "anchor" (same for every frame by construction).
#   - 1 world meter = px/frame_m pixels (default 256/2.0 = 128 px/m, i.e.
#     4x supersampling over the 32 px/m tactical tier; sheet_pack downscales).
#
# Output: <out>/<action>/d<dir>_f<frame>.png + <out>/meta.json

import bpy
import json
import math
import os
import sys
from mathutils import Vector


# ---------------------------------------------------------------- arguments

def parse_args():
    import argparse
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser(prog="sprite_render.py")
    p.add_argument("--fbx", default=None, help="character FBX (with skin)")
    p.add_argument("--anim", action="append", default=[],
                   help="name=file.fbx[:frames] extra animation (no skin)")
    p.add_argument("--out", default="gfx/out/placeholder")
    p.add_argument("--dirs", type=int, default=16)
    p.add_argument("--frames", type=int, default=8,
                   help="default samples per action (placeholder/--fbx walk)")
    p.add_argument("--px", type=int, default=256, help="render resolution")
    p.add_argument("--frame-m", type=float, default=2.0,
                   help="world meters covered by the frame")
    p.add_argument("--height", type=float, default=1.8,
                   help="normalize character to this height in meters")
    p.add_argument("--elev", type=float, default=45.0,
                   help="camera elevation from horizon, degrees")
    p.add_argument("--aim-z", type=float, default=0.8,
                   help="camera aim height (frame vertical centering)")
    p.add_argument("--model-yaw", type=float, default=0.0,
                   help="extra yaw (deg) to make the model face the camera "
                        "at direction 0")
    p.add_argument("--samples", type=int, default=32)
    return p.parse_args(argv)


# ---------------------------------------------------------------- scene rig

def build_rig(args):
    """The ONE light+camera rig shared by every asset (GFX_DESIGN.md §4)."""
    scn = bpy.context.scene

    # renderer: Cycles CPU — works headless everywhere, sprites are cheap
    scn.render.engine = "CYCLES"
    scn.cycles.samples = args.samples
    scn.cycles.use_denoising = True
    scn.render.resolution_x = args.px
    scn.render.resolution_y = args.px
    scn.render.film_transparent = True
    scn.render.image_settings.file_format = "PNG"
    scn.render.image_settings.color_mode = "RGBA"
    # sprites must not go through a filmic tonemap: flat sRGB out
    scn.view_settings.view_transform = "Standard"

    # camera aim: empty at (0, 0, aim_z) so the frame is vertically centered
    # on the character, while (0,0,0) stays the ground anchor
    aim = bpy.data.objects.new("AIM", None)
    aim.location = (0.0, 0.0, args.aim_z)
    scn.collection.objects.link(aim)

    # orthographic camera south of the character, elevated; looks at AIM.
    # screen-up = world +Y (north), so "NW key light" = light from (-X, +Y).
    cam_data = bpy.data.cameras.new("CAM")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = args.frame_m
    cam = bpy.data.objects.new("CAM", cam_data)
    elev = math.radians(args.elev)
    dist = 20.0
    cam.location = (0.0, -dist * math.cos(elev), dist * math.sin(elev))
    tt = cam.constraints.new("TRACK_TO")
    tt.target = aim
    tt.track_axis = "TRACK_NEGATIVE_Z"
    tt.up_axis = "UP_Y"
    scn.collection.objects.link(cam)
    scn.camera = cam

    def sun(name, azim_deg, elev_deg, energy, color):
        d = bpy.data.lights.new(name, "SUN")
        d.energy = energy
        d.color = color
        d.angle = math.radians(5)  # soft-ish shadow edges
        o = bpy.data.objects.new(name, d)
        # sun objects only need orientation: -Z points along the light
        o.rotation_euler = (math.radians(90 - elev_deg), 0.0,
                            math.radians(azim_deg))
        scn.collection.objects.link(o)
        return o

    # azimuth here: 0 = light travelling from north to south, measured so
    # that +45 puts the source in the world NW octant (screen upper-left)
    sun("KEY", 45.0, 50.0, 4.0, (1.0, 0.96, 0.90))     # warm key from NW
    sun("FILL", -135.0, 30.0, 0.8, (0.75, 0.80, 1.0))  # cool fill from SE
    # rim/backlight: from straight behind (north), low — bakes the rim that
    # pops the silhouette off the terrain (GFX_DESIGN.md §4)
    sun("RIM", 0.0, 15.0, 2.5, (0.85, 0.90, 1.0))

    # flat dark world so the rim has something to win against
    world = bpy.data.worlds.new("WORLD")
    world.use_nodes = True
    bg = world.node_tree.nodes["Background"]
    bg.inputs[0].default_value = (0.05, 0.05, 0.06, 1.0)
    bg.inputs[1].default_value = 0.3
    scn.world = world


# ------------------------------------------------------------- placeholder

def build_placeholder():
    """Animated pawn: cone body + head + nose wedge marking 'forward'.
    Forward = -Y (toward the camera at direction 0). Returns (root, actions)
    where actions maps name -> (frame_start, frame_end)."""
    def mesh_obj(name, op, **kw):
        op(**kw)
        o = bpy.context.active_object
        o.name = name
        return o

    body = mesh_obj("body", bpy.ops.mesh.primitive_cone_add,
                    radius1=0.35, radius2=0.18, depth=1.2,
                    location=(0, 0, 0.6))
    head = mesh_obj("head", bpy.ops.mesh.primitive_uv_sphere_add,
                    radius=0.28, location=(0, 0, 1.45))   # oversized head:
    nose = mesh_obj("nose", bpy.ops.mesh.primitive_cube_add,  # reads at 20px
                    size=0.18, location=(0, -0.30, 1.45))

    mat = bpy.data.materials.new("pawn")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (0.25, 0.45, 0.20, 1.0)
    nmat = bpy.data.materials.new("nose")
    nmat.use_nodes = True
    nmat.node_tree.nodes["Principled BSDF"].inputs["Base Color"] \
        .default_value = (0.8, 0.15, 0.1, 1.0)
    for o in (body, head):
        o.data.materials.append(mat)
    nose.data.materials.append(nmat)

    root = bpy.data.objects.new("CHAR_ROOT", None)
    bpy.context.scene.collection.objects.link(root)
    for o in (body, head, nose):
        o.parent = root

    # fake 8-frame walk: two bob cycles + a little roll, loops cleanly
    for f in range(1, 9):
        t = (f - 1) / 8.0
        root.location.z = 0.05 * abs(math.sin(t * math.tau))
        root.rotation_euler.x = math.radians(4) * math.sin(t * math.tau)
        root.keyframe_insert("location", frame=f)
        root.keyframe_insert("rotation_euler", frame=f)
    return root, {"walk": (1, 8)}


# ------------------------------------------------------------- FBX import

def import_character(args):
    """Mixamo FBX character + extra animation FBXs. Returns (root, actions),
    actions maps name -> (frame_start, frame_end, sample_count)."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=args.fbx)
    imported = [o for o in bpy.data.objects if o not in before]
    arma = next(o for o in imported if o.type == "ARMATURE")

    root = bpy.data.objects.new("CHAR_ROOT", None)
    bpy.context.scene.collection.objects.link(root)
    for o in imported:
        if o.parent is None:
            o.parent = root

    # normalize height (Mixamo FBX scale varies): measure rest bounding box
    zs = []
    for o in imported:
        if o.type == "MESH":
            zs += [(o.matrix_world @ Vector(c)).z for c in o.bound_box]
    if zs:
        root.scale = (args.height / max(zs),) * 3

    actions = {}
    if arma.animation_data and arma.animation_data.action:
        a = arma.animation_data.action
        actions["base"] = (a, args.frames)

    # extra animations: import, steal the action, delete the rest
    for spec in args.anim:
        name, _, rest = spec.partition("=")
        path, _, nstr = rest.partition(":")
        n = int(nstr) if nstr else args.frames
        before = set(bpy.data.objects)
        prev_actions = set(bpy.data.actions)
        bpy.ops.import_scene.fbx(filepath=path)
        new_act = next(a for a in bpy.data.actions if a not in prev_actions)
        new_act.name = name
        actions[name] = (new_act, n)
        for o in set(bpy.data.objects) - before:
            bpy.data.objects.remove(o, do_unlink=True)

    # resolve to frame ranges keyed by action name; bind happens at render
    out = {}
    for name, (act, n) in actions.items():
        f0, f1 = act.frame_range
        out[name] = {"action": act, "armature": arma,
                     "range": (f0, f1), "samples": n}
    return root, out


# ------------------------------------------------------- root motion fix

def measure_drift(arma, f0, f1, nsmp):
    """World XY of the hips at each sampled frame. Mixamo clips are often
    NOT in place (root motion lives on mixamorig:Hips): cancelling these
    offsets per frame keeps the character centered on the ground anchor.
    Must run with TURNTABLE at identity and CHAR_ROOT at the origin."""
    scn = bpy.context.scene
    dg = bpy.context.evaluated_depsgraph_get()
    hips = next((pb.name for pb in arma.pose.bones
                 if "hips" in pb.name.lower()), None)
    if hips is None:
        print("[sprite_render] WARNING: no hips bone, root motion not fixed")
        return [(0.0, 0.0)] * nsmp
    offs = []
    for j in range(nsmp):
        scn.frame_set(int(round(f0 + (f1 - f0) * j / nsmp)))
        ev = arma.evaluated_get(dg)
        w = ev.matrix_world @ ev.pose.bones[hips].head
        offs.append((w.x, w.y))
    drift = math.hypot(offs[-1][0] - offs[0][0], offs[-1][1] - offs[0][1])
    print(f"[sprite_render] root drift over clip: {drift:.2f} m")
    return offs


# ----------------------------------------------------------------- render

def render_all(args, root, actions):
    scn = bpy.context.scene
    os.makedirs(args.out, exist_ok=True)

    # direction spin lives on a dedicated, never-animated parent: keyframes
    # on the character root (or armature) would overwrite a rotation set
    # directly on it at every frame_set
    table = bpy.data.objects.new("TURNTABLE", None)
    scn.collection.objects.link(table)
    root.parent = table

    meta = {
        "px": args.px, "frame_m": args.frame_m, "dirs": args.dirs,
        "px_per_m": args.px / args.frame_m,
        # pixel of world (0,0,0): image center is the AIM point, the ground
        # anchor sits aim_z * cos(elev) world-meters below it on screen
        "anchor": [args.px / 2.0,
                   args.px / 2.0 + args.aim_z * math.cos(math.radians(args.elev))
                   * args.px / args.frame_m],
        "k_z": math.cos(math.radians(args.elev)),  # flight z -> screen px/m
        "actions": {},
    }

    for name, spec in actions.items():
        if isinstance(spec, tuple):                       # placeholder
            f0, f1 = spec
            nsmp = args.frames
            offs = None        # root itself is keyframed, and never drifts
        else:                                             # fbx action
            f0, f1 = spec["range"]
            nsmp = spec["samples"]
            arma = spec["armature"]
            if arma.animation_data is None:
                arma.animation_data_create()
            arma.animation_data.action = spec["action"]
            table.rotation_euler.z = 0.0   # measure in character space
            offs = measure_drift(arma, f0, f1, nsmp)
        meta["actions"][name] = {"frames": nsmp}

        for k in range(args.dirs):
            # row k = heading k*22.5deg clockwise from facing-camera
            table.rotation_euler.z = math.radians(-k * 360.0 / args.dirs
                                                  + args.model_yaw)
            for j in range(nsmp):
                # sample [f0, f1) evenly — endpoint excluded so loops loop
                scn.frame_set(int(round(f0 + (f1 - f0) * j / nsmp)))
                if offs:
                    # cancel root motion: root.location lives in turntable
                    # space, so the correction rotates with the direction
                    root.location.x = -offs[j][0]
                    root.location.y = -offs[j][1]
                scn.render.filepath = os.path.join(
                    args.out, name, f"d{k:02d}_f{j:02d}.png")
                bpy.ops.render.render(write_still=True)

    with open(os.path.join(args.out, "meta.json"), "w") as fp:
        json.dump(meta, fp, indent=2)
    print(f"[sprite_render] done: {args.out}")


def main():
    args = parse_args()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    build_rig(args)
    if args.fbx:
        root, actions = import_character(args)
    else:
        root, actions = build_placeholder()
    render_all(args, root, actions)


main()

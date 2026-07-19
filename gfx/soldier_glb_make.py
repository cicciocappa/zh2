# soldier_glb_make.py — build assets/models/soldier.glb from the rigged FBX
# plus the Mixamo animation clips (SOLDIER_DESIGN skeletal pipeline, model.c).
#
# Run headless from the project root:
#   ~/Scaricati/blender-5.2.0-linux-x64/blender --background --python gfx/soldier_glb_make.py
#
# Steps:
#   1. import blend/soldier_medium_rigged.fbx (T-pose rig + skinned mesh)
#   2. import each Mixamo clip FBX, steal its action, verify it targets the
#      same skeleton (shared "mixamorig:" bone names -> retarget is by name,
#      no remap needed), rename it idle/run
#   3. material: if blend/soldier_diffuse.png + soldier_atlas_uv.bin exist
#      (built once by gfx/soldier_bake.py from the ORIGINAL downloaded model),
#      rewrite the mesh UVs with the atlas layout by loop index (topology and
#      loop order survive Mixamo bit-exactly — verified) and texture the
#      soldier; otherwise fall back to a flat olive material. Then push each
#      clip to its own NLA track named idle/run and export a GLB with
#      export_animation_mode='NLA_TRACKS' -> one glTF animation per track,
#      named after the track (the names vat_horde looks up)
#
# Scale convention: the FBX imports with a 0.01 unit scale (and the axis
# rotation) on the armature object. We DELIBERATELY do NOT apply it — it rides
# along as the root node transform, which model.c now honors as the skeleton
# root_pre (see MdlSkeleton.root_pre): the spec-correct render lands exactly
# in the mesh bbox space, and vat_horde auto-fits that bbox to ~1.8 m.
# Baking the 0.01 into the data instead produced a 1.8 cm soldier (the pose
# fcurves don't get rescaled), so leave the node transform alone.
#
# Blender 5.2 note: the slotted-actions API removed Action.fcurves/.groups;
# fcurves now live under action.layers[].strips[].channelbags[].fcurves.

import bpy
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_RIG = os.path.join(ROOT, "blend", "soldier_medium_rigged.fbx")
# Directional runs (twin-stick legs): Mixamo's pack download has no "In
# Place" option, so these clips carry root motion — cancel_root_motion()
# strips the drift from the Hips location fcurves before export.
CLIPS = [
    ("idle",      os.path.join(ROOT, "blend", "Rifle Idle.fbx")),
    ("run",       os.path.join(ROOT, "blend", "Run Forward.fbx")),
    ("run_back",  os.path.join(ROOT, "blend", "Run Backwards.fbx")),
    ("run_left",  os.path.join(ROOT, "blend", "Run Left.fbx")),
    ("run_right", os.path.join(ROOT, "blend", "Run Right.fbx")),
    # diagonali (8 settori), lancio granata da fermo, sparo da fermo
    ("run_fl",    os.path.join(ROOT, "blend", "Run Forward Left.fbx")),
    ("run_fr",    os.path.join(ROOT, "blend", "Run Forward Right.fbx")),
    ("run_bl",    os.path.join(ROOT, "blend", "Run Backward Left.fbx")),
    ("run_br",    os.path.join(ROOT, "blend", "Run Backward Right.fbx")),
    ("toss",      os.path.join(ROOT, "blend", "Toss Grenade.fbx")),
    ("fire",      os.path.join(ROOT, "blend", "Firing Rifle.fbx")),
    # scavalcamento (SOLDIER_DESIGN): salita sul muro + salto giù dall'altra
    # parte. Root motion KEPT (see KEEP_ROOT below).
    ("climb",     os.path.join(ROOT, "blend", "Climbing.fbx")),
    ("jump_down", os.path.join(ROOT, "blend", "Jumping Down.fbx")),
]

# Clips whose root motion IS the move (the climb rise + mantle-over, the jump
# drop): keep it — the host anchors the model at the wall base / wall top and
# the baked hips motion carries the mesh up and over. Only the horizontal
# start offset is rebased to zero so the clip begins on the anchor.
KEEP_ROOT = {"climb", "jump_down"}
# Frame windows (inclusive start, exclusive-ish end, None = keep to the end).
# Climbing ships with a ~24-frame run-up (2.7 m of forward travel) that would
# walk the soldier INTO the wall — cut at the pre-jump crouch. Jumping Down
# has ~0.4 s of dead standing at the head and ~1 s of idle tail past the
# landing recovery (hips settle by frame 72).
TRIM = {"climb": (24, None), "jump_down": (12, 72)}
OUT = os.path.join(ROOT, "assets", "models", "soldier.glb")
DIFFUSE = os.path.join(ROOT, "blend", "soldier_diffuse.png")
ATLAS_UV = os.path.join(ROOT, "blend", "soldier_atlas_uv.bin")

MIN_BONE_MATCH = 0.90            # fraction of clip bones that must exist in rig
MAT_COLOR = (0.24, 0.28, 0.20, 1.0)   # olive drab, so the GLB carries a color

_BONE_RE = re.compile(r'pose\.bones\["([^"]+)"\]')


def die(msg):
    print("FATAL: " + msg)
    sys.exit(1)


def action_fcurves(act):
    """All fcurves of an action, across the 5.2 slotted-action layout."""
    fcs = []
    for layer in act.layers:
        for strip in layer.strips:
            for cbag in strip.channelbags:
                fcs.extend(cbag.fcurves)
    return fcs


def action_bone_names(act):
    names = set()
    for fc in action_fcurves(act):
        m = _BONE_RE.match(fc.data_path)
        if m:
            names.add(m.group(1))
    return names


DRIFT_MIN = 0.05     # units of net drift before an axis counts as locomotion

def cancel_root_motion(act, clip_name):
    """In-place-ify a Mixamo clip: on the Hips location fcurves, an axis with
    net first->last drift is the locomotion axis — subtract the linear ramp
    (loops again) and zero the mean (recenter). Bob/sway axes untouched."""
    for fc in action_fcurves(act):
        if not (fc.data_path.endswith('"mixamorig:Hips"].location')
                or fc.data_path.endswith("'mixamorig:Hips'].location")):
            continue
        kps = fc.keyframe_points
        n = len(kps)
        if n < 2:
            continue
        v0, v1 = kps[0].co[1], kps[n - 1].co[1]
        if abs(v1 - v0) < DRIFT_MIN:
            continue
        t0, t1 = kps[0].co[0], kps[n - 1].co[0]
        span = (t1 - t0) if t1 > t0 else 1.0
        vals = []
        for k in kps:
            ramp = v0 + (v1 - v0) * (k.co[0] - t0) / span
            vals.append(k.co[1] - ramp)
        mean = sum(vals) / n
        for k, v in zip(kps, vals):
            k.co[1] = v - mean
            k.handle_left[1] = k.co[1]
            k.handle_right[1] = k.co[1]
        print("%s: cancelled root motion on hips axis %d (drift %.2f)"
              % (clip_name, fc.array_index, v1 - v0))


def trim_action(act, f0, f1, clip_name):
    """Cut every fcurve to [f0, f1] (Mixamo bakes a key per frame, so f0/f1
    land on existing keys) and shift left so the clip starts at frame 0."""
    for fc in action_fcurves(act):
        kps = fc.keyframe_points
        for i in range(len(kps) - 1, -1, -1):
            f = kps[i].co[0]
            if f < f0 or (f1 is not None and f > f1):
                kps.remove(kps[i], fast=True)
        for k in kps:
            k.co[0] -= f0
            k.handle_left[0] -= f0
            k.handle_right[0] -= f0
        fc.update()
    fr = act.frame_range
    print("%s: trimmed to frames %.0f..%.0f (was %s..%s)"
          % (clip_name, fr[0], fr[1], f0, f1 if f1 is not None else "end"))


def rebase_root_start(act, clip_name):
    """KEEP_ROOT clips: zero the HORIZONTAL start offset of the hips (axes 0
    and 2 — axis 1 is the vertical rise/drop, measured on both clips) so the
    baked travel starts on the model anchor. Print the net motion: the host
    hardcodes these as SOL_CLIMB_*/SOL_JUMP_* anchor constants."""
    for fc in action_fcurves(act):
        if not (fc.data_path.endswith('"mixamorig:Hips"].location')
                or fc.data_path.endswith("'mixamorig:Hips'].location")):
            continue
        kps = fc.keyframe_points
        if len(kps) < 2:
            continue
        if fc.array_index != 1:
            v0 = kps[0].co[1]
            for k in kps:
                k.co[1] -= v0
                k.handle_left[1] -= v0
                k.handle_right[1] -= v0
            fc.update()
        print("%s: hips axis %d  start %.3f  end %.3f  (net %.3f)"
              % (clip_name, fc.array_index, kps[0].co[1],
                 kps[len(kps) - 1].co[1], kps[len(kps) - 1].co[1] - kps[0].co[1]))


def import_fbx(path):
    if not os.path.isfile(path):
        die("missing file: " + path)
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path, use_anim=True)
    return [o for o in bpy.data.objects if o not in before]


def find_armature(objs, label):
    arms = [o for o in objs if o.type == 'ARMATURE']
    if len(arms) != 1:
        die("%s: expected 1 armature, found %d" % (label, len(arms)))
    return arms[0]


def main():
    bpy.ops.wm.read_factory_settings(use_empty=True)

    # --- 1. base rig ---
    rig_objs = import_fbx(SRC_RIG)
    arm = find_armature(rig_objs, "rig")
    meshes = [o for o in rig_objs if o.type == 'MESH']
    if not meshes:
        die("rig: no meshes imported")
    arm.animation_data_clear()          # drop the rest-pose action if any

    bones = {b.name for b in arm.data.bones}
    print("rig: %d bones, %d mesh(es): %s"
          % (len(bones), len(meshes),
             ", ".join("%s(%dv)" % (m.name, len(m.data.vertices)) for m in meshes)))

    s = arm.scale[0]
    if abs(arm.scale[1] - s) > 1e-6 or abs(arm.scale[2] - s) > 1e-6:
        die("rig armature scale is non-uniform: %r" % (tuple(arm.scale),))

    # --- 2. clips ---
    actions = []
    for clip_name, path in CLIPS:
        clip_objs = import_fbx(path)
        carm = find_armature(clip_objs, clip_name)
        if not (carm.animation_data and carm.animation_data.action):
            die(clip_name + ": no action on imported armature")
        act = carm.animation_data.action

        cbones = action_bone_names(act)
        matched = cbones & bones
        frac = len(matched) / max(1, len(cbones))
        fr = act.frame_range
        print("%s: %d animated bones, %d match rig (%.0f%%), frames %.0f..%.0f"
              % (clip_name, len(cbones), len(matched), frac * 100.0, fr[0], fr[1]))
        if frac < MIN_BONE_MATCH:
            die(clip_name + ": bone-name match below threshold — not the same "
                "Mixamo skeleton as the rig?")

        if clip_name in TRIM:
            trim_action(act, TRIM[clip_name][0], TRIM[clip_name][1], clip_name)
        if clip_name in KEEP_ROOT:
            rebase_root_start(act, clip_name)
        else:
            cancel_root_motion(act, clip_name)
        act.name = clip_name
        act.use_fake_user = True
        # detach so removing the clip's objects doesn't take the action with them
        carm.animation_data_clear()
        for o in clip_objs:
            bpy.data.objects.remove(o, do_unlink=True)
        actions.append(act)

    # NOTE: no transform_apply / no fcurve rescale — see the scale-convention
    # note at the top. The 0.01 armature scale rides along as a node scale that
    # model.c ignores; the raw meter-scale data is what animates.

    # --- 3. material + NLA tracks + export ---
    textured = os.path.isfile(DIFFUSE) and os.path.isfile(ATLAS_UV)
    if textured:
        # atlas UVs (gfx/soldier_bake.py): float32 u,v per loop, same loop
        # order as this FBX — rewrite the UV layer in place
        with open(ATLAS_UV, "rb") as f:
            magic, count = struct.unpack("<II", f.read(8))
            if magic != 0x56555341:
                die("bad magic in " + ATLAS_UV)
            flat = struct.unpack("<%df" % (count * 2), f.read(count * 2 * 4))
        total = sum(len(m.data.loops) for m in meshes)
        if total != count:
            die("atlas uv count %d != mesh loops %d" % (count, total))
        base = 0
        for m in meshes:
            uvl = m.data.uv_layers[0]
            n = len(m.data.loops)
            uvl.data.foreach_set("uv", flat[base * 2:(base + n) * 2])
            base += n
    mat = bpy.data.materials.new("soldier_diffuse" if textured else "soldier_flat")
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes.get("Principled BSDF")
    if bsdf:
        bsdf.inputs["Base Color"].default_value = MAT_COLOR
        if "Metallic" in bsdf.inputs:
            bsdf.inputs["Metallic"].default_value = 0.0
        if "Roughness" in bsdf.inputs:
            bsdf.inputs["Roughness"].default_value = 0.9
        if textured:
            tex = mat.node_tree.nodes.new("ShaderNodeTexImage")
            tex.image = bpy.data.images.load(DIFFUSE)
            mat.node_tree.links.new(tex.outputs["Color"],
                                    bsdf.inputs["Base Color"])
    print("material: %s" % ("diffuse atlas" if textured else
                            "flat olive (run gfx/soldier_bake.py for texture)"))
    for m in meshes:
        m.data.materials.clear()
        m.data.materials.append(mat)

    ad = arm.animation_data_create()
    ad.action = None
    for act in actions:
        track = ad.nla_tracks.new()
        track.name = act.name
        track.strips.new(act.name, int(act.frame_range[0]), act)
        track.mute = False

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=OUT,
        export_format='GLB',
        export_animations=True,
        export_animation_mode='NLA_TRACKS',
        export_skins=True,
        export_apply=False,
        export_yup=True,
    )
    print("wrote %s (%.1f KiB)" % (OUT, os.path.getsize(OUT) / 1024.0))


main()

# Shared logic for the outfit projection-bake pipeline (OUTFIT_DESIGN.md).
# Runs INSIDE Blender (bpy). Both outfit_template.py (renders the front/back
# collage templates) and outfit_bake.py (projects the collages back onto the
# real UVs) import this module, so the framing is identical by construction:
# a template painted for body X lines up with the bake of body X exactly.
#
# Conventions (must match vat/bake_vat.py):
# - Source of truth is the rigged FBX that feeds the VAT bake; plain
#   bpy.ops.import_scene.fbx, world-space coordinates via matrix_world.
# - Character is framed in REST pose (armatures set to pose_position='REST').
# - Front/back projection along world Y. front_sign = -1 means the character
#   faces -Y (front camera sits at -Y looking +Y), which is the usual Mixamo
#   import orientation; override with --front +y if a model faces the other way.
#
# Projection math (front_sign = s, s in {-1,+1}):
#   front image: u = 0.5 + s*(x-cx)/S   v = 0.5 + (z-cz)/S
#   back  image: u = 0.5 - s*(x-cx)/S   v = 0.5 + (z-cz)/S
# S = max bbox extent * (1+2*margin). v is Blender-image style (0 = bottom).
# An ortho camera placed by camera_for_side() renders EXACTLY this mapping.

import math
import bpy
from mathutils import Vector


def clean_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_fbx(path):
    """Import an FBX and return (mesh_objects, all_new_objects)."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path)
    new = [o for o in bpy.data.objects if o not in before]
    meshes = [o for o in new if o.type == 'MESH']
    if not meshes:
        raise RuntimeError("no mesh object in %s" % path)
    for o in new:
        if o.type == 'ARMATURE':
            o.data.pose_position = 'REST'   # frame/bake the bind pose
    return meshes, new


def world_bbox(meshes, deps):
    """World-space bbox of the evaluated meshes (rest pose)."""
    lo = Vector((1e30, 1e30, 1e30))
    hi = Vector((-1e30, -1e30, -1e30))
    for mesh in meshes:
        em = mesh.evaluated_get(deps)
        md = em.to_mesh()
        mw = mesh.matrix_world
        for v in md.vertices:
            w = mw @ v.co
            lo.x = min(lo.x, w.x); lo.y = min(lo.y, w.y); lo.z = min(lo.z, w.z)
            hi.x = max(hi.x, w.x); hi.y = max(hi.y, w.y); hi.z = max(hi.z, w.z)
        em.to_mesh_clear()
    return lo, hi


class Framing:
    """Square ortho framing shared by template render and projection UVs."""
    def __init__(self, lo, hi, margin=0.05, front_sign=-1):
        self.lo, self.hi = lo, hi
        self.cx = (lo.x + hi.x) * 0.5
        self.cy = (lo.y + hi.y) * 0.5
        self.cz = (lo.z + hi.z) * 0.5
        ext = max(hi.x - lo.x, hi.z - lo.z)
        self.S = ext * (1.0 + 2.0 * margin)
        self.front_sign = front_sign   # -1: front camera at -Y (faces -Y)

    def uv(self, wx, wz, side):
        """World point -> projected UV for 'front' or 'back' image."""
        s = self.front_sign if side == 'front' else -self.front_sign
        u = 0.5 + s * (wx - self.cx) / self.S
        v = 0.5 + (wz - self.cz) / self.S
        return u, v

    def camera_for_side(self, side, dist=None):
        """Create an ortho camera whose render matches self.uv(side)."""
        s = self.front_sign if side == 'front' else -self.front_sign
        if dist is None:
            dist = (self.hi - self.lo).length + 1.0
        cam_data = bpy.data.cameras.new("tpl_cam_" + side)
        cam_data.type = 'ORTHO'
        cam_data.ortho_scale = self.S
        cam_data.clip_start = 0.01
        cam_data.clip_end = dist * 4.0
        cam = bpy.data.objects.new("tpl_cam_" + side, cam_data)
        bpy.context.scene.collection.objects.link(cam)
        cam.location = (self.cx, self.cy + s * dist, self.cz)
        # camera looks along local -Z toward the body, screen-up = +Z world
        cam.rotation_euler = (math.pi * 0.5, 0.0, 0.0 if s < 0 else math.pi)
        return cam


def parse_argv(argv):
    """Options after the '--' separator: --key value pairs and flags."""
    if '--' in argv:
        argv = argv[argv.index('--') + 1:]
    opts = {}
    i = 0
    while i < len(argv):
        k = argv[i]
        if k.startswith('--'):
            if i + 1 < len(argv) and not argv[i + 1].startswith('--'):
                opts[k[2:]] = argv[i + 1]; i += 2
            else:
                opts[k[2:]] = True; i += 1
        else:
            opts.setdefault('_pos', []).append(k); i += 1
    return opts


def front_sign_from_opt(opts):
    v = str(opts.get('front', '-y')).lower()
    if v in ('-y', 'neg', '-1'): return -1
    if v in ('+y', 'y', 'pos', '1'): return 1
    raise SystemExit("--front must be -y or +y, got %r" % v)

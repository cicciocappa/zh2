# Shared logic for the PROP cubic projection-bake pipeline (the prop analog of
# outfit_common.py — see CORPSE_DESIGN §10.8 sessions / OUTFIT_DESIGN for the
# method). Runs INSIDE Blender (bpy). Both prop_template.py (renders the 6
# ortho side templates) and prop_bake.py (projects the painted sides back onto
# the prop's UVs) import this module, so the framing is identical by
# construction: paint on the template of side S, the pixels land on the prop
# exactly where you put them.
#
# The 6 sides are named by the WORLD axis the camera sits on (Blender Z-up):
#   px = seen from +X   nx = from -X   py = from +Y   ny = from -Y
#   pz = from above     nz = from below
# Each side has its OWN square framing fitted to the prop's silhouette on that
# axis (margin included): px/m varies between sides of an elongated prop (a bus
# front view is not wasted at the length of the side view) but is uniform
# within each image. The mapping is deterministic from the world bbox -> no
# meta file needed, template and bake recompute it identically.
#
# Projection math for side (d = look dir, r = screen right, u = screen up):
#   u = 0.5 + dot(p - c, r)/S     v = 0.5 + dot(p - c, u)/S
# with c = bbox center, S = max in-plane extent * (1 + 2*margin). An ortho
# camera built by camera_for_side() renders EXACTLY this mapping.

import os
import sys
import math

import bpy
from mathutils import Vector, Matrix

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from outfit_common import world_bbox, parse_argv  # noqa: E402 (riuso)

# side -> (look dir d, screen right r, screen up u); r = cross(u, -d) so that
# the rendered image and the UV projection agree (right-handed camera basis).
SIDES = {
    'px': (Vector((-1, 0, 0)), Vector((0,  1, 0)), Vector((0, 0, 1))),
    'nx': (Vector(( 1, 0, 0)), Vector((0, -1, 0)), Vector((0, 0, 1))),
    'py': (Vector((0, -1, 0)), Vector((-1, 0, 0)), Vector((0, 0, 1))),
    'ny': (Vector((0,  1, 0)), Vector(( 1, 0, 0)), Vector((0, 0, 1))),
    'pz': (Vector((0, 0, -1)), Vector(( 1, 0, 0)), Vector((0, 1, 0))),
    'nz': (Vector((0, 0,  1)), Vector((-1, 0, 0)), Vector((0, 1, 0))),
}
SIDE_ORDER = ['px', 'nx', 'py', 'ny', 'pz', 'nz']
OPPOSITE = {'px': 'nx', 'nx': 'px', 'py': 'ny', 'ny': 'py',
            'pz': 'nz', 'nz': 'pz'}


def clean_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def import_glb(path, scale=1.0):
    """Import a glb/gltf, join all meshes into ONE object, apply transforms
    (and the optional catalog scale) so vertex coords are world-final."""
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=os.path.abspath(path))
    new = [o for o in bpy.data.objects if o not in before]
    meshes = [o for o in new if o.type == 'MESH']
    other_names = [o.name for o in new if o.type != 'MESH']
    if not meshes:
        raise RuntimeError("no mesh object in %s" % path)
    # un-parent keeping world transform (gltf import often drives the Y-up ->
    # Z-up conversion through parent empties), then join into ONE object
    bpy.ops.object.select_all(action='DESELECT')
    for o in meshes:
        o.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.object.parent_clear(type='CLEAR_KEEP_TRANSFORM')
    if len(meshes) > 1:
        bpy.ops.object.join()          # removes every mesh but the active one
    obj = bpy.context.view_layer.objects.active
    if scale != 1.0:
        obj.scale = (obj.scale[0]*scale, obj.scale[1]*scale, obj.scale[2]*scale)
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    for name in other_names:           # drop leftover empties/lights
        o = bpy.data.objects.get(name)
        if o:
            bpy.data.objects.remove(o, do_unlink=True)
    return obj


class Framing6:
    """Per-side square ortho framing shared by template render and bake."""
    def __init__(self, lo, hi, margin=0.05):
        self.lo, self.hi = lo, hi
        self.c = (lo + hi) * 0.5
        self.margin = margin
        self.S = {}
        for side, (d, r, u) in SIDES.items():
            ext = Vector((hi.x - lo.x, hi.y - lo.y, hi.z - lo.z))
            er = abs(ext.x * r.x) + abs(ext.y * r.y) + abs(ext.z * r.z)
            eu = abs(ext.x * u.x) + abs(ext.y * u.y) + abs(ext.z * u.z)
            self.S[side] = max(er, eu, 1e-6) * (1.0 + 2.0 * margin)

    def uv(self, p, side):
        d, r, u = SIDES[side]
        q = p - self.c
        S = self.S[side]
        return 0.5 + q.dot(r) / S, 0.5 + q.dot(u) / S

    def camera_for_side(self, side, dist=None):
        """Ortho camera whose render matches self.uv(side) exactly."""
        d, r, u = SIDES[side]
        if dist is None:
            dist = (self.hi - self.lo).length + 1.0
        cam_data = bpy.data.cameras.new("tpl_cam_" + side)
        cam_data.type = 'ORTHO'
        cam_data.ortho_scale = self.S[side]
        cam_data.clip_start = 0.01
        cam_data.clip_end = dist * 4.0
        cam = bpy.data.objects.new("tpl_cam_" + side, cam_data)
        bpy.context.scene.collection.objects.link(cam)
        z = -d  # camera local +Z points away from what it looks at
        m = Matrix(((r.x, u.x, z.x, 0.0),
                    (r.y, u.y, z.y, 0.0),
                    (r.z, u.z, z.z, 0.0),
                    (0.0, 0.0, 0.0, 1.0)))
        cam.matrix_world = m
        cam.location = self.c - d * dist
        return cam

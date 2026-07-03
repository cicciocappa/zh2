# Level export: authored Blender level -> .scn (+ terrain/statics glb + .zhm).
# Convention: BLENDER_LEVEL.md. Runs INSIDE Blender on the authored .blend:
#
#   blender --background levels/level1.blend --python gfx/export_scn.py -- \
#           --out assets/scenes/level1.scn [--mesh-dir assets/terrain] [--ppm 4]
#           [--no-bake] [--catalog assets/props/catalog.txt]
#
# Rules (see BLENDER_LEVEL.md for the full convention):
#   - object-name prefix = entity type (goal/spawn/pack/cost/wall/turret/poly/
#     prop_<key>), custom properties = parameters; `.NNN` blender suffixes and
#     anything after the first `.` are ignored, `type_label` free labels too;
#   - objects/collections starting with `_` are ignored (refs, work lights);
#   - collection `terrain` -> glb#1 + baked .zhm; collection `statics` -> glb#2
#     AND one `poly solid` per mesh footprint (opt-out: custom prop nav="none");
#   - collection-instance empties -> `prop <collection-name> x y rot`;
#   - scene custom props: cell, world_w/world_h (default: terrain AABB),
#     set_<name> -> `set <name> v`; `mission`/`budget`/`exit`/`lz` reserved.
#
# Validation is BLOCKING (unknown prefix, off-world entity, bad catalog key,
# format limits): no .scn / glb / zhm is written if any error is found.
# Output is deterministic: objects are processed in name order.

import bpy
import math
import os
import sys
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import terrain_bake   # bake_zhm + world_aabb (BVH engine, terrain objs only)

# mirror of scene.h limits — keep in sync
SCENE_MAX_SET        = 32
SCENE_MAX_POLY       = 256
SCENE_POLY_MAX_VERTS = 16
SCENE_MAX_RECT       = 64
SCENE_MAX_PROP       = 256
SCENE_PROP_KEY_LEN   = 24

RECT_TYPES  = ("goal", "spawn", "pack", "cost")
RESERVED    = ("exit", "lz")           # GAME_PLAN fase A — do not repurpose
SKIP_OBTYPE = ("CAMERA", "LIGHT", "LIGHT_PROBE", "SPEAKER", "ARMATURE")

errors, warnings, notes = [], [], []


def err(msg):  errors.append(msg)
def warn(msg): warnings.append(msg)


def parse_args():
    import argparse
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    p = argparse.ArgumentParser(prog="export_scn.py")
    p.add_argument("--out", required=True, help="output .scn path")
    p.add_argument("--mesh-dir", default="assets/terrain",
                   help="output dir for terrain/statics glb + zhm (default assets/terrain)")
    p.add_argument("--ppm", type=float, default=4.0,
                   help="zhm samples per meter (default 4)")
    p.add_argument("--no-bake", action="store_true",
                   help="skip the .zhm bake (layout-only iteration)")
    p.add_argument("--catalog", default="assets/props/catalog.txt",
                   help="prop catalog for key validation")
    return p.parse_args(argv)


def fmt(v):
    s = f"{v:.3f}".rstrip("0").rstrip(".")
    return "0" if s == "-0" else s


def world_corners(ob):
    return [ob.matrix_world @ Vector(c) for c in ob.bound_box]


def aabb_xy(ob):
    cs = world_corners(ob)
    xs = [c.x for c in cs]; ys = [c.y for c in cs]
    return min(xs), min(ys), max(xs) - min(xs), max(ys) - min(ys)


def yaw_deg(ob):
    return math.degrees(ob.matrix_world.to_euler().z)


def check_rect_yaw(ob):
    # rects are axis-aligned by format: the AABB is exported; any yaw that is
    # not a multiple of 90 deg inflates it
    if abs(yaw_deg(ob)) % 90.0 > 0.5 and abs((abs(yaw_deg(ob)) % 90.0) - 90.0) > 0.5:
        warn(f"'{ob.name}': yaw {yaw_deg(ob):.1f} deg on a rect entity — "
             f"exporting the world AABB (rects are axis-aligned)")


def mesh_world_verts(ob, deps):
    eo = ob.evaluated_get(deps)
    me = eo.to_mesh()
    vs = [eo.matrix_world @ v.co for v in me.vertices]
    eo.to_mesh_clear()
    return vs


def convex_hull_xy(pts):
    """Monotone chain on rounded XY points; collinear points are dropped."""
    P = sorted(set((round(p.x, 4), round(p.y, 4)) for p in pts))
    if len(P) <= 2:
        return P
    def cross(o, a, b):
        return (a[0]-o[0])*(b[1]-o[1]) - (a[1]-o[1])*(b[0]-o[0])
    def half(seq):
        h = []
        for p in seq:
            while len(h) >= 2 and cross(h[-2], h[-1], p) <= 1e-9:
                h.pop()
            h.append(p)
        return h
    lower = half(P)
    upper = half(reversed(P))
    return lower[:-1] + upper[:-1]


def convex_overlap(A, B):
    """SAT for two convex polygons given as [(x,y), ...]."""
    for poly in (A, B):
        n = len(poly)
        for i in range(n):
            ax, ay = poly[i]; bx, by = poly[(i + 1) % n]
            nx, ny = -(by - ay), (bx - ax)
            pa = [nx*px + ny*py for px, py in A]
            pb = [nx*px + ny*py for px, py in B]
            if max(pa) < min(pb) or max(pb) < min(pa):
                return False
    return True


def rect_corners(x, y, w, h):
    return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]


def load_catalog(path):
    keys = set()
    try:
        with open(path) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    keys.add(line.split()[0])
    except OSError:
        err(f"prop catalog not found: {path}")
    return keys


def base_token(name):
    return name.split(".", 1)[0].strip().lower()


def collect_collections(scene):
    """Walk the collection tree: subtree sets for `_`-ignored, terrain, statics."""
    ignored, terrain, statics = set(), set(), set()
    found = {"terrain": 0, "statics": 0}

    def walk(col, ign):
        ign = ign or col.name.startswith("_")
        tok = base_token(col.name)
        into = None
        if not ign and tok in ("terrain", "statics"):
            found[tok] += 1
            into = terrain if tok == "terrain" else statics
        for ob in col.objects:
            if ign:
                ignored.add(ob)
            elif into is not None:
                into.add(ob)
        for child in col.children:
            if into is not None:
                # nested collections inside terrain/statics belong to them
                for ob in child.all_objects:
                    (ignored if child.name.startswith("_") else into).add(ob)
            else:
                walk(child, ign)

    walk(scene.collection, False)
    for k, n in found.items():
        if n > 1:
            err(f"more than one '{k}' collection in the scene")
    return ignored, terrain, statics


def poly_entry(ob, deps, name_for_msgs):
    """-> (hull, height, cost_or_None) for an explicit poly or statics footprint."""
    vs = mesh_world_verts(ob, deps)
    if not vs:
        err(f"'{name_for_msgs}': empty mesh, no footprint")
        return None
    hull = convex_hull_xy(vs)
    if len(hull) < 3:
        err(f"'{name_for_msgs}': degenerate footprint (collinear vertices)")
        return None
    if len(hull) > SCENE_POLY_MAX_VERTS:
        err(f"'{name_for_msgs}': convex hull has {len(hull)} verts "
            f"(max {SCENE_POLY_MAX_VERTS}) — simplify the footprint")
        return None
    zs = [v.z for v in vs]
    height = float(ob.get("height", max(zs) - min(zs)))
    cost = ob.get("cost", None)
    return hull, height, (float(cost) if cost is not None else None)


def export_glb(objs, path):
    bpy.ops.object.select_all(action="DESELECT")
    meshes = [ob for ob in objs if ob.type == "MESH"]
    for ob in meshes:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.export_scene.gltf(filepath=path, use_selection=True,
                              export_apply=True)
    print(f"[export_scn] wrote {path}  ({len(meshes)} meshes)")


def main():
    args = parse_args()
    sc = bpy.context.scene
    deps = bpy.context.evaluated_depsgraph_get()
    stem = os.path.splitext(os.path.basename(args.out))[0]
    catalog = load_catalog(args.catalog)

    ignored, terrain_objs, statics_objs = collect_collections(sc)
    terrain_meshes = sorted((o for o in terrain_objs if o.type == "MESH"),
                            key=lambda o: o.name)
    statics_meshes = sorted((o for o in statics_objs if o.type == "MESH"),
                            key=lambda o: o.name)

    # ---- scene-level parameters -------------------------------------------
    cell = float(sc.get("cell", 0.5))
    for k in ("mission", "budget"):
        if k in sc.keys():
            err(f"scene property '{k}' is reserved (GAME_PLAN fase A), "
                f"not supported yet")
    sets = sorted((k[4:], float(sc[k])) for k in sc.keys()
                  if k.startswith("set_"))
    if len(sets) > SCENE_MAX_SET:
        err(f"{len(sets)} set_* properties (max {SCENE_MAX_SET})")

    ter_lo = ter_hi = None
    if terrain_meshes:
        ter_lo, ter_hi = terrain_bake.world_aabb(terrain_meshes)
        if abs(ter_lo.x) > cell / 2 or abs(ter_lo.y) > cell / 2:
            err(f"terrain AABB min is ({ter_lo.x:.2f},{ter_lo.y:.2f}), must "
                f"be at the world origin (0,0) within half a cell — move the "
                f"ground mesh")
    if "world_w" in sc.keys() or "world_h" in sc.keys():
        if not ("world_w" in sc.keys() and "world_h" in sc.keys()):
            err("world_w/world_h: set both or neither")
            world_w = world_h = 1.0
        else:
            world_w = float(sc["world_w"]); world_h = float(sc["world_h"])
            if ter_hi is not None and (ter_hi.x > world_w + cell / 2 or
                                       ter_hi.y > world_h + cell / 2):
                warn(f"terrain extends to ({ter_hi.x:.1f},{ter_hi.y:.1f}) "
                     f"beyond the declared world {world_w}x{world_h}")
    elif ter_hi is not None:
        world_w = round(ter_hi.x / cell) * cell
        world_h = round(ter_hi.y / cell) * cell
    else:
        err("no terrain collection and no world_w/world_h scene properties: "
            "world size unknown")
        world_w = world_h = 1.0

    # ---- entity walk (name order = deterministic output) ------------------
    rects   = {t: [] for t in RECT_TYPES}   # (name, x, y, w, h[, weight])
    walls   = []                            # (name, hp, mult, x, y, w, h)
    turrets = []                            # (name, x, y, range, heavy, hp)
    polys   = []                            # (name, hull, height, cost|None)
    props   = []                            # (name, key, x, y, rot)

    for ob in sorted(sc.objects, key=lambda o: o.name):
        if ob in ignored or ob.name.startswith("_"):
            continue
        if ob in terrain_objs or ob in statics_objs:
            continue

        # collection instance = prop, whatever the object is named
        if ob.type == "EMPTY" and ob.instance_collection is not None:
            key = base_token(ob.instance_collection.name)
            p = ob.matrix_world.translation
            props.append((ob.name, key, p.x, p.y, yaw_deg(ob) % 360.0))
            continue

        tok = base_token(ob.name)
        if tok.startswith("prop_"):
            key = tok[5:]
            p = ob.matrix_world.translation
            props.append((ob.name, key, p.x, p.y, yaw_deg(ob) % 360.0))
            continue

        head = tok.split("_", 1)[0]
        if head in RECT_TYPES:
            x, y, w, h = aabb_xy(ob)
            check_rect_yaw(ob)
            if head == "cost":
                if "weight" not in ob.keys():
                    err(f"'{ob.name}': cost rect without 'weight' property")
                    continue
                rects["cost"].append((ob.name, x, y, w, h, float(ob["weight"])))
            else:
                rects[head].append((ob.name, x, y, w, h))
        elif head == "wall":
            x, y, w, h = aabb_xy(ob)
            check_rect_yaw(ob)
            if min(w, h) < cell:
                warn(f"'{ob.name}': wall thinner than the nav cell "
                     f"({min(w, h):.2f} < {cell}) — may rasterize with gaps")
            walls.append((ob.name, float(ob.get("hp", 500.0)),
                          float(ob.get("cost_mult", 1.0)), x, y, w, h))
        elif head == "turret":
            p = ob.matrix_world.translation
            turrets.append((ob.name, p.x, p.y, float(ob.get("range", 30.0)),
                            int(ob.get("heavy", 0)), float(ob.get("hp", 0.0))))
        elif head == "poly":
            if ob.type != "MESH":
                err(f"'{ob.name}': poly must be a mesh")
                continue
            pe = poly_entry(ob, deps, ob.name)
            if pe:
                polys.append((ob.name, *pe))
        elif head in RESERVED:
            err(f"'{ob.name}': '{head}' is reserved (GAME_PLAN fase A), "
                f"not supported yet")
        elif ob.type in SKIP_OBTYPE:
            notes.append(f"skipped {ob.type.lower()} '{ob.name}'")
        else:
            err(f"'{ob.name}': unknown entity prefix '{head}' — rename it or "
                f"prefix it with '_' to ignore it")

    # statics footprints: visual and nav from the SAME object (opt-out nav="none")
    for ob in statics_meshes:
        if str(ob.get("nav", "")).lower() == "none":
            notes.append(f"statics '{ob.name}': nav=none, no footprint")
            continue
        pe = poly_entry(ob, deps, f"statics {ob.name}")
        if pe:
            hull, height, cost = pe
            if cost is not None:
                warn(f"statics '{ob.name}': 'cost' property ignored "
                     f"(statics footprints are solid)")
            polys.append((ob.name, hull, height, None))

    # ---- validation --------------------------------------------------------
    for t in RECT_TYPES:
        if len(rects[t]) > SCENE_MAX_RECT:
            err(f"{len(rects[t])} '{t}' rects (max {SCENE_MAX_RECT})")
    if len(walls) > SCENE_MAX_RECT:
        err(f"{len(walls)} walls (max {SCENE_MAX_RECT})")
    if len(turrets) > SCENE_MAX_RECT:
        err(f"{len(turrets)} turrets (max {SCENE_MAX_RECT})")
    if len(polys) > SCENE_MAX_POLY:
        err(f"{len(polys)} polys incl. statics footprints (max {SCENE_MAX_POLY})")
    if len(props) > SCENE_MAX_PROP:
        err(f"{len(props)} props (max {SCENE_MAX_PROP})")

    tol = cell / 2
    def check_bounds(name, x0, y0, x1, y1):
        if x0 < -tol or y0 < -tol or x1 > world_w + tol or y1 > world_h + tol:
            err(f"'{name}': outside the world [0,{fmt(world_w)}]x"
                f"[0,{fmt(world_h)}]")
    for t in RECT_TYPES:
        for r in rects[t]:
            check_bounds(r[0], r[1], r[2], r[1] + r[3], r[2] + r[4])
    for wl in walls:
        check_bounds(wl[0], wl[3], wl[4], wl[3] + wl[5], wl[4] + wl[6])
    for tu in turrets:
        check_bounds(tu[0], tu[1], tu[2], tu[1], tu[2])
    for pl in polys:
        xs = [p[0] for p in pl[1]]; ys = [p[1] for p in pl[1]]
        check_bounds(pl[0], min(xs), min(ys), max(xs), max(ys))
    for pr in props:
        check_bounds(pr[0], pr[2], pr[3], pr[2], pr[3])

    for pr in props:
        if pr[1] not in catalog:
            err(f"'{pr[0]}': prop key '{pr[1]}' not in {args.catalog}")
        if len(pr[1]) >= SCENE_PROP_KEY_LEN:
            err(f"'{pr[0]}': prop key '{pr[1]}' too long "
                f"(max {SCENE_PROP_KEY_LEN - 1})")

    solid_hulls = [(pl[0], pl[1]) for pl in polys if pl[3] is None]
    for t in ("spawn", "goal"):
        for r in rects[t]:
            rc = rect_corners(r[1], r[2], r[3], r[4])
            for hn, hull in solid_hulls:
                if convex_overlap(rc, hull):
                    warn(f"'{r[0]}' ({t}) overlaps solid footprint '{hn}'")

    if errors:
        for e in errors:
            print(f"[export_scn] ERROR: {e}", file=sys.stderr)
        sys.exit(1)

    # ---- outputs (only reached with zero errors) ---------------------------
    os.makedirs(args.mesh_dir, exist_ok=True)
    if os.path.dirname(args.out):
        os.makedirs(os.path.dirname(args.out), exist_ok=True)

    ter_path = st_path = None
    if terrain_meshes:
        ter_path = os.path.join(args.mesh_dir, f"{stem}.glb")
        export_glb(terrain_meshes, ter_path)
        if not args.no_bake:
            zhm = os.path.join(args.mesh_dir, f"{stem}.zhm")
            terrain_bake.bake_zhm(terrain_meshes, deps, zhm, args.ppm)
    if statics_meshes:
        st_path = os.path.join(args.mesh_dir, f"{stem}_st.glb")
        export_glb(statics_meshes, st_path)

    blend = os.path.basename(bpy.data.filepath) or "<unsaved>"
    L = [f"# {stem}.scn — generated by gfx/export_scn.py from {blend}",
         f"# (do not hand-edit: the .blend is the source of truth, "
         f"see BLENDER_LEVEL.md)",
         f"cell {fmt(cell)}",
         f"world {fmt(world_w)} {fmt(world_h)}"]
    if ter_path:
        L.append(f"terrain {ter_path}")
    if st_path:
        L.append(f"statics {st_path}")
    for name, val in sets:
        L.append(f"set {name} {fmt(val)}")
    for t in RECT_TYPES:
        for r in rects[t]:
            line = f"{t} {fmt(r[1])} {fmt(r[2])} {fmt(r[3])} {fmt(r[4])}"
            if t == "cost":
                line += f" {fmt(r[5])}"
            L.append(line)
    for name, hull, height, cost in polys:
        kind = "solid" if cost is None else f"cost {fmt(cost)}"
        vs = " ".join(f"{fmt(x)} {fmt(y)}" for x, y in hull)
        L.append(f"poly {fmt(height)} {kind} {vs}")
    for name, hp, mult, x, y, w, h in walls:
        L.append(f"wall {fmt(hp)} {fmt(mult)} {fmt(x)} {fmt(y)} "
                 f"{fmt(w)} {fmt(h)}")
    for name, x, y, rng, heavy, hp in turrets:
        L.append(f"turret {fmt(x)} {fmt(y)} {fmt(rng)} {heavy:d} {fmt(hp)}")
    for name, key, x, y, rot in props:
        L.append(f"prop {key} {fmt(x)} {fmt(y)} {fmt(rot)}")

    with open(args.out, "w") as f:
        f.write("\n".join(L) + "\n")

    for n in notes:
        print(f"[export_scn] note: {n}")
    for w in warnings:
        print(f"[export_scn] WARNING: {w}")
    counts = " ".join(f"{t}:{len(rects[t])}" for t in RECT_TYPES)
    print(f"[export_scn] wrote {args.out}  world {fmt(world_w)}x{fmt(world_h)} "
          f"cell {fmt(cell)}  {counts} poly:{len(polys)} wall:{len(walls)} "
          f"turret:{len(turrets)} prop:{len(props)} set:{len(sets)}  "
          f"({len(warnings)} warnings)")


if __name__ == "__main__":
    main()

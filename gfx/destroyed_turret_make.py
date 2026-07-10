# Placeholder "torretta distrutta" (richiesta utente 2026-07-09): un cubo
# scuro e schiacciato al posto del modello vivo dopo il crollo. Genera sia il
# sorgente blend/destroyed_turret.blend (da autorare piu' avanti) sia il glb
# runtime assets/models/destroyed_turret.glb (triangle soup flat-color letta
# da load_glb_soup in vat_horde). Base appoggiata a z=0, origine al centro
# del footprint — stessa convenzione dei prop di catalogo.
#
#   blender --background --python gfx/destroyed_turret_make.py
#
import bpy
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

bpy.ops.wm.read_factory_settings(use_empty=True)

# squat dark cube: 0.8 x 0.8 m footprint, 0.45 m tall, base on z=0
bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0.0, 0.0, 0.225))
ob = bpy.context.active_object
ob.name = "wreck"
ob.scale = (0.4, 0.4, 0.225)
bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

mat = bpy.data.materials.new("wreck_charred")
mat.use_nodes = True
bsdf = mat.node_tree.nodes.get("Principled BSDF")
if bsdf:
    bsdf.inputs["Base Color"].default_value = (0.10, 0.095, 0.09, 1.0)
    bsdf.inputs["Roughness"].default_value = 0.95
ob.data.materials.append(mat)

blend_path = os.path.join(ROOT, "blend", "destroyed_turret.blend")
glb_path = os.path.join(ROOT, "assets", "models", "destroyed_turret.glb")
bpy.ops.wm.save_as_mainfile(filepath=blend_path)
bpy.ops.export_scene.gltf(filepath=glb_path, export_format="GLB")
print(f"[destroyed_turret_make] {blend_path} + {glb_path}")

# Blender setup for texture painting an exported RoN unit. Run headless:
#   blender -b -P re/tools/mods/blender-setup.py -- <model.obj> <out.blend> [preview.png]
# Imports the OBJ, wires its texture into a material, switches to Texture Paint with the image as the paint target,
# frames the model and saves a .blend (plus an optional preview render to check the import).
import bpy, sys, os, math
argv = sys.argv[sys.argv.index('--') + 1:]
obj_path, blend_path = os.path.abspath(argv[0]), os.path.abspath(argv[1])
preview = os.path.abspath(argv[2]) if len(argv) > 2 else None

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.obj_import(filepath=obj_path, forward_axis='Y', up_axis='Z')
objs = [o for o in bpy.context.scene.objects if o.type == 'MESH']
bpy.ops.object.select_all(action='DESELECT')
for o in objs: o.select_set(True)
bpy.context.view_layer.objects.active = objs[0]
if len(objs) > 1: bpy.ops.object.join()
ob = bpy.context.view_layer.objects.active
mat = ob.active_material
tex_node = next((n for n in mat.node_tree.nodes if n.type == 'TEX_IMAGE'), None) if mat and mat.use_nodes else None
img = tex_node.image if tex_node else None
if img:
    tex_node.interpolation = 'Closest'   # the game samples these tiny atlases without much filtering; keep pixels crisp
    img.alpha_mode = 'CHANNEL_PACKED'    # alpha is a team-colour code, not transparency
    bsdf = next(n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
    bsdf.inputs['Roughness'].default_value = 1.0
    for l in list(mat.node_tree.links):
        if l.to_socket.name == 'Alpha': mat.node_tree.links.remove(l)
mat.blend_method = 'OPAQUE'

# Scene: light, camera framing the model, grey world.
ob.rotation_euler = (0, 0, 0)
bpy.ops.object.shade_flat()
dims = ob.dimensions; center = ob.location + ob.matrix_world.to_3x3() @ (sum((v.co for v in ob.data.vertices), ob.data.vertices[0].co * 0) / len(ob.data.vertices))
r = max(dims) * 0.9
cam_data = bpy.data.cameras.new('Camera'); cam = bpy.data.objects.new('Camera', cam_data); bpy.context.scene.collection.objects.link(cam)
cam.location = center + __import__('mathutils').Vector((r * 1.6, -r * 2.2, r * 1.1))
d = center - cam.location; cam.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
bpy.context.scene.camera = cam
sun = bpy.data.objects.new('Sun', bpy.data.lights.new('Sun', 'SUN')); bpy.context.scene.collection.objects.link(sun)
sun.rotation_euler = (math.radians(50), 0, math.radians(30)); sun.data.energy = 3
w = bpy.context.scene.world or bpy.data.worlds.new('World'); bpy.context.scene.world = w; w.use_nodes = True
w.node_tree.nodes['Background'].inputs[0].default_value = (0.25, 0.25, 0.28, 1)

# Texture Paint mode with the atlas as the active paint slot, in every 3D viewport.
bpy.ops.object.mode_set(mode='TEXTURE_PAINT')
bpy.context.scene.tool_settings.image_paint.mode = 'MATERIAL'
for scr in bpy.data.screens:
    for area in scr.areas:
        if area.type == 'VIEW_3D':
            for sp in area.spaces:
                if sp.type == 'VIEW_3D': sp.shading.type = 'MATERIAL'
        if area.type == 'IMAGE_EDITOR' and img:
            for sp in area.spaces:
                if sp.type == 'IMAGE_EDITOR': sp.image = img
bpy.ops.wm.save_as_mainfile(filepath=blend_path)
if preview:
    bpy.context.scene.render.engine = 'BLENDER_EEVEE_NEXT' if hasattr(bpy.types, 'SceneEEVEE') and 'BLENDER_EEVEE_NEXT' in [i.identifier for i in bpy.types.RenderSettings.bl_rna.properties['engine'].enum_items] else 'BLENDER_EEVEE'
    bpy.context.scene.render.resolution_x = 600; bpy.context.scene.render.resolution_y = 600
    bpy.context.scene.render.filepath = preview
    bpy.ops.render.render(write_still=True)
print('saved', blend_path, 'image', img.name if img else None, 'size', tuple(img.size) if img else None)

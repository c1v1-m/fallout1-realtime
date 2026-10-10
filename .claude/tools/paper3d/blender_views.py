# Four big, clean views of the carved vault dweller -- front, back, left, right -- as input for an
# image-to-3D generator (Hunyuan3D's multi-view mode wants exactly these). White background,
# orthographic, the character filling the frame.
#
#   blender -b VaultDweller.blend --python blender_views.py -- <out_dir> [size]

import bpy, sys, os, math

args = sys.argv[sys.argv.index('--') + 1:]
out_dir = args[0]
size = int(args[1]) if len(args) > 1 else 1024
os.makedirs(out_dir, exist_ok=True)

scene = bpy.context.scene
for o in list(bpy.data.objects):  # only the standing dude
    if o.name != 'Dude':
        o.hide_render = True
dude = bpy.data.objects['Dude']
h = dude.dimensions.z
centre = (0, 0, h / 2)

cam_data = bpy.data.cameras.new('views')
cam_data.type = 'ORTHO'
cam_data.ortho_scale = h * 1.12
cam = bpy.data.objects.new('views', cam_data)
scene.collection.objects.link(cam)
scene.camera = cam

scene.render.resolution_x = scene.render.resolution_y = size
scene.render.film_transparent = False
world = scene.world or bpy.data.worlds.new('World')
scene.world = world
world.use_nodes = True
bg = world.node_tree.nodes.get('Background')
bg.inputs['Color'].default_value = (1, 1, 1, 1)
bg.inputs['Strength'].default_value = 1.0
scene.view_settings.view_transform = 'Standard'

# He faces -Y (Blender's front). Camera distance is irrelevant for an orthographic camera.
views = {
    'front': ((0, -10, centre[2]), (math.radians(90), 0, 0)),
    'back': ((0, 10, centre[2]), (math.radians(90), 0, math.radians(180))),
    'left': ((10, 0, centre[2]), (math.radians(90), 0, math.radians(90))),   # his left side: he faces -Y, so his left is +X
    'right': ((-10, 0, centre[2]), (math.radians(90), 0, math.radians(-90))),
}
for name, (loc, rot) in views.items():
    cam.location = loc
    cam.rotation_euler = rot
    scene.render.filepath = os.path.join(out_dir, f'vault_dweller_{name}.png')
    bpy.ops.render.render(write_still=True)
    print('VIEW', name, scene.render.filepath)

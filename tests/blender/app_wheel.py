"""An app wheel made in Blender for testing the interface's 3D models: eight bevelled lacquer slices, each an
object named app:<id> (so clicking it opens that app), a gold rim, a glass dome in the middle, small glowing
marks, a turning animation ("spin") and a hover animation ("pop"), and a camera. Exported as one .glb.
  blender -b --factory-startup -P tests/blender/app_wheel.py -- OUT.glb"""
import bpy, bmesh, math, sys

out = sys.argv[sys.argv.index("--") + 1] if "--" in sys.argv else "app_wheel.glb"
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene


def material(name, color, metallic=0.0, roughness=0.5, coat=0.0, coat_rough=0.03, transmission=0.0, emit=None,
             emit_strength=0.0, alpha=1.0, ior=1.5):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    p = m.node_tree.nodes["Principled BSDF"]
    p.inputs["Base Color"].default_value = (*color, 1)
    p.inputs["Metallic"].default_value = metallic
    p.inputs["Roughness"].default_value = roughness
    p.inputs["Coat Weight"].default_value = coat
    p.inputs["Coat Roughness"].default_value = coat_rough
    p.inputs["Transmission Weight"].default_value = transmission
    p.inputs["IOR"].default_value = ior
    if emit:
        p.inputs["Emission Color"].default_value = (*emit, 1)
        p.inputs["Emission Strength"].default_value = emit_strength
    if alpha < 1:
        p.inputs["Alpha"].default_value = alpha
    return m


gold = material("Gold", (1.0, 0.78, 0.34), metallic=1.0, roughness=0.22)
lacquers = [material(f"Lacquer {i}", c, roughness=0.35, coat=1.0, coat_rough=0.04) for i, c in enumerate([
    (0.55, 0.03, 0.05), (0.02, 0.05, 0.12), (0.55, 0.03, 0.05), (0.02, 0.05, 0.12),
    (0.55, 0.03, 0.05), (0.02, 0.05, 0.12), (0.55, 0.03, 0.05), (0.02, 0.05, 0.12)])]
glass = material("Glass", (0.85, 0.92, 1.0), roughness=0.04, transmission=1.0, ior=1.45)
glows = [material(f"Glow {i}", c, emit=c, emit_strength=4.0, roughness=0.4) for i, c in enumerate([
    (1.0, 0.75, 0.3), (0.35, 0.8, 1.0), (1.0, 0.75, 0.3), (0.35, 0.8, 1.0),
    (1.0, 0.75, 0.3), (0.35, 0.8, 1.0), (1.0, 0.75, 0.3), (0.35, 0.8, 1.0)])]

wheel = bpy.data.objects.new("Wheel", None)
scene.collection.objects.link(wheel)
wheel.rotation_euler = (math.radians(90), 0, 0)  # standing, facing the front (-Y in Blender, +Z in glTF)

apps = ["files", "notes", "settings", "terminal", "music", "photos", "browser", "store"]
n = len(apps)
r_in, r_out, depth, gap = 0.42, 1.0, 0.14, math.radians(2.2)


def sector(a0, a1, segs=16):
    bm = bmesh.new()
    ring = []
    for i in range(segs + 1):
        a = a0 + (a1 - a0) * i / segs
        ring.append((math.cos(a), math.sin(a)))
    outer = [bm.verts.new((c * r_out, s * r_out, 0)) for c, s in ring]
    inner = [bm.verts.new((c * r_in, s * r_in, 0)) for c, s in reversed(ring)]
    face = bm.faces.new(outer + inner)
    ext = bmesh.ops.extrude_face_region(bm, geom=[face])
    top = [v for v in ext["geom"] if isinstance(v, bmesh.types.BMVert)]
    bmesh.ops.translate(bm, verts=top, vec=(0, 0, depth))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return bm


for i, app in enumerate(apps):
    a0 = 2 * math.pi * i / n + gap / 2 + math.pi / 2
    a1 = 2 * math.pi * (i + 1) / n - gap / 2 + math.pi / 2
    me = bpy.data.meshes.new(f"slice-{app}")
    bm = sector(a0, a1)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(f"app:{app}", me)
    ob.data.materials.append(lacquers[i])
    ob.parent = wheel
    ob["title"] = app.capitalize()
    ob["hover"] = "lift"
    scene.collection.objects.link(ob)
    bev = ob.modifiers.new("Bevel", "BEVEL")
    bev.width, bev.segments, bev.limit_method = 0.018, 4, "ANGLE"
    for p in ob.data.polygons:
        p.use_smooth = True
    # a small glowing mark on the slice's face (an icon's stand-in)
    mid = (a0 + a1) / 2
    bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=0.07, depth=0.03,
                                        location=(math.cos(mid) * 0.72, math.sin(mid) * 0.72, depth + 0.012))
    mark = bpy.context.active_object
    mark.name = f"mark-{app}"
    mark.data.materials.append(glows[i])
    mark.parent = ob

# the gold rim and hub ring
bpy.ops.mesh.primitive_torus_add(major_radius=r_out + 0.035, minor_radius=0.045, major_segments=128, minor_segments=24,
                                 location=(0, 0, depth / 2))
rim = bpy.context.active_object
rim.name = "Rim"
rim.data.materials.append(gold)
rim.parent = wheel
bpy.ops.object.shade_smooth()
bpy.ops.mesh.primitive_torus_add(major_radius=r_in - 0.03, minor_radius=0.03, major_segments=96, minor_segments=20,
                                 location=(0, 0, depth / 2))
hub = bpy.context.active_object
hub.name = "HubRing"
hub.data.materials.append(gold)
hub.parent = wheel
bpy.ops.object.shade_smooth()
# the glass dome in the middle: opens the launcher
bpy.ops.mesh.primitive_uv_sphere_add(segments=64, ring_count=32, radius=r_in - 0.06, location=(0, 0, depth / 2))
dome = bpy.context.active_object
dome.name = "Dome"
dome.scale = (1, 1, 0.55)
dome.data.materials.append(glass)
dome.parent = wheel
dome["action"] = "open:launcher"
dome["title"] = "Search"
bpy.ops.object.shade_smooth()

# a turning animation on the wheel ("spin"): one eighth, so it can step slice by slice
scene.frame_start, scene.frame_end = 1, 24
wheel.rotation_mode = "XYZ"
wheel.keyframe_insert("rotation_euler", frame=1)
wheel.rotation_euler[2] = 2 * math.pi / n
wheel.keyframe_insert("rotation_euler", frame=24)
wheel.animation_data.action.name = "spin"

# a camera in front
cam = bpy.data.cameras.new("Camera")
cam.lens = 85
co = bpy.data.objects.new("Camera", cam)
scene.collection.objects.link(co)
co.location = (0, -7.2, 1.6)
co.rotation_euler = (math.radians(77.5), 0, 0)
scene.camera = co

bpy.ops.export_scene.gltf(filepath=out, export_format="GLB", export_extras=True, export_cameras=True,
                          export_apply=True, export_animations=True)
print("wrote", out)

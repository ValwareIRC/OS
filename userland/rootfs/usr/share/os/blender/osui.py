"""osui: helpers for making interface models in Blender, for <model> in a Facet design.

    import osui
    from osui import *
    reset()
    wheel = empty("Wheel")
    for i, app in enumerate(["files", "notes", "terminal", "settings"]):
        s = ring_slice(0.45, 1.0, i * 90 + 2, (i + 1) * 90 - 2, depth=0.12, bevel_width=0.015, name=f"app:{app}")
        s.data.materials.append(mat("lacquer", "#8a1020"))
        part(s, title=app.title(), hover="lift")
        s.parent = wheel
    rim = torus(1.04, 0.04, mat("gold"), parent=wheel)
    turn(wheel, "Z", 90, frames=24, name="spin")
    export("wheel")            # ~/Models/wheel.glb

Everything here is ordinary Blender (bpy); use bpy directly for anything these don't do.

Models face the viewer along -Y (Blender's front view), up is +Z; a model about 2 units across fits any size
on screen. Parts: an object named "app:<id>" opens that app when clicked; or give any object an action
("open:files", "toggle:#menu", "url:https://...", "ask:...") with part(); its children click with it.
Materials are exported to glTF's physically based ones: what shows in the OS is what these values say.
Procedural textures (noise, wood, marble node trees) don't export: use bake_texture() or images."""
import bpy, bmesh, math, os

MODELS = os.environ.get("OS_MODELS", os.path.expanduser("~/Models"))


def reset():
    """An empty scene, metric, 24 frames a second."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    s = bpy.context.scene
    s.render.fps = 24
    s.frame_start, s.frame_end = 1, 48
    return s


def _rgb(c):
    if isinstance(c, str):
        c = c.lstrip("#")
        v = [int(c[i:i + 2], 16) / 255 for i in (0, 2, 4)]
        return tuple(x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in v)  # sRGB to linear
    return tuple(c[:3])


# name: (base colour, metallic, roughness, extras)
PRESETS = {
    "gold": ("#ffc35a", 1.0, 0.22, {}),
    "rose-gold": ("#f2b29b", 1.0, 0.25, {}),
    "silver": ("#f2f3f5", 1.0, 0.18, {}),
    "chrome": ("#f4f5f7", 1.0, 0.04, {}),
    "brushed-steel": ("#c8cbd0", 1.0, 0.38, {}),
    "gunmetal": ("#4a4e55", 1.0, 0.32, {}),
    "copper": ("#f0a080", 1.0, 0.28, {}),
    "brass": ("#e6c06e", 1.0, 0.3, {}),
    "bronze": ("#c08a50", 1.0, 0.35, {}),
    "glass": ("#ffffff", 0.0, 0.02, {"transmission": 1.0, "ior": 1.45}),
    "frosted-glass": ("#f4f8ff", 0.0, 0.35, {"transmission": 1.0, "ior": 1.45}),
    "tinted-glass": ("#9fd8ff", 0.0, 0.03, {"transmission": 1.0, "ior": 1.5}),
    "crystal": ("#ffffff", 0.0, 0.0, {"transmission": 1.0, "ior": 2.0}),
    "lacquer": ("#8a1020", 0.0, 0.35, {"coat": 1.0, "coat_roughness": 0.03}),
    "car-paint": ("#1a3a8a", 0.4, 0.4, {"coat": 1.0, "coat_roughness": 0.02}),
    "ceramic": ("#f5f2ea", 0.0, 0.15, {"coat": 0.6, "coat_roughness": 0.05}),
    "porcelain": ("#fbfaf7", 0.0, 0.1, {"coat": 0.8, "coat_roughness": 0.03}),
    "plastic": ("#e8e8e8", 0.0, 0.4, {}),
    "glossy-plastic": ("#e8e8e8", 0.0, 0.12, {}),
    "rubber": ("#202022", 0.0, 0.85, {}),
    "felt": ("#0f5a2c", 0.0, 1.0, {}),
    "velvet": ("#5a0f2a", 0.0, 0.9, {}),
    "leather": ("#5a3420", 0.0, 0.55, {"coat": 0.2, "coat_roughness": 0.4}),
    "wood": ("#8a5a32", 0.0, 0.55, {}),
    "polished-wood": ("#7a4a26", 0.0, 0.3, {"coat": 0.9, "coat_roughness": 0.05}),
    "marble": ("#eeeae4", 0.0, 0.12, {"coat": 0.5, "coat_roughness": 0.03}),
    "stone": ("#8a8780", 0.0, 0.8, {}),
    "matte": ("#d0d0d0", 0.0, 0.7, {}),
    "obsidian": ("#0b0b0f", 0.0, 0.08, {"coat": 1.0, "coat_roughness": 0.02}),
    "neon": ("#ff2bd6", 0.0, 0.5, {"emission": 6.0}),
    "glow": ("#ffe8a0", 0.0, 0.5, {"emission": 3.0}),
    "screen": ("#101418", 0.0, 0.2, {"emission": 1.0}),
}


def mat(preset="plastic", color=None, roughness=None, metallic=None, emission=None, name=None, **kw):
    """A material from a preset (see PRESETS), optionally in another colour ("#rrggbb" or linear rgb)."""
    base, metal, rough, extra = PRESETS.get(preset, PRESETS["plastic"])
    extra = {**extra, **kw}
    m = bpy.data.materials.new(name or f"{preset}{'-' + str(color) if color else ''}")
    m.use_nodes = True
    p = m.node_tree.nodes["Principled BSDF"]
    col = _rgb(color if color is not None else base)
    p.inputs["Base Color"].default_value = (*col, 1)
    p.inputs["Metallic"].default_value = metal if metallic is None else metallic
    p.inputs["Roughness"].default_value = rough if roughness is None else roughness
    if "coat" in extra:
        p.inputs["Coat Weight"].default_value = extra["coat"]
        p.inputs["Coat Roughness"].default_value = extra.get("coat_roughness", 0.03)
    if "transmission" in extra:
        p.inputs["Transmission Weight"].default_value = extra["transmission"]
        p.inputs["IOR"].default_value = extra.get("ior", 1.45)
    em = extra.get("emission") if emission is None else emission
    if em:
        p.inputs["Emission Color"].default_value = (*col, 1)
        p.inputs["Emission Strength"].default_value = em
    if extra.get("alpha") is not None:
        p.inputs["Alpha"].default_value = extra["alpha"]
        if hasattr(m, "blend_method"):
            m.blend_method = "BLEND"
    return m


def _link(ob, material=None, parent=None, smooth=True, name=None):
    if name:
        ob.name = name
    if material is not None:
        ob.data.materials.clear()
        ob.data.materials.append(material)
    if parent is not None:
        ob.parent = parent
    if smooth and getattr(ob.data, "polygons", None) is not None:
        for p in ob.data.polygons:
            p.use_smooth = True
    return ob


def bevel(ob, width=0.02, segments=4):
    """Rounded edges (applied on export): what makes a shape look made rather than drawn."""
    m = ob.modifiers.new("Bevel", "BEVEL")
    m.width, m.segments, m.limit_method = width, segments, "ANGLE"
    m.harden_normals = True
    return ob


def empty(name="Group", parent=None):
    ob = bpy.data.objects.new(name, None)
    bpy.context.scene.collection.objects.link(ob)
    if parent is not None:
        ob.parent = parent
    return ob


def _mesh(name, bm):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def box(size=(1, 1, 1), at=(0, 0, 0), material=None, bevel_width=0.02, parent=None, name="Box"):
    """A box (sizes along x, y, z), its edges rounded by bevel_width (0: sharp)."""
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1)
    bmesh.ops.scale(bm, vec=size, verts=bm.verts)
    ob = _mesh(name, bm)
    ob.location = at
    if bevel_width:
        bevel(ob, bevel_width)
    return _link(ob, material, parent)


def cylinder(radius=0.5, depth=0.2, at=(0, 0, 0), material=None, bevel_width=0.01, segments=64, parent=None,
             name="Cylinder", axis="Y"):
    """A cylinder or coin; axis is the way its round face looks ("Y": at the viewer)."""
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=segments, radius1=radius, radius2=radius, depth=depth)
    ob = _mesh(name, bm)
    ob.location = at
    if axis == "Y":
        ob.rotation_euler = (math.radians(90), 0, 0)
    elif axis == "X":
        ob.rotation_euler = (0, math.radians(90), 0)
    if bevel_width:
        bevel(ob, bevel_width)
    return _link(ob, material, parent)


def sphere(radius=0.5, at=(0, 0, 0), material=None, parent=None, name="Sphere", scale=(1, 1, 1)):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=64, v_segments=32, radius=radius)
    ob = _mesh(name, bm)
    ob.location, ob.scale = at, scale
    return _link(ob, material, parent)


def torus(major=1.0, minor=0.05, material=None, at=(0, 0, 0), parent=None, name="Torus", axis="Y"):
    bpy.ops.mesh.primitive_torus_add(major_radius=major, minor_radius=minor, major_segments=128, minor_segments=24,
                                     location=at)
    ob = bpy.context.active_object
    if axis == "Y":
        ob.rotation_euler = (math.radians(90), 0, 0)
    return _link(ob, material, parent, name=name)


def ring_slice(r_in, r_out, a0, a1, depth=0.12, bevel_width=0.015, at=(0, 0, 0), material=None, parent=None, name="Slice",
               facing="Y"):
    """One slice of a ring (angles in degrees, 0 at the right, counter-clockwise), facing the viewer."""
    bm = bmesh.new()
    segs = max(4, int(abs(a1 - a0) / 3))
    pts = [(math.cos(math.radians(a0 + (a1 - a0) * i / segs)), math.sin(math.radians(a0 + (a1 - a0) * i / segs)))
           for i in range(segs + 1)]
    outer = [bm.verts.new((c * r_out, s * r_out, 0)) for c, s in pts]
    inner = [bm.verts.new((c * r_in, s * r_in, 0)) for c, s in reversed(pts)]
    face = bm.faces.new(outer + inner)
    ext = bmesh.ops.extrude_face_region(bm, geom=[face])
    bmesh.ops.translate(bm, verts=[v for v in ext["geom"] if isinstance(v, bmesh.types.BMVert)], vec=(0, 0, depth))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    ob = _mesh(name, bm)
    ob.location = at
    if facing == "Y":
        ob.rotation_euler = (math.radians(90), 0, 0)
    if bevel_width:
        bevel(ob, bevel_width)
    return _link(ob, material, parent)


def text(s, size=0.3, depth=0.03, at=(0, 0, 0), material=None, font=None, parent=None, name=None, align="CENTER"):
    """3D lettering facing the viewer (exported as a mesh)."""
    cu = bpy.data.curves.new(name or f"text-{s[:12]}", "FONT")
    cu.body, cu.size, cu.extrude = s, size, depth
    cu.align_x, cu.align_y = align, "CENTER"
    cu.bevel_depth = depth * 0.15
    if font and os.path.exists(font):
        cu.font = bpy.data.fonts.load(font)
    ob = bpy.data.objects.new(name or f"text-{s[:12]}", cu)
    bpy.context.scene.collection.objects.link(ob)
    ob.location, ob.rotation_euler = at, (math.radians(90), 0, 0)
    if material is not None:
        ob.data.materials.append(material)
    if parent is not None:
        ob.parent = parent
    return ob


def part(ob, action=None, hover=None, title=None, app=None):
    """Makes an object a part of the interface: what a click does, how it answers the pointer ("lift", "glow",
    "both", "none"), its name for a tooltip. app="files" is the same as naming it "app:files"."""
    if app:
        ob.name = f"app:{app}"
    if action:
        ob["action"] = action
    if hover:
        ob["hover"] = hover
    if title:
        ob["title"] = title
    return ob


def turn(ob, axis="Z", degrees=360, frames=48, name="spin", start=1):
    """An animation turning ob round its own axis (the wheel's spin, a coin's flip)."""
    ob.rotation_mode = "XYZ"
    i = "XYZ".index(axis.upper())
    ob.keyframe_insert("rotation_euler", index=i, frame=start)
    ob.rotation_euler[i] += math.radians(degrees)
    ob.keyframe_insert("rotation_euler", index=i, frame=start + frames)
    ob.rotation_euler[i] -= math.radians(degrees)
    act = ob.animation_data.action
    act.name = name
    _linear(act)
    return act


def move(ob, offset=(0, 0, 0.2), frames=12, name="pop", start=1):
    """An animation moving ob by offset and back (a drawer opening, a button pressed)."""
    loc = tuple(ob.location)
    ob.keyframe_insert("location", frame=start)
    ob.location = tuple(a + b for a, b in zip(loc, offset))
    ob.keyframe_insert("location", frame=start + frames)
    ob.location = loc
    act = ob.animation_data.action
    act.name = name
    return act


def _linear(act):
    try:
        for fc in act.fcurves:
            for k in fc.keyframe_points:
                k.interpolation = "LINEAR"
    except AttributeError:  # layered actions (Blender 4.4+): keys live in channelbags
        for layer in getattr(act, "layers", []):
            for strip in layer.strips:
                for bag in getattr(strip, "channelbags", []):
                    for fc in bag.fcurves:
                        for k in fc.keyframe_points:
                            k.interpolation = "LINEAR"


def camera(distance=6.0, lens=70, height=0.0, look_at=(0, 0, 0), name="Camera"):
    """A camera in front of the model (optional: without one, the OS frames the model itself)."""
    cam = bpy.data.cameras.new(name)
    cam.lens = lens
    ob = bpy.data.objects.new(name, cam)
    bpy.context.scene.collection.objects.link(ob)
    ob.location = (look_at[0], look_at[1] - distance, look_at[2] + height)
    d = (look_at[0] - ob.location[0], look_at[1] - ob.location[1], look_at[2] - ob.location[2])
    ob.rotation_euler = (math.atan2(math.hypot(d[0], d[1]), -d[2]), 0, math.atan2(d[0], d[1]) * -1)
    bpy.context.scene.camera = ob
    return ob


def export(name, folder=None, camera=True):
    """Writes the scene as one .glb (modifiers applied, custom properties kept for parts) and says where."""
    folder = folder or MODELS
    os.makedirs(folder, exist_ok=True)
    path = name if name.endswith(".glb") and os.path.isabs(name) else os.path.join(folder, name if name.endswith(".glb") else name + ".glb")
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLB", export_extras=True, export_cameras=camera,
                              export_apply=True, export_animations=True, export_lights=False, export_yup=True)
    parts = [o.name for o in bpy.context.scene.objects if o.name.startswith("app:") or "action" in o.keys()]
    print(f"osui: wrote {path} ({len(parts)} clickable parts: {', '.join(parts) or 'none'})")
    return path

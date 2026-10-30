"""Send to OS: turns an animated armature in Blender into a clip for the
companion's figure (an .osclip file).

Made for recordings from FreeMoCap (record or load videos there, open the
result in Blender), and it also knows Mixamo, BVH (CMU-style names) and VRM
armatures. The armature's rest pose should be a T-pose (arms out level, as
FreeMoCap's is); if it isn't, give a frame where it stands in a T-pose with
--rest-frame.

The figure's skeleton uses VRM's humanoid bones in a T-pose, so each bone's
movement is carried over as the rotation it made since its rest pose (not
as positions), which keeps her proportions whatever the recorded person's
were. The hips' travel is kept separately, scaled to her size; jitter is
smoothed (a One-Euro filter); moments when a foot is planted on the ground
or a hand holds still on something (a ledge, the rim of her frame) are
marked as contacts, which the OS pins to the real world when it plays them.

In Blender: open the recording, select its armature, run this script
(Scripting > Open > Run), then File > Export > OS clip (.osclip). Or set the
playback range to the part you want and use the "Send to OS" button in the
sidebar (N) > OS tab, which writes into the OS's clip folder.

Without the window:
  blender -b recording.blend --python os_rig.py -- --out wave.osclip --name wave [--start 10 --end 80]
      [--rest-frame N] [--hips 0.78] [--smooth 1.0] [--fps 30] [--loop]
"""
import math
import os
import sys

import bpy
import mathutils

# ---- names: VRM humanoid bone <- the armature's bone ----

VRM_BONES = [
    # (vrm name, parent)
    ("hips", None), ("spine", "hips"), ("chest", "spine"), ("upperChest", "chest"), ("neck", "upperChest"),
    ("head", "neck"),
    ("leftShoulder", "upperChest"), ("leftUpperArm", "leftShoulder"), ("leftLowerArm", "leftUpperArm"),
    ("leftHand", "leftLowerArm"),
    ("rightShoulder", "upperChest"), ("rightUpperArm", "rightShoulder"), ("rightLowerArm", "rightUpperArm"),
    ("rightHand", "rightLowerArm"),
    ("leftUpperLeg", "hips"), ("leftLowerLeg", "leftUpperLeg"), ("leftFoot", "leftLowerLeg"), ("leftToes", "leftFoot"),
    ("rightUpperLeg", "hips"), ("rightLowerLeg", "rightUpperLeg"), ("rightFoot", "rightLowerLeg"),
    ("rightToes", "rightFoot"),
]
for side, s in (("left", "L"), ("right", "R")):
    VRM_BONES += [(f"{side}ThumbMetacarpal", f"{side}Hand"), (f"{side}ThumbProximal", f"{side}ThumbMetacarpal"),
                  (f"{side}ThumbDistal", f"{side}ThumbProximal")]
    for f in ("Index", "Middle", "Ring", "Little"):
        VRM_BONES += [(f"{side}{f}Proximal", f"{side}Hand"), (f"{side}{f}Intermediate", f"{side}{f}Proximal"),
                      (f"{side}{f}Distal", f"{side}{f}Intermediate")]
VRM_PARENT = dict(VRM_BONES)


def _freemocap():
    m = {"hips": "pelvis", "spine": "spine", "chest": "spine.001", "neck": "neck", "head": "face"}
    for side, s in (("left", "L"), ("right", "R")):
        m.update({f"{side}Shoulder": f"shoulder.{s}", f"{side}UpperArm": f"upper_arm.{s}",
                  f"{side}LowerArm": f"forearm.{s}", f"{side}Hand": f"hand.{s}",
                  f"{side}UpperLeg": f"thigh.{s}", f"{side}LowerLeg": f"shin.{s}", f"{side}Foot": f"foot.{s}",
                  f"{side}ThumbMetacarpal": f"thumb.01.{s}", f"{side}ThumbProximal": f"thumb.02.{s}",
                  f"{side}ThumbDistal": f"thumb.03.{s}"})
        for vf, ff in (("Index", "f_index"), ("Middle", "f_middle"), ("Ring", "f_ring"), ("Little", "f_pinky")):
            for i, part in enumerate(("Proximal", "Intermediate", "Distal")):
                m[f"{side}{vf}{part}"] = f"{ff}.0{i + 1}.{s}"
    return m


def _mixamo():
    p = "mixamorig:"
    m = {"hips": p + "Hips", "spine": p + "Spine", "chest": p + "Spine1", "upperChest": p + "Spine2",
         "neck": p + "Neck", "head": p + "Head"}
    for side, S in (("left", "Left"), ("right", "Right")):
        m.update({f"{side}Shoulder": f"{p}{S}Shoulder", f"{side}UpperArm": f"{p}{S}Arm",
                  f"{side}LowerArm": f"{p}{S}ForeArm", f"{side}Hand": f"{p}{S}Hand",
                  f"{side}UpperLeg": f"{p}{S}UpLeg", f"{side}LowerLeg": f"{p}{S}Leg", f"{side}Foot": f"{p}{S}Foot",
                  f"{side}Toes": f"{p}{S}ToeBase", f"{side}ThumbMetacarpal": f"{p}{S}HandThumb1",
                  f"{side}ThumbProximal": f"{p}{S}HandThumb2", f"{side}ThumbDistal": f"{p}{S}HandThumb3"})
        for vf, mf in (("Index", "Index"), ("Middle", "Middle"), ("Ring", "Ring"), ("Little", "Pinky")):
            for i, part in enumerate(("Proximal", "Intermediate", "Distal")):
                m[f"{side}{vf}{part}"] = f"{p}{S}Hand{mf}{i + 1}"
    return m


def _cmu():
    m = {"hips": "Hips", "spine": "Spine", "chest": "Spine1", "neck": "Neck", "head": "Head"}
    for side, S in (("left", "Left"), ("right", "Right")):
        m.update({f"{side}Shoulder": f"{S}Shoulder", f"{side}UpperArm": f"{S}Arm", f"{side}LowerArm": f"{S}ForeArm",
                  f"{side}Hand": f"{S}Hand", f"{side}UpperLeg": f"{S}UpLeg", f"{side}LowerLeg": f"{S}Leg",
                  f"{side}Foot": f"{S}Foot", f"{side}Toes": f"{S}ToeBase"})
    return m


SCHEMES = {"freemocap": _freemocap(), "mixamo": _mixamo(), "cmu": _cmu(), "vrm": {n: n for n, _ in VRM_BONES}}

# Blender (Z up, facing -Y) to the figure's space (Y up, facing +Z; left is +X in both)
C = mathutils.Matrix(((1, 0, 0), (0, 0, 1), (0, -1, 0)))
CI = C.inverted()


def detect(arm):
    names = set(b.name for b in arm.data.bones)
    best, score = None, 0
    for key, m in SCHEMES.items():
        s = sum(1 for v in m.values() if v in names)
        if s > score:
            best, score = key, s
    return best, score


def find_armature():
    ob = bpy.context.active_object
    if ob and ob.type == "ARMATURE" and detect(ob)[1] >= 8:
        return ob
    arms = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    arms.sort(key=lambda o: -detect(o)[1])
    return arms[0] if arms and detect(arms[0])[1] >= 8 else None


# ---- smoothing: One-Euro, per channel ----

class OneEuro:
    def __init__(self, rate, mincutoff=1.6, beta=0.4, dcutoff=1.0):
        self.rate, self.mc, self.beta, self.dc = rate, mincutoff, beta, dcutoff
        self.x = self.dx = None

    @staticmethod
    def _alpha(rate, cutoff):
        tau = 1.0 / (2 * math.pi * cutoff)
        return 1.0 / (1.0 + tau * rate)

    def __call__(self, x):
        if self.x is None:
            self.x, self.dx = x, 0.0
            return x
        dx = (x - self.x) * self.rate
        a_d = self._alpha(self.rate, self.dc)
        self.dx = a_d * dx + (1 - a_d) * self.dx
        cutoff = self.mc + self.beta * abs(self.dx)
        a = self._alpha(self.rate, cutoff)
        self.x = a * x + (1 - a) * self.x
        return self.x


# ---- the work ----

def sample(arm, scheme, f0, f1, rest_frame=None):
    """World rotations (the figure's space, since rest) of every mapped bone, and the hips' position, per frame."""
    scene = bpy.context.scene
    mp = SCHEMES[scheme]
    mapped = {v: s for v, s in mp.items() if s in arm.data.bones}
    mw = arm.matrix_world.to_3x3().normalized()

    def pose_rot(ev, src):
        return (mw @ ev.pose.bones[src].matrix.to_3x3()).normalized()

    if rest_frame is not None:
        scene.frame_set(rest_frame)
        ev = arm.evaluated_get(bpy.context.evaluated_depsgraph_get())
        rest = {v: pose_rot(ev, s) for v, s in mapped.items()}
        rest_hips = (arm.matrix_world @ ev.pose.bones[mapped["hips"]].head) if "hips" in mapped else None
    else:
        rest = {v: (mw @ arm.data.bones[s].matrix_local.to_3x3()).normalized() for v, s in mapped.items()}
        rest_hips = (arm.matrix_world @ arm.data.bones[mapped["hips"]].head_local) if "hips" in mapped else None
    frames, hips, ends = [], [], []
    for f in range(f0, f1 + 1):
        scene.frame_set(f)
        ev = arm.evaluated_get(bpy.context.evaluated_depsgraph_get())
        w = {}
        for v, s in mapped.items():
            d = pose_rot(ev, s) @ rest[v].inverted()
            w[v] = (C @ d @ CI).to_quaternion()
        frames.append(w)
        if "hips" in mapped:
            hips.append(C @ (arm.matrix_world @ ev.pose.bones[mapped["hips"]].head))
        # hands and feet, for contacts
        pts = {}
        for v in ("leftFoot", "rightFoot", "leftHand", "rightHand"):
            if v in mapped:
                pts[v] = C @ (arm.matrix_world @ ev.pose.bones[mapped[v]].head)
        ends.append(pts)
    return frames, hips, (C @ rest_hips if rest_hips is not None else None), ends


def to_locals(frames):
    """Each bone's rotation relative to its parent (unmapped bones turn with their parent)."""
    out = []
    for w in frames:
        world = {}
        loc = {}
        for name, parent in VRM_BONES:
            pw = world.get(parent, mathutils.Quaternion()) if parent else mathutils.Quaternion()
            wq = w.get(name, pw)
            world[name] = wq
            loc[name] = pw.inverted() @ wq
        out.append(loc)
    return out


def smooth(locals_, rate, amount):
    if amount <= 0:
        return locals_
    names = list(locals_[0].keys())
    filt = {n: [OneEuro(rate, 1.6 / amount, 0.4) for _ in range(4)] for n in names}
    prev = {}
    for fr in locals_:
        for n in names:
            q = fr[n].copy()
            if n in prev and prev[n].dot(q) < 0:  # the same rotation, the nearer sign
                q.negate()
            prev[n] = q
            vals = [filt[n][i](c) for i, c in enumerate((q.w, q.x, q.y, q.z))]
            fr[n] = mathutils.Quaternion(vals).normalized()
    return locals_


def contacts(ends, rate, floor_y):
    """Feet planted on the ground; hands holding still above the hips."""
    out = []
    for bone, kind in (("leftFoot", "ground"), ("rightFoot", "ground"), ("leftHand", "hand"), ("rightHand", "hand")):
        run = None
        for i, pts in enumerate(ends):
            p = pts.get(bone)
            ok = False
            if p is not None and i > 0 and ends[i - 1].get(bone) is not None:
                v = (p - ends[i - 1][bone]).length * rate
                if kind == "ground":
                    ok = p.y - floor_y < 0.06 and v < 0.35
                else:
                    ok = v < 0.12
            if ok and run is None:
                run = i
            if (not ok or i == len(ends) - 1) and run is not None:
                end = i if ok else i - 1
                if end - run >= (3 if kind == "ground" else 6):
                    out.append((run, end, bone, kind))
                run = None
    return out


def resample(seq, src_fps, dst_fps):
    n = len(seq)
    if n < 2 or abs(src_fps - dst_fps) < 1e-3:
        return seq, list(range(n))
    dur = (n - 1) / src_fps
    m = int(round(dur * dst_fps)) + 1
    idx = [min(n - 1, i * src_fps / dst_fps) for i in range(m)]
    return idx, idx


def write_clip(path, name, fps, loop, locals_, root, contact_list):
    bones = [n for n, _ in VRM_BONES]
    with open(path, "w", newline="\n") as f:
        f.write(f"osclip 1\nname {name}\nfps {fps:g}\nframes {len(locals_)}\nloop {1 if loop else 0}\n")
        f.write(f"root {1 if root else 0}\nbones {' '.join(bones)}\n")
        for f0, f1, bone, kind in contact_list:
            f.write(f"contact {f0} {f1} {bone} {kind}\n")
        for i, fr in enumerate(locals_):
            parts = []
            for n in bones:
                q = fr[n]
                parts.append(f"{q.x:.5f} {q.y:.5f} {q.z:.5f} {q.w:.5f}")
            line = "f " + " ".join(parts)
            if root:
                r = root[i]
                line += f" {r.x:.5f} {r.y:.5f} {r.z:.5f}"
            f.write(line + "\n")


def send(arm=None, out=None, name=None, start=None, end=None, rest_frame=None, hips=0.78, amount=1.0, fps=30.0,
         loop=False):
    arm = arm or find_armature()
    if not arm:
        raise RuntimeError("no armature with bones this knows (FreeMoCap, Mixamo, BVH or VRM names)")
    scheme, score = detect(arm)
    scene = bpy.context.scene
    f0 = scene.frame_start if start is None else start
    f1 = scene.frame_end if end is None else end
    src_fps = scene.render.fps / scene.render.fps_base
    frames, hips_pos, rest_hips, ends = sample(arm, scheme, f0, f1, rest_frame)
    locals_ = to_locals(frames)
    locals_ = smooth(locals_, src_fps, amount)
    # the hips' travel, at her scale
    root = None
    if hips_pos and rest_hips is not None and rest_hips.y > 0.2:
        k = hips / rest_hips.y
        root = [(p - rest_hips) * k for p in hips_pos]
    floor_y = min((p.y for pts in ends for b, p in pts.items() if "Foot" in b), default=0.0)
    cl = contacts(ends, src_fps, floor_y)
    # to the clip's frame rate
    if abs(src_fps - fps) > 1e-3 and len(locals_) > 1:
        n = len(locals_)
        m = int(round((n - 1) / src_fps * fps)) + 1
        new_l, new_r = [], []
        for i in range(m):
            x = min(n - 1.0, i * src_fps / fps)
            a, t = int(x), x - int(x)
            b = min(a + 1, n - 1)
            new_l.append({k: locals_[a][k].slerp(locals_[b][k], t) for k in locals_[a]})
            if root:
                new_r.append(root[a].lerp(root[b], t))
        locals_ = new_l
        root = new_r if root else None
        cl = [(int(a * fps / src_fps), int(b * fps / src_fps), bone, kind) for a, b, bone, kind in cl]
    name = name or os.path.splitext(os.path.basename(bpy.data.filepath or "clip"))[0]
    if not out:
        out = os.path.join(default_folder(), name + ".osclip")
    write_clip(out, name, fps, loop, locals_, root, cl)
    print(f"os_rig: {arm.name} ({scheme}, {score} bones known), frames {f0}-{f1} at {src_fps:g} fps -> {out} "
          f"({len(locals_)} frames at {fps:g}, {len(cl)} contacts)")
    return out


def default_folder():
    # the OS's built-in clips when this sits in its source tree (tools/figure), else beside the .blend
    here = os.path.dirname(os.path.abspath(__file__))
    tree = os.path.normpath(os.path.join(here, "..", "..", "userland", "rootfs", "usr", "share", "os", "clips"))
    if os.path.isdir(os.path.dirname(tree)):
        os.makedirs(tree, exist_ok=True)
        return tree
    return os.path.dirname(bpy.data.filepath) if bpy.data.filepath else os.getcwd()


# ---- in Blender's window ----

class OS_OT_send(bpy.types.Operator):
    """Turn the selected armature's animation (the playback range) into a clip for the OS's figure"""
    bl_idname = "os.send_clip"
    bl_label = "Send to OS"
    name: bpy.props.StringProperty(name="Clip name", default="")
    loop: bpy.props.BoolProperty(name="Loops (a walk, an idle)", default=False)
    smooth: bpy.props.FloatProperty(name="Smoothing", default=1.0, min=0.0, max=4.0)

    def invoke(self, context, event):
        if not self.name:
            self.name = os.path.splitext(os.path.basename(bpy.data.filepath or "clip"))[0]
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        try:
            out = send(name=self.name, amount=self.smooth, loop=self.loop)
        except Exception as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        self.report({"INFO"}, f"Sent: {out}")
        return {"FINISHED"}


class OS_OT_export(bpy.types.Operator):
    """Save the selected armature's animation as an OS clip (.osclip)"""
    bl_idname = "os.export_clip"
    bl_label = "OS clip (.osclip)"
    filepath: bpy.props.StringProperty(subtype="FILE_PATH")
    filter_glob: bpy.props.StringProperty(default="*.osclip", options={"HIDDEN"})
    loop: bpy.props.BoolProperty(name="Loops", default=False)

    def invoke(self, context, event):
        self.filepath = os.path.join(default_folder(),
                                     os.path.splitext(os.path.basename(bpy.data.filepath or "clip"))[0] + ".osclip")
        context.window_manager.fileselect_add(self)
        return {"RUNNING_MODAL"}

    def execute(self, context):
        name = os.path.splitext(os.path.basename(self.filepath))[0]
        try:
            send(out=self.filepath, name=name, loop=self.loop)
        except Exception as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        return {"FINISHED"}


class OS_PT_panel(bpy.types.Panel):
    bl_label = "OS figure"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "OS"

    def draw(self, context):
        arm = find_armature()
        col = self.layout.column()
        col.label(text=f"Armature: {arm.name if arm else 'none found'}")
        if arm:
            col.label(text=f"Bones: {detect(arm)[0]}")
        sc = context.scene
        col.label(text=f"Frames {sc.frame_start}-{sc.frame_end}")
        col.operator("os.send_clip", icon="EXPORT")


def menu_export(self, context):
    self.layout.operator(OS_OT_export.bl_idname)


def register():
    for c in (OS_OT_send, OS_OT_export, OS_PT_panel):
        try:
            bpy.utils.register_class(c)
        except ValueError:  # run twice in one session
            bpy.utils.unregister_class(c)
            bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_file_export.append(menu_export)


def main_cli(argv):
    import argparse
    p = argparse.ArgumentParser(prog="os_rig")
    p.add_argument("--out")
    p.add_argument("--name")
    p.add_argument("--start", type=int)
    p.add_argument("--end", type=int)
    p.add_argument("--rest-frame", type=int)
    p.add_argument("--hips", type=float, default=0.78)
    p.add_argument("--smooth", type=float, default=1.0)
    p.add_argument("--fps", type=float, default=30.0)
    p.add_argument("--loop", action="store_true")
    a = p.parse_args(argv)
    send(out=a.out, name=a.name, start=a.start, end=a.end, rest_frame=a.rest_frame, hips=a.hips, amount=a.smooth,
         fps=a.fps, loop=a.loop)


if __name__ == "__main__":
    if "--" in sys.argv:
        main_cli(sys.argv[sys.argv.index("--") + 1:])
    else:
        register()

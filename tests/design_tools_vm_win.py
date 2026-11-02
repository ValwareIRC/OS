"""The companion's design tools, in the OS, with Windows QEMU (Hyper-V) and silent
sound; tests/fake_ai.py plays a companion whose model can see:
  it styles with things that do nothing here, and is told so, all of it;
  having changed the look, it is shown pictures of it and says [[ok]];
  it writes ~/Pictures/logo.svg itself and puts it in the taskbar;
  it bevels a button the Windows 95 way and turns text bottom to top;
  a rule of its own that fights is taken out with ui:unstyle;
  [[undo]] takes the file back.
  set ISO=build\\test.iso & python tests\\design_tools_vm_win.py [OUTDIR]"""
import json, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = vmkit.ROOT
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "design-vm"))
os.makedirs(out, exist_ok=True)

logo = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64"><rect x="4" y="4" width="26" height="26" '
        'fill="#f25022"/><rect x="34" y="4" width="26" height="26" fill="#7fba00"/><rect x="4" y="34" width="26" '
        'height="26" fill="#00a4ef"/><rect x="34" y="34" width="26" height="26" fill="#ffb900"/></svg>')
replies = [
    {"when": r"make it retro", "reply": "Going retro. [[ui:style #bar { border-top: 2px outset #c0c0c0; filter: "
                                        "blur(2px); text-decoration: underline; }]]"},
    {"when": r"how it looks now", "reply": "[[ok]]"},
    {"when": r"a logo", "reply": "Drawn and saved. [[write:~/Pictures/logo.svg]]\n" + logo + "\n[[/write]]"},
    {"when": r"put it in", "reply": "There. [[ui:add #bar <image id=\"logo\" src=\"~/Pictures/logo.svg\" "
                                    "style=\"height: 20\"/>]] [[ui:add #bar <text id=\"spine\">Mitaware</text>]] "
                                    "[[ui:style #spine { writing-mode: sideways-lr; color: #000; font-size: 11; }]] "
                                    "[[ui:style #start { box-shadow: inset -1px -1px #0a0a0a, inset 1px 1px #ffffff, "
                                    "inset -2px -2px #808080, inset 2px 2px #dfdfdf; }]]"},
    {"when": r"bigger bar", "reply": "[[ui:style taskbar { height: 40; }]]"},
    {"when": r"take that out", "reply": "Gone. [[ui:unstyle taskbar { height }]]"},
    {"when": r"undo the logo", "reply": "Taken back. [[undo]]"},
]
for _ in range(12):
    replies.append({"when": r"how it looks now", "reply": "[[ok]]"})
for _ in range(6):
    replies.append("Mm.")

vm = vmkit.Vm(out, replies, alive="proactive=0\n", model="fake-vision",
              design_path=os.path.join(ROOT, "tests", "designs", "w95.xml"))
results = {}


def ai_log():
    try:
        return [json.loads(l) for l in open(vm.ailog, encoding="utf-8") if l.strip()]
    except OSError:
        return []


try:
    vm.start()
    results["desktop"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    time.sleep(6)
    vm.focus_companion()
    vm.say("make it retro", r"ui command [^\n]*#bar[^\n]*")
    told = vm.wait_for(r'styled #bar, but: [^\n]*filter[^\n]*has no effect[^\n]*text-decoration[^\n]*has no effect', 10)
    results["told what does nothing (all of it)"] = bool(told)
    time.sleep(8)
    seen = [e for e in ai_log() if e.get("pictures")]
    results["shown pictures of the change"] = bool(seen)
    if seen:
        print("    pictures in a request:", seen[0]["pictures"])
    vm.say("draw me a logo", r"write: wrote /root/Pictures/logo.svg")
    results["wrote the file"] = "write: wrote /root/Pictures/logo.svg" in vm.log_text()
    vm.say("put it in the bar", r'ui command "add #bar <image')
    time.sleep(8)
    log = vm.log_text()
    results["image added"] = bool(re.search(r'ui command "add #bar <image[^\n]*: added', log))
    results["spine turned"] = bool(re.search(r'ui command "style #spine[^\n]*: styled #spine\n', log))
    results["bevel taken"] = bool(re.search(r'ui command "style #start \{ box-shadow[^\n]*: styled #start\n', log))
    vm.shot("1-retro.png")
    vm.say("bigger bar", r'ui command "style taskbar')
    time.sleep(6)
    vm.say("take that out", r'ui command "unstyle taskbar')
    results["unstyle"] = bool(re.search(r'ui command "unstyle taskbar \{ height \}"[^\n]*took those properties out',
                                        vm.log_text()))
    time.sleep(6)
    vm.say("undo the logo", r"companion|voice")
    time.sleep(4)
    vm.shot("2-after.png")
    for line in re.findall(r"shell: ui command [^\n]*", vm.log_text())[-12:]:
        print("   ", line[:220])
finally:
    vm.stop()
    for k, v in results.items():
        print(f"{k}: {v}")

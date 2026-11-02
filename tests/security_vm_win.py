"""The install-safety layer in the OS itself (Windows QEMU, silent sound, keys and clicks through QMP):
  - the companion writes a notes app that reaches any server: nothing runs; the system's dialog lists what its
    code can do, with the rules' warning; the companion is told only counts, kinds and numbers (none of the app's
    text); they allow it and it runs;
  - an update adds a server: it waits, showing what it already had and what is new;
  - Settings > Security lists the app's permissions; turning warnings off is refused before the wait even with the
    phrase typed, and works after it; the permissions are still listed;
  - Settings opened again by anything (here the launcher) never opens onto Security.
  python tests\\security_vm_win.py [OUTDIR]"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "security-vm"))
os.makedirs(out, exist_ok=True)
V1 = open(os.path.join(vmkit.ROOT, "tests", "facet2", "security", "c_notepad.facet")).read()
V2 = V1.replace('    net.post("https://" + where + "/in", draft)',
                '    net.post("https://" + where + "/in", draft)\n    net.post("https://sync.pad.test/save", draft)')
replies = [
    {"when": r"make me a pad", "reply": "[[app:Pad]]\n" + V1 + "[[/app]]"},
    {"when": r"update the pad", "reply": "[[app:Pad]]\n" + V2 + "[[/app]]"},
    {"when": r"An app is asking to be allowed", "reply": "It can reach any server, which a notes app shouldn't need."},
] + ["Mm."] * 8
vm = vmkit.Vm(out, replies, qport=4477, aiport=8921)
results = {}
PHRASE = "I understand this puts my computer at risk"


def log():
    return vm.log_text()


def rect(name, since=0):
    m = re.findall(r"rect %s (-?\d+) (-?\d+) (\d+) (\d+)" % re.escape(name), log()[since:])
    return tuple(map(int, m[-1])) if m else None


def click_rect(r):
    vm.click(r[0] + r[2] // 2, r[1] + r[3] // 2)


try:
    vm.start()
    results["desktop"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    time.sleep(6)
    vm.focus_companion()
    # the companion writes an app: the gate, not the app
    mark = len(log())
    vm.say("make me a pad", r"appgate: asking about", 90)
    asked = vm.wait_for(r"appgate: asking about [^\n]*Pad\.facet: (\d+) new, (\d+) held, rules level (\d)", 30, mark)
    results["nothing runs before they decide"] = bool(asked) and "appgate: " not in log()[mark:].split("asking about")[0]
    results["the rules warn (level 2) with no model"] = bool(asked and asked.group(3) == "2")
    time.sleep(4)
    vm.shot("1-gate.png")
    told = vm.wait_for(r"An app is asking to be allowed", 60)
    ai = open(vm.ailog, encoding="utf-8", errors="replace").read()
    i = ai.find("An app is asking to be allowed")
    note = ai[i:ai.find("\"}", i)] if told and i >= 0 else ""  # the note alone (the rest is their conversation)
    results["the companion is told"] = bool(told)
    results["the companion is told nothing of the app's text"] = bool(told) and not any(
        s in note for s in ("collect.example.test", "Pad.txt", "draft", "where", "Keep", "Pad"))
    allow = rect("appgate-allow", mark)
    if allow:
        click_rect(allow)
    results["allowed, it runs"] = bool(vm.wait_for(r"appgate: [^\n]*Pad\.facet allowed", 15, mark))
    time.sleep(3)
    # an update with a new server
    vm.focus_companion()
    mark = len(log())
    vm.say("update the pad", r"appgate: asking about", 90)
    up = vm.wait_for(r"appgate: asking about [^\n]*Pad\.facet: (\d+) new, (\d+) held", 30, mark)
    results["an update waits: new vs held"] = bool(up and int(up.group(1)) >= 1 and int(up.group(2)) >= 1)
    time.sleep(3)
    vm.shot("2-update.png")
    allow = rect("appgate-allow", mark)
    if allow:
        click_rect(allow)
    vm.wait_for(r"appgate: [^\n]*Pad\.facet allowed", 15, mark)
    time.sleep(2)
    # Settings > Security, by their click
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.5)
    vm.type("settings")
    time.sleep(2)
    vm.key("ret")
    time.sleep(4)
    sec = rect("settings-page-Security")
    if sec:
        click_rect(sec)
    time.sleep(3)
    vm.shot("3-security.png")
    tog = rect("security-toggle")
    mark = len(log())
    if tog:
        click_rect(tog)
    results["the turn-off dialog opens from its button"] = bool(vm.wait_for(r"security: the turn-off dialog opened", 10, mark))
    time.sleep(1)
    vm.type(PHRASE)
    time.sleep(1)
    go = rect("security-turnoff", mark)
    vm.shot("4-turnoff.png")
    if go:
        click_rect(go)  # too soon: the wait isn't over
    time.sleep(2)
    results["refused before the wait, phrase typed"] = "turned off by them" not in log()[mark:]
    time.sleep(10)
    if go:
        click_rect(go)
    results["off after the wait"] = bool(vm.wait_for(r"security: warnings and second opinions turned off by them", 10, mark))
    time.sleep(2)
    vm.shot("5-off.png")
    results["the permissions are still listed"] = rect("security-toggle", mark) is not None
    # Settings closed and opened again by something else: never onto Security
    vm.key("esc")
    time.sleep(1)
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "f4"}])
    time.sleep(2)
    mark = len(log())
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.5)
    vm.type("settings")
    time.sleep(2)
    vm.key("ret")
    opened = vm.wait_for(r"settings: opened on (\w+)", 15, mark)
    results["opened again, not onto Security"] = bool(opened and opened.group(1) != "Security")
    if opened:
        print("   ", opened.group(0))
finally:
    vm.stop()
    for line in re.findall(r"shell: (?:appgate|security|settings: opened)[^\n]*", log())[-20:]:
        print("   ", line[:200])
    for k, v in results.items():
        print(f"{'PASS' if v else 'FAIL'} {k}")

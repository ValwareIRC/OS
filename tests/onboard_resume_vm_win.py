"""Onboarding keeps track (what went wrong in a real run, 2026-11-03):
  - a pick it offered was on screen: when they choose, it is told what was shown, so it never says "I don't
    think they showed up" and offers them again; offered again anyway, it is told they already answered;
  - the desktop program crashing mid-way (here: killed with SIGSEGV once the draft is in) brings them back to
    the same place, the draft and the conversation, never the very first screen;
  - a new draft is told to it as new, with pictures for a model that sees.
tests/fake_ai.py plays the companion. Windows QEMU, silent sound, a fresh disk.

  python tests\\onboard_resume_vm_win.py [OUTDIR]"""
import json, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "onboard-resume-vm"))
os.makedirs(out, exist_ok=True)
ROOMS = ("[[pick:room|Morning tiles:sun:#f3e9dc,#e0a458,#3d7ea6,#5b3a29:sunlight on blue tiles|Harbour dusk:waves:"
         "#1d2b3a,#e8a87c,#9fd3c7,#0e1621:the sea going violet|Paper and salt:paper:#f7f3ea,#c9a227,#4a6c6f,#2b2b2b:"
         "quiet and bright]]")
replies = [
    {"when": "Say hello", "reply": "Hello. What should I call you?"},
    {"when": "Valware$", "reply": "Nice to meet you, Valware. [[user:Valware]] Which feels like home? " + ROOMS},
    {"when": "Harbour dusk", "reply": "I don't think they showed up, so here they are again. " + ROOMS},
    {"when": "offered the room choice again", "reply": "Right, Harbour dusk. I'll be Ember. [[name:Ember]] [[done]]"},
    {"when": "first draft of their space", "reply": "Here it is. [[sh: kill -SEGV $(pidof os-shell)]]"},
    {"when": "stopped because of a fault", "reply": "That had to restart, but I'm back where we were."},
] + [{"when": "Not (yet|quite yet)", "reply": "All right. [[done]]"}] * 5 + ["Okay."] * 30
results = {}
vm = vmkit.Vm(out, replies, memory=os.environ.get("MEM", "12G"), facet=False, qport=4480, aiport=8924,
              model="gpt-4o-stand-in")


def requests():
    return [json.loads(l) for l in open(vm.ailog) if l.strip()]


def told(text):
    return any(text in t.get("content", "") for r in requests() for t in r["tail"])


try:
    vm.start()
    vm.wait_for(r"screen onboard", 300)
    time.sleep(6)
    vm.key("2")  # comfort: as it is
    vm.wait_for(r"shell: ai Hello", 60)
    time.sleep(3)
    vm.type("Valware\n")
    vm.wait_for(r"pick shown room", 60)
    time.sleep(3)
    m = len(vm.log_text())
    vm.key("2")  # Harbour dusk
    vm.wait_for(r"picked room", 10, m)
    time.sleep(8)
    results["told what was on screen with the answer"] = told("What you offered was on the screen for them")
    results["offered again: told they already answered"] = bool(
        vm.wait_for(r"pick room offered again: told it was answered", 30, m))
    time.sleep(6)
    results["...and it reads that"] = told("offered the room choice again")
    results["the draft comes in"] = bool(vm.wait_for(r"preview: told the companion draft 1 is in, with pictures", 400))
    vm.shot("1-draft.png")
    results["the desktop crashed (as the test made it)"] = bool(vm.wait_for(r"init: the desktop exited", 90))
    results["onboarding picks up where it was, with the draft"] = bool(
        vm.wait_for(r"onboarding: picked up where it was \(\d+ messages, the draft\)", 120))
    results["not the first screen"] = not re.search(r"pick shown comfort", vm.log_text()[vm.log_text().find("init: the desktop exited"):])
    results["it knows it restarted, and says so"] = bool(vm.wait_for(r"shell: ai That had to restart", 90))
    time.sleep(4)
    vm.shot("2-after-restart.png")
finally:
    vm.stop()
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)

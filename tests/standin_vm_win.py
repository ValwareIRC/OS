"""The built-in model (5 GB) stays on the disc while the companion's mind is elsewhere, and stands in only with a
yes: Windows QEMU, silent sound. The mind is tests/fake_ai.py; the boot must not read the model; then the fake
mind stops answering, the companion is asked something, and the question "may the one on this computer stand
in?" must appear; yes reads the model from the disc, starts it, and what went unanswered is answered by it; the
fake mind comes back and the companion goes back to it.

  set ISO=build\\dev.iso & python tests\\standin_vm_win.py"""
import os, re, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.join(ROOT, "build", "standin")
os.makedirs(out, exist_ok=True)
results = {}
disk = os.path.join(out, "disk.img")  # (a disk, as a real system has: downloads go there, not into memory)
vmkit.Vm.new_disk(disk, 24 << 30)
vm = vmkit.Vm(out, ["Hello! I'm here."] * 20, memory=os.environ.get("MEM", "12G"), disk=disk, qport=4486, aiport=8930)
try:
    vm.start()
    results["the desktop starts"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    results["the model is left on the disc at boot"] = bool(
        re.search(r"medium: /usr/share/os/mind/model\.gguf left on the disc until it is asked for", vm.log_text()))
    time.sleep(40)  # (long enough that a background read, as before, would have begun and shown)
    results["nothing reads it while the mind is elsewhere"] = "model.gguf read" not in vm.log_text() and \
        "asked for from the disc" not in vm.log_text()
    vm.focus_companion()
    results["the companion answers (its own mind)"] = bool(vm.say("hello", r"shell: ai Hello! I'm here", 90))

    # the mind goes away
    vm.server.terminate()
    vm.server.wait(10)
    m = len(vm.log_text())
    vm.type("are you still there?\n")
    asked = vm.wait_for(r"rect standin-yes (\d+) (\d+) (\d+) (\d+)", 90, m)
    results["asked whether the one here may stand in"] = bool(asked)
    results["not switched before the yes"] = "standing in for" not in vm.log_text()[m:]
    vm.shot("1-asked.png")
    if asked:
        x, y, w, h = map(int, asked.groups())
        t_yes = time.time()
        vm.click(x + w // 2, y + h // 2)
        results["yes: it stands in"] = bool(vm.wait_for(r"mind: standing in for", 20, m))
        results["the model is asked for from the disc"] = bool(vm.wait_for(r"medium: .*model\.gguf asked for", 30, m))
        read = vm.wait_for(r"medium: /usr/share/os/mind/model\.gguf read \((\d+) MiB in (\d+) ms", 600, m)
        results["read from the disc"] = bool(read)
        if read:
            print(f"PERF standin_read_s={int(read.group(2)) / 1000:.1f}", flush=True)
        ready = vm.wait_for(r"mind: the one on this computer is standing in$|the stand-in couldn't start", 900, m)
        results["it starts"] = bool(ready) and "couldn't" not in ready.group(0)
        if not results["it starts"]:
            ready = None
            raise SystemExit  # (the rest needs it running; the log says why below)
        print(f"PERF standin_ready_s={time.time() - t_yes:.0f}", flush=True)
        # what went unanswered, answered by the model here (not the fake: it is gone)
        ans = vm.wait_for(r"shell: ai (?!Hello! I'm here)(.{1,80})", 900, ready.end() + m if ready else m)
        results["what went unanswered is answered here"] = bool(ans)
        if ans:
            print(f"  answered: {ans.group(1)!r}", flush=True)
        vm.shot("2-standing-in.png")

        # the mind comes back: the companion goes back to it within about a minute
        vm.server = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_ai.py"), str(vm.aiport),
                                      vm.script, vm.ailog])
        m2 = len(vm.log_text())
        results["back to its own mind when it answers"] = bool(vm.wait_for(r"answers again: it is theirs again", 150, m2))
        results["the one here stops"] = bool(vm.wait_for(r"models: the built-in model let go of memory|mind: .*stopp|"
                                                         r"answers again", 5, m2))
        time.sleep(3)
        results["the companion answers with its own mind again"] = bool(vm.say("thank you", r"shell: ai Hello! I'm here", 90))
finally:
    vm.stop()
for l in re.findall(r".*(?:medium:|mind:|standin|stand-in).*", vm.log_text())[-24:]:
    print("  " + l[:200])
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)

"""Firefox small and at full size: how long the same pages take to load and to scroll at each, and where the
kernel's time goes meanwhile (/proc/kprofile, per phase), so what makes apps slow at full size can be found.

Firefox (from Flathub, on a fresh disk) is maximized (Super+Up), then made a quarter of the screen (Super+Down to
restore, Super+Left, Super+Up). At each size it loads about:support, scrolls it, and loads about:preferences; each
is timed until the screen settles (unchanged for 0.6 s). An autorun script in the system restarts the kernel's
profile at every change of size (the desktop logs "wm snap ...") and prints the last phase's, with /proc/vmstat.

  python tests\\appsize_vm_win.py [OUTDIR]"""
import hashlib, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "appsize-vm"))
os.makedirs(out, exist_ok=True)
disk = os.path.join(out, "disk.img")
vmkit.Vm.new_disk(disk, 24 << 30)
os.environ["DISK_RW"] = "1"
sampler = os.path.join(out, "sample.sh")
with open(sampler, "w", newline="\n") as f:
    f.write(r"""#!/bin/sh
# At every "wm snap" in the desktop's log: the phase before it, printed (the kernel's profile, its counters, the
# app's processes), and the profile started again.
dump() {
    echo "kprof begin $1"
    grep -E '^(ticks|sys|prog) ' /proc/kprofile
    grep '^pc ' /proc/kprofile | sort -k3 -n -r | head -150
    grep -E '^(pgfault|pgmajfault|fault_us|cow|nr_tlb_remote_flush|tlb_remote_flush_us) ' /proc/vmstat | tr '\n' ' '
    echo
    for d in /proc/[0-9]*; do
        line=$(cat $d/stat 2>/dev/null) || continue
        case "$line" in *firefox* | *Web\ Co* | *Isolated* | *Privileged* | *WebExt* | *RDD* | *shell*) ;; *) continue ;; esac
        rest=${line#*) }
        set -- $rest
        flt=$8
        shift 11
        echo "proc ${line%% *} $(echo "$line" | sed 's/^[0-9]* (\(.*\)) .*/\1/' | tr ' ' '_') cpu $1+$2 flt $flt"
    done
    echo "kprof end $1"
}
seen=0
phase=start
echo 1 > /proc/kprofile
while :; do
    sleep 0.5
    n=$(grep -c 'wm snap' /tmp/os-shell.log 2>/dev/null)
    [ -n "$n" ] || continue
    if [ "$n" != "$seen" ]; then
        dump "$phase"
        echo 1 > /proc/kprofile
        seen=$n
        phase=$(grep 'wm snap' /tmp/os-shell.log | tail -1 | sed 's/.*wm snap //' | tr ' ' '-')
    fi
done
""")
os.environ["AUTORUN"] = sampler
results, times = {}, {}


def keys(*names):
    vm.cmd("send-key", keys=[{"type": "qcode", "data": n} for n in names])
    time.sleep(0.6)


def typ(text):
    for ch in text:
        if ch == ":":
            vm.key("semicolon", True)
        else:
            vm.type(ch)


def screen_hash():
    ppm = os.path.join(out, "now.ppm")
    vm.cmd("screendump", filename=ppm.replace(os.sep, "/"))
    time.sleep(0.05)
    return hashlib.md5(open(ppm, "rb").read()).hexdigest()


def settle(t0, limit=60):
    """Seconds from t0 until the screen stops changing (the same for 0.6 s)."""
    last, same, at = None, 0, None
    while time.time() - t0 < limit:
        h = screen_hash()
        if h == last:
            same += 1
            if same >= 3:
                return at - t0
        else:
            same, at = 0, time.time()
        last = h
        time.sleep(0.2)
    return None


def page(url, name):
    keys("ctrl", "l")
    typ(url)
    t0 = time.time()
    vm.key("ret")
    s = settle(t0)
    vm.shot(name + ".png")
    times[name] = s
    print(f"PERF {name}: settled after {s if s is not None else '-'} s", flush=True)
    return s


def scroll(name):
    t0 = time.time()
    for _ in range(15):
        vm.key("pgdn")
        time.sleep(0.08)
    s = settle(t0)
    times[name] = s
    print(f"PERF {name}: settled after {s if s is not None else '-'} s", flush=True)


def phase(size):
    page("about:support", f"{size}-support")
    scroll(f"{size}-scroll")
    page("about:preferences", f"{size}-preferences")
    sizes = re.findall(r"app window \d+: .*? (\d+x\d+)|wm snap win-\d+ (\S+)", vm.log_text())
    print(f"  {size}: {sizes[-3:]}", flush=True)


vm = vmkit.Vm(out, ["Okay."] * 10, memory=os.environ.get("MEM", "12G"), disk=disk, qport=4486, aiport=8930)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 300)
    vm.wait_for(r"launcher: Flathub offers [1-9]\d* apps", 600)
    time.sleep(3)
    m = len(vm.log_text())
    keys("alt", "spc")
    vm.type("firefox")
    time.sleep(4)
    vm.key("ret")
    got = vm.wait_for(r"launcher: installed org\.mozilla\.firefox|launcher: .*install.* failed", 1200, m)
    results["Firefox installed"] = bool(got) and "installed" in got.group(0)
    vm.wait_for(r"window: first buffer", 120, m)
    time.sleep(12)
    vm.key("ret")  # (its first-run terms: Continue)
    time.sleep(4)
    keys("meta_l", "up")  # the whole screen
    time.sleep(4)
    phase("full")
    keys("meta_l", "down")  # back, then a quarter
    time.sleep(2)
    keys("meta_l", "left")
    time.sleep(2)
    keys("meta_l", "up")
    time.sleep(4)
    phase("quarter")
    keys("meta_l", "down")  # (the last phase's profile, printed)
    time.sleep(6)
finally:
    vm.stop()
text = vm.log_text()
for name, s in times.items():
    print(f"  {name}: {s} s")
for label, body in re.findall(r"kprof begin (\S+)\n(.*?)kprof end", text, re.S):
    open(os.path.join(out, f"kprof-{label}.txt"), "a").write(body)
print("profiles:", sorted(set(re.findall(r"kprof begin (\S+)", text))))
results["loads at both sizes"] = all(v is not None for v in times.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if all(results.values()) else "SOME FAILED")

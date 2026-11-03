"""What the desktop costs while nothing happens: the desktop up on a fresh disk, a minute to settle, then ten seconds
measured from /proc inside the VM (through the companion's shell tag, the stand-in AI asking for it): the desktop
process's share of one core, and how busy the whole machine is and how much of that is the kernel. Prints PERF
lines (the Performance fixes panel reads them).  python tests\\perf_idle_vm_win.py   (ISO= the image; dev.iso)"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.join(vmkit.ROOT, "build", "perf-idle")
os.makedirs(out, exist_ok=True)
os.environ.setdefault("ISO", os.path.join(vmkit.ROOT, "build", "dev.iso"))
disk = vmkit.Vm.new_disk(os.path.join(out, "disk.img"), 8 << 30)
# each process: "pid (name) utime stime", sorted for join; /proc/stat's first line for the whole machine
snap = "for p in /proc/[0-9]*; do echo \"${p#/proc/} $(cut -d' ' -f2,14,15 $p/stat 2>/dev/null)\"; done | sort"
# every thread of the desktop, by name (names may have spaces: the fields after the last ")"), and its ticks
tsnap = ("awk '{i=index($0,\") \"); split(substr($0,i+2),f,\" \"); c=substr($0,index($0,\"(\"),i-index($0,\"(\")+1); "
         "print FILENAME, c, \"user\", f[12], \"kernel\", f[13], f[12]+f[13]}' /proc/$(pidof os-shell | tr ' ' '\n' | sort -n | head -1)/task/*/stat")
# (the snapshots are taken outside the measured ten seconds: they run a few hundred small programs themselves)
cmd = (f"{snap} > /tmp/pa; {tsnap} > /tmp/ta; head -1 /proc/stat > /tmp/sa; sleep 10; head -1 /proc/stat > /tmp/sb; {snap} > /tmp/pb; {tsnap} > /tmp/tb; "
       "echo MEASURED; awk 'FNR==NR {a[$1]=$NF; next} ($1 in a) {d=$NF-a[$1]; if (d>0) print d, $0, \"was\", a[$1]}' /tmp/ta /tmp/tb | sort -rn | head -6; "
       "awk 'FNR==NR {a[$1]=$3+$4; next} ($1 in a) {d=$3+$4-a[$1]; if (d>0) print d, $2}' /tmp/pa /tmp/pb | sort -rn | head -8; "
       "awk 'FNR==NR {a[$1]=$3+$4; next} ($1 in a) && $2==\"(os-shell)\" {s+=$3+$4-a[$1]} END {p=s/10; print \"PERF idle_desktop_cpu_pct=\" p}' /tmp/pa /tmp/pb; "
       "cat /tmp/sa /tmp/sb | awk 'NR==1 {for (i=2;i<=NF;i++) a[i]=$i} NR==2 {for (i=2;i<=NF;i++) {d=$i-a[i]; t+=d; if (i==4) k=d; if (i==5) idle=d}} "
       "END {b=(t-idle)*100/t; kp=k*100/t; print \"PERF idle_machine_busy_pct=\" b; print \"PERF idle_kernel_pct=\" kp}'")
replies = [{"when": r"measure the desktop", "reply": "[[sh: " + cmd + "]]"}, {"when": r"MEASURED", "reply": "Done."}] + ["Okay."] * 8
# ALWAYS_DRAW=1: every frame drawn, as before (the same image otherwise, to compare)
alive = "proactive=0\n" + ("always_draw=1\n" if os.environ.get("ALWAYS_DRAW") else "")
vm = vmkit.Vm(out, replies, alive=alive, memory=os.environ.get("MEM", "8G"), disk=disk, qport=4504, aiport=8948)


def qemu_cpu_s():
    """The QEMU process's processor time so far (all its threads: every virtual CPU), from Windows itself."""
    import ctypes
    from ctypes import wintypes
    c, e, k, u = (wintypes.FILETIME() for _ in range(4))
    ctypes.windll.kernel32.GetProcessTimes(wintypes.HANDLE(vm.qemu._handle), ctypes.byref(c), ctypes.byref(e),
                                           ctypes.byref(k), ctypes.byref(u))
    return sum((t.dwHighDateTime << 32 | t.dwLowDateTime) for t in (k, u)) / 1e7
found = []
try:
    vm.start()
    if not vm.wait_for(r"shell: screen desktop", 300):
        print("the desktop didn't come up")
    time.sleep(60)  # (past the start: models settled, the first greeting said)
    c0, t0 = qemu_cpu_s(), time.time()
    time.sleep(20)  # nothing happening: what the idle desktop costs this PC
    host = (qemu_cpu_s() - c0) / (time.time() - t0) * 100
    vm.focus_companion()
    t0 = time.time()
    vm.say("measure the desktop", r"ai \[\[sh", 120)
    while time.time() - t0 < 120:
        found = re.findall(r"PERF \w+=[\d.]+", open(vm.ailog, encoding="utf-8", errors="replace").read())
        if len(found) >= 3:
            break
        time.sleep(1)
finally:
    vm.stop()
raw = re.search(r"MEASURED\\n(.*?)\\n\\n", open(vm.ailog, encoding="utf-8", errors="replace").read())
if raw:
    print(raw.group(1).replace("\\n", "\n"))
print("\n".join(dict.fromkeys(found)) if found else "no measurement came back (see build/perf-idle/ai.log)")
if "host" in globals():  # (of one of this PC's cores: the whole VM, every virtual CPU and QEMU itself)
    print(f"PERF idle_vm_host_cpu_pct={host:.1f}")

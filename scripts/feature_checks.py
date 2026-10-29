#!/usr/bin/env python3
"""The new desktop features, end to end on the main tree: one VM run per
check, one summary line per run on stdout.

As it goes it writes build/feature-checks/events.jsonl (one JSON object a
line: the suite starting, each run starting, each PASS/FAIL line as it
appears, each run ending with its counts and the errors in its serial log)
and keeps each run's whole output in build/feature-checks/<run>.log. The
"Feature test runs" gadget in Prism watches those.

  python3 scripts/feature_checks.py [--only NAME,NAME...] [--list]

NAME matches a run's name or a part of it ("tray", "xp", "alive").
"""
import json, os, re, subprocess, sys, threading, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT = os.path.join(ROOT, "build", "feature-checks")
EVENTS = os.path.join(OUT, "events.jsonl")

# name, command (relative to ROOT), the run's output folder (its serial.log)
RUNS = [
    ("launcher (orbit)", "python3 tests/launcher_test.py tests/designs/orbit.xml {out} panel", "build/fc-launcher"),
    ("launcher (xp)", "python3 tests/launcher_test.py tests/designs/xp.xml {out} attached", "build/fc-launcher-xp"),
    ("windows (xp)", "python3 tests/wm_test.py tests/designs/xp.xml {out}", "build/fc-wm"),
    ("apps (paper)", "python3 tests/apps_test.py tests/designs/paper.xml {out}", "build/fc-apps-paper"),
    ("apps (night)", "python3 tests/apps_test.py tests/designs/night.xml {out}", "build/fc-apps-night"),
    ("alive companion", "python3 tests/alive_test.py {out}", "build/fc-alive"),
    ("stylist", "python3 tests/stylist_test.py {out}", "build/fc-stylist"),
    ("tray (xp)", "python3 tests/tray_test.py tests/designs/xp.xml {out}", "build/fc-tray-xp"),
    ("tray (glass)", "python3 tests/tray_test.py tests/designs/glass.xml {out}", "build/fc-tray-glass"),
    ("settings (glass)", "python3 tests/settings_test.py tests/designs/glass.xml {out}", "build/fc-settings"),
    ("terminal", "python3 tests/term_test.py {out}", "build/fc-term"),
    ("boot", "bash tests/boot_test.sh", None),
    ("web browser", "python3 tests/web_test.py {out}", "build/fc-web"),
    ("title bar buttons (xp)", "python3 tests/titlebar_test.py {out}", "build/fc-titlebar"),
]
TIMEOUT = 1500

# What in a serial log is worth showing when a run ends.
TROUBLE = re.compile(r"panic|page fault|general protection|segfault|killed|Bail out|Traceback|ERROR|CRIT|"
                     r"not implemented|assertion|could not|can't|failed|error:", re.I)
CHECK = re.compile(r"^(PASS|ok|FAIL)\b\s*(.*)")


def emit(ev, **kv):
    kv.update(ev=ev, t=round(time.time(), 1))
    with open(EVENTS, "a", encoding="utf-8") as f:
        f.write(json.dumps(kv) + "\n")


def slug(name):
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def trouble(serial, limit=8):
    """The last lines of a serial log that look like something went wrong."""
    try:
        text = open(serial, encoding="utf-8", errors="replace").read()
    except OSError:
        return []
    seen, out = set(), []
    for line in text.splitlines():
        line = re.sub(r"\x1b\[[0-9;]*m", "", line).strip()
        if line and TROUBLE.search(line) and "rect " not in line and line not in seen:
            seen.add(line)
            out.append(line[:220])
    return out[-limit:]


def run_one(name, cmd, outdir):
    out = os.path.join(ROOT, outdir) if outdir else None
    log_path = os.path.join(OUT, slug(name) + ".log")
    emit("start", name=name, out=outdir, log=os.path.relpath(log_path, ROOT))
    t0 = time.time()
    ok, fails, tail = 0, [], []
    env = dict(os.environ, OS_MEM=os.environ.get("OS_MEM", "3G"),
               OS_QMP=os.environ.get("OS_QMP", "/tmp/qmp-feature.sock"), PYTHONUNBUFFERED="1")
    timed_out = False
    with open(log_path, "w", encoding="utf-8") as log:
        p = subprocess.Popen(cmd.format(out=out or "").split(), cwd=ROOT, env=env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, text=True, errors="replace")
        # a run that hangs without a word still ends: the watchdog kills it
        def overdue():
            nonlocal timed_out
            timed_out = True
            p.kill()
        dog = threading.Timer(TIMEOUT, overdue)
        dog.start()
        for line in p.stdout:
            log.write(line)
            log.flush()
            line = line.rstrip()
            tail = (tail + [line])[-40:]
            m = CHECK.match(line)
            if m and m.group(2):
                passed = m.group(1) != "FAIL"
                ok += passed
                if not passed:
                    fails.append(m.group(2)[:120])
                emit("check", name=name, passed=passed, text=m.group(2)[:160])
        p.wait()
        dog.cancel()
    summary = [l for l in tail if re.search(r"launcher test:|failure\(s\)|^PASS$|^\d+/\d+ passed|Traceback|"
                                            r"TimeoutError|\[(uefi|bios)\] (PASS|FAIL|SKIP)", l)][-3:]
    crashed = any("Traceback" in l or "TimeoutError" in l for l in tail)
    good = not fails and not timed_out and not crashed and p.returncode == 0
    errors = trouble(os.path.join(out, "serial.log")) if out else []
    if crashed:
        errors = [l for l in tail if l.strip()][-6:] + errors
    emit("end", name=name, passed=good, ok=ok, fails=fails, secs=round(time.time() - t0),
         timed_out=timed_out, code=p.returncode, summary=summary, errors=errors[-12:])
    word = "passed" if good else "FAILED"
    extra = ("; failed: " + "; ".join(f[:60] for f in fails)) if fails else ""
    extra += " (timed out)" if timed_out else " (crashed)" if crashed else ""
    print(f"{name}: {word}, {ok} checks passed{extra}", flush=True)
    return good


def main():
    args = sys.argv[1:]
    if "--list" in args:
        for n, _, _ in RUNS:
            print(n)
        return 0
    runs = RUNS
    if "--only" in args:
        want = [w.strip().lower() for w in args[args.index("--only") + 1].split(",") if w.strip()]
        runs = [r for r in RUNS if any(w in r[0].lower() for w in want)]
    os.makedirs(OUT, exist_ok=True)
    # a group of its own: stopping the suite (kill -TERM -PID) takes its
    # tests and their VMs with it, and nothing else
    try:
        os.setpgrp()
    except OSError:
        pass
    emit("suite", runs=[r[0] for r in runs], pid=os.getpid())
    bad = 0
    for name, cmd, outdir in runs:
        bad += not run_one(name, cmd, outdir)
    emit("done", failed=bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

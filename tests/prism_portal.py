#!/usr/bin/env python3
"""Prism as a Flatpak, end to end: opens it through the launcher, waits for
its window, then asks it to open a folder (Ctrl+O). The request must travel
app -> xdg-desktop-portal -> os-portal -> the shell, which shows the OS's
own chooser as a modal over Prism. Photographs each stage.

  python3 tests/prism_portal.py OUT_DIR [WAIT_SECONDS]
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = os.path.abspath(sys.argv[1])
wait = int(sys.argv[2]) if len(sys.argv) > 2 else 240
pick = len(sys.argv) > 3 and sys.argv[3] == "pick"  # choose the folder instead of cancelling
os.makedirs(out, exist_ok=True)
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
design = os.path.join(demo.ROOT, "tests", "designs", "xp.xml")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
os.environ.setdefault("OS_DEMO_DISK", "")
extra = ["-fw_cfg", "name=opt/os/autorun.sh,file=" + os.environ["PP_AUTORUN"]] if os.environ.get("PP_AUTORUN") else []
if os.environ.get("PP_HEAPCHECK"):  # the kernel heap reports writes to freed memory
    extra += ["-fw_cfg", "name=opt/os/heapcheck,string=1"]
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"] + extra)
result = {}
try:
    demo.wait_for(r"shell: screen desktop", 300)
    time.sleep(5)
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    m = len(demo.log_text())
    q.type("prism")
    try:
        hit = demo.wait_for(r"shell: app window \d+: [^\n]*", wait, since=m)
        result["window"] = hit.group(0)
        time.sleep(20)  # let it finish drawing
        q.screenshot_to(os.path.join(out, "1-prism.png"))
        m2 = len(demo.log_text())
        if pick:
            q.click(199, 480)  # Prism's own "Open folder" button (focus can miss a key)
        else:
            q.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"}, {"type": "qcode", "data": "o"}])
        try:
            hit = demo.wait_for(r"portal: chooser [^\n]*", 60, since=m2)
            result["chooser"] = hit.group(0)
            time.sleep(2)
            q.screenshot_to(os.path.join(out, "2-chooser.png"))
            q.key("ret" if pick else "esc")  # pick: the folder it opens on (home)
            hit = demo.wait_for(r"portal: chooser answered [^\n]*", 20, since=m2)
            result["answered"] = hit.group(0)
            if pick:
                time.sleep(25)
                q.screenshot_to(os.path.join(out, "3-opened.png"))
                q.click(538, 231)  # the project page's "Terminal" button
                time.sleep(12)
                q.screenshot_to(os.path.join(out, "4-terminal.png"))
                q.click(700, 720)  # into the terminal, then a command
                time.sleep(1)
                q.type("echo PTY-WORKS-$((6*7))")
                time.sleep(8)
                q.screenshot_to(os.path.join(out, "5-typed.png"))
                soak = int(os.environ.get("PP_SOAK", "0"))  # seconds of file-heavy work, as a long session does
                if soak:
                    q.type("while :; do find /root /usr/share /var/lib/os-data -type f >/dev/null 2>&1; ls -laR /root >/dev/null 2>&1; date +SOAK-%s; done")
                    t0 = time.time()
                    while time.time() - t0 < soak and "PANIC" not in demo.log_text():
                        time.sleep(15)
                    q.screenshot_to(os.path.join(out, "6-soaked.png"))
        except TimeoutError:
            q.screenshot_to(os.path.join(out, "2-no-chooser.png"))
    except TimeoutError:
        q.screenshot_to(os.path.join(out, "1-no-window.png"))
    log = demo.log_text()[m:]
    keep = [l for l in log.splitlines() if "rect " not in l and l.strip() and (
        re.search(r"term:|forkpty|register|workspace|launch|spawn|not implemented|fault|killed|signal|error|Error|fail|FATAL|cannot|No such|app window|portal|flatpak|bwrap|zypak|sandbox", l))]
    print("\n".join(l[:240] for l in keep[:80]))
finally:
    q.quit()
for k in ("window", "chooser", "answered"):
    print(k.upper() + ":", result.get(k, "NO"))
sys.exit(0 if len(result) == 3 else 1)

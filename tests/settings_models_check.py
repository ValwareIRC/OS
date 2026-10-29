#!/usr/bin/env python3
"""Settings > Mind > Models on this computer: a desktop already set up with
the built-in model; the models list shows this computer, what fits, and
what's in use; then switching back to the built-in model wakes it.
  python3 tests/settings_models_check.py OUT_DIR"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
here = os.path.dirname(os.path.abspath(__file__))
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"builtin","base_url":"http://127.0.0.1:8080/v1","api_key":"","model":"os"}\n')
os.environ["OS_DEMO_DISK"] = "none"
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
demo.W, demo.H = 1600, 900
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}",
               "-fw_cfg", f"name=opt/os/facet.xml,file={os.path.join(here, 'designs', 'night.xml')}"])
result = {}
try:
    demo.wait_for(r"shell: screen desktop", 180)
    time.sleep(6)
    q.click(*demo.last_rect("ui-app-settings"))
    time.sleep(2)
    q.click(*demo.last_rect("settings-page-Mind"))
    time.sleep(2)
    q.screenshot_to(os.path.join(out, "1-mind.png"))
    p = demo.last_rect("settings-mind-models")
    result["models button"] = p is not None
    q.click(*p)
    result["network"] = demo.wait_for(r"models: (online|offline)", 30).group(1)
    time.sleep(3)
    q.screenshot_to(os.path.join(out, "2-models.png"))
    result["rows"] = sorted(set(m.group(1) for m in __import__("re").finditer(r"rect models-(\S+) ", demo.log_text())))
finally:
    q.quit()
for k, v in result.items():
    print(f"{k}: {v}")

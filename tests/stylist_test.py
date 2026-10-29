#!/usr/bin/env python3
"""The stylist's loop, end to end, against a stand-in model.

Boots the Luna theme, asks for a new look from its start menu, and answers
the design requests itself (an OpenAI-style server on the build machine,
reached by the VM at 10.0.2.2): first a design with a known flaw (a search
field that grows to fill a column), then, when shown pictures and the lint,
a corrected one. Checks that the VM sent two pictures and the lint line,
kept the corrected document, and shows progress while it works.

  scripts/vm-slot.sh python3 tests/stylist_test.py [OUT_DIR]

Pictures the VM sent are saved as OUT_DIR/sent-N.jpg.
"""
import base64, http.server, json, os, re, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

PORT = int(os.environ.get("STYLIST_MOCK_PORT", "4580"))
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(demo.ROOT, "build", "stylist-test"))
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")

flawed = open(os.path.join(demo.ROOT, "tests", "designs", "glass.xml")).read()
fixed = flawed.replace("</style>", "#launch search { grow:0; width:420; }\n</style>").replace(
    "<facet>", "<facet>\n<!-- corrected by the art director -->", 1)
fixed = re.sub(r'tagline="[^"]*"', 'tagline="fixed-by-stylist"', fixed, count=1)
if 'tagline="fixed-by-stylist"' not in fixed:
    fixed = fixed.replace("<meta ", '<meta tagline="fixed-by-stylist" ', 1)

seen = {"design": 0, "critique": 0, "images": 0, "lint_line": False, "pictures": []}


class Mock(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.send({"object": "list", "data": [{"id": "mock-vl", "object": "model"}]})

    def do_POST(self):
        q = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        msgs = q.get("messages", [])
        system = next((m["content"] for m in msgs if m["role"] == "system"), "")
        talk = [m for m in msgs if m["role"] != "system"]
        answer = "Hello."
        if "companion living in their personal operating system" in system:
            said = talk[-1]["content"] if talk else ""
            if isinstance(said, str) and "new look" in said:
                answer = "Let's try something new. [[redesign:a fresh look with a calmer launcher]]"
        if "FACET MARKUP" in system:
            last = talk[-1]["content"]
            if isinstance(last, list):  # pictures and words: the critique round
                seen["critique"] += 1
                for part in last:
                    if part.get("type") == "image_url":
                        seen["images"] += 1
                        url = part["image_url"]["url"]
                        data = base64.b64decode(url.split(",", 1)[1])
                        path = os.path.join(out, f"sent-{seen['images']}.jpg")
                        open(path, "wb").write(data)
                        seen["pictures"].append(path)
                    elif part.get("type") == "text":
                        seen["lint_line"] |= "grows to" in part["text"] and "grow:0" in part["text"]
                        open(os.path.join(out, "critique-prompt.txt"), "w").write(part["text"])
                answer = fixed
            elif len(talk) == 1:
                seen["design"] += 1
                time.sleep(8)  # long enough to photograph the designing screen
                answer = flawed
            else:  # a text-only repair round
                seen["critique"] += 1
                answer = fixed
            time.sleep(4)
        self.send({"id": "mock", "object": "chat.completion", "model": "mock-vl",
                   "choices": [{"index": 0, "message": {"role": "assistant", "content": answer},
                                "finish_reason": "stop"}]})


server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Mock)
threading.Thread(target=server.serve_forever, daemon=True).start()

mind = os.path.join(out, "mind.json")
open(mind, "w").write(json.dumps({"provider": "ollama", "base_url": f"http://10.0.2.2:{PORT}/v1",
                                  "api_key": "", "model": "mock-vl"}))
theme = os.path.join(demo.ROOT, "userland", "rootfs", "usr", "share", "os", "themes", "luna.xml")
os.environ.setdefault("OS_DEMO_DISK", "none")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={theme}"])
ok = False
try:
    demo.wait_for(r"shell: screen desktop", 150)
    time.sleep(5)
    q.click(*demo.last_rect("ui-start"))
    time.sleep(2)
    pos = demo.last_rect("ui-app-redesign")
    if not pos:
        raise SystemExit("no New Look item in the start menu")
    q.click(*pos)
    demo.wait_for(r"preview: opened", 30)  # the redesign item opens the preview, and asks
    time.sleep(1)
    q.click(*demo.last_rect("preview-input"))
    q.type("a new look please")
    demo.wait_for(r"preview: drafting", 60)
    since = len(demo.log_text())
    time.sleep(5)
    q.screenshot_to(os.path.join(out, "1-designing.png"))
    demo.wait_for(r"stylist: first draft", 120, since)
    time.sleep(1)
    q.screenshot_to(os.path.join(out, "2-looking.png"))
    demo.wait_for(r"shell: design (from the AI|from their)", 300, since)
    demo.wait_for(r"preview: draft ready", 60, since)
    time.sleep(2)
    q.click(*demo.last_rect("preview-apply"))
    demo.wait_for(r"design applied from the preview", 30, since)
    time.sleep(4)
    q.screenshot_to(os.path.join(out, "3-result.png"))
    log = demo.log_text()
    last = log.rsplit("design-facet", 1)[-1].split("\n", 1)[0] if "design-facet" in log else ""
    kept = "fixed-by-stylist" in last
    print("design requests:", seen["design"], "critique rounds:", seen["critique"], "pictures:", seen["images"],
          "lint line sent:", seen["lint_line"], "corrected kept:", kept)
    for line in re.findall(r"shell: stylist[^\n]*", log):
        print(" ", line)
    ok = seen["design"] >= 1 and seen["critique"] >= 1 and seen["images"] >= 2 and seen["lint_line"] and kept
finally:
    q.quit()
    server.shutdown()
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)

#!/usr/bin/env python3
"""The preview, end to end, against a stand-in model that streams.

Boots the Luna theme and opens the preview from the start menu's redesign
item. Asks for a new look in the preview's own input; the stand-in companion
answers with a [[redesign:]] tag, and the stand-in designer streams a whole
document (server-sent events, chunked) slowly enough to photograph it being
built. Then points at something in the picture, asks for it to be warmer (a
[[ui:style]] edit that must land in the preview, not the desktop), compares
with Before, and applies. Checks the desktop never went full screen while
designing, edits stayed in the preview until Apply, and the new design is
the desktop afterwards.

  scripts/vm-slot.sh python3 tests/preview_window_test.py [OUT_DIR]
"""
import http.server, json, os, re, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

PORT = int(os.environ.get("PREVIEW_MOCK_PORT", "4581"))
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(demo.ROOT, "build", "preview-test"))
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")

night = open(os.path.join(demo.ROOT, "tests", "designs", "night.xml")).read()
night = re.sub(r'tagline="[^"]*"', 'tagline="streamed-in-preview"', night, count=1)
style = re.search(r"<style>.*?</style>\n?", night, re.S).group(0)
night = night.replace(style, "").replace("</facet>", style + "</facet>")  # style last, as the prompt asks
seen = {"design": 0, "streamed": 0, "refine": 0, "companion": []}


class Mock(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, obj, status=200):
        body = json.dumps(obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def chunk(self, data):
        b = data.encode()
        self.wfile.write(b"%x\r\n%s\r\n" % (len(b), b))
        self.wfile.flush()

    def stream(self, text, pieces, pause):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()
        step = max(1, len(text) // pieces)
        for i in range(0, len(text), step):
            ev = {"choices": [{"index": 0, "delta": {"content": text[i:i + step]}, "finish_reason": None}]}
            self.chunk("data: " + json.dumps(ev) + "\n\n")
            time.sleep(pause)
        self.chunk("data: " + json.dumps({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}) + "\n\n")
        self.chunk("data: [DONE]\n\n")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def do_GET(self):
        self.send({"object": "list", "data": [{"id": "mock", "object": "model"}]})

    def do_POST(self):
        q = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        msgs = q.get("messages", [])
        system = next((m["content"] for m in msgs if m["role"] == "system"), "")
        talk = [m for m in msgs if m["role"] != "system"]
        last = talk[-1]["content"] if talk else ""
        if isinstance(last, list):
            last = " ".join(p.get("text", "") for p in last if p.get("type") == "text")
        if "FACET MARKUP" in system:
            if len(talk) == 1:
                seen["design"] += 1
                if q.get("stream"):
                    seen["streamed"] += 1
                    return self.stream(night, 40, 0.35)  # about 14 seconds: long enough to watch
                answer = night
            else:
                seen["refine"] += 1
                answer = night
            return self.send({"choices": [{"index": 0, "message": {"role": "assistant", "content": answer},
                                           "finish_reason": "stop"}]})
        # the companion
        seen["companion"].append(last[:120])
        if "starry" in last:
            answer = "Let's try it. [[redesign:a deep night sky with stars, silver type, one quiet dock]]"
        elif "redesign is ready" in last:
            answer = "Here it is: a night sky and quieter type. Apply it if it feels right, or keep adjusting."
        elif "warmer" in last:
            m = re.search(r"selector: ([^)]+)\)", last)
            target = m.group(1) if m else "dock"
            answer = "Warmer, then. [[ui:style %s { background: rgba(255,140,80,0.55); }]]" % target
        else:
            answer = "Hello."
        self.send({"choices": [{"index": 0, "message": {"role": "assistant", "content": answer},
                                "finish_reason": "stop"}]})


server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Mock)
threading.Thread(target=server.serve_forever, daemon=True).start()

mind = os.path.join(out, "mind.json")
open(mind, "w").write(json.dumps({"provider": "ollama", "base_url": f"http://10.0.2.2:{PORT}/v1",
                                  "api_key": "", "model": "mock"}))
theme = os.path.join(demo.ROOT, "userland", "rootfs", "usr", "share", "os", "themes", "luna.xml")
os.environ.setdefault("OS_DEMO_DISK", "none")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={theme}"])
results = []


def check(ok, what):
    results.append((ok, what))
    print(("ok   " if ok else "FAIL ") + what, flush=True)


def wait(pattern, timeout, since=0):
    try:
        return demo.wait_for(pattern, timeout, since)
    except TimeoutError:
        return None


try:
    demo.wait_for(r"shell: screen desktop", 150)
    time.sleep(6)
    q.click(*demo.last_rect("ui-start"))
    time.sleep(2)
    pos = demo.last_rect("ui-app-redesign")
    if not pos:
        raise SystemExit("no redesign item in the start menu")
    mark = len(demo.log_text())
    q.click(*pos)
    check(wait(r"preview: opened on a copy of the desktop", 15, mark) is not None, "the redesign item opens the preview")
    time.sleep(2)
    q.screenshot_to(os.path.join(out, "1-preview-opened.png"))

    # ask for a new look, in the preview itself
    q.click(*demo.last_rect("preview-input"))
    time.sleep(0.5)
    mark = len(demo.log_text())
    q.type("make it feel like a starry night")
    q.key("ret")
    check(wait(r"preview: drafting", 40, mark) is not None, "a redesign starts in the preview")
    time.sleep(5)
    q.screenshot_to(os.path.join(out, "2-being-built.png"))
    time.sleep(4)
    q.screenshot_to(os.path.join(out, "3-being-built.png"))
    check(seen["streamed"] >= 1, "the design was asked for as a stream")
    check(not re.search(r"shell: screen designing", demo.log_text()[mark:]), "the desktop stays; no full-screen designing")
    check(wait(r"preview: draft ready", 240, mark) is not None, "the draft lands in the preview")
    time.sleep(3)
    q.screenshot_to(os.path.join(out, "4-draft-ready.png"))
    check("design applied from the preview" not in demo.log_text()[mark:], "nothing is applied before Apply")

    # point at something, and ask for it to be warmer
    log = demo.log_text()
    ix, iy, iw, ih = map(int, re.findall(r"shell: rect preview-image (-?\d+) (-?\d+) (\d+) (\d+)", log)[-1])
    # the night design's dock: bottom middle, its centre about 51 px above the screen's bottom edge
    mark = len(demo.log_text())
    for fy in (51, 40, 62):
        q.click(ix + iw // 2, iy + ih - fy * ih // demo.H)
        time.sleep(1)
        if wait(r"preview: pointing at #", 2, mark):
            break
    pointed = wait(r"preview: pointing at #([^:]+): ([^\n]*)", 2, mark)
    check(pointed is not None, "clicking the picture points at something (%s)" % (pointed.group(0) if pointed else "none"))
    q.screenshot_to(os.path.join(out, "5-pointing.png"))
    q.click(*demo.last_rect("preview-input"))
    mark = len(demo.log_text())
    q.type("make this warmer")
    q.key("ret")
    edit = wait(r"ui command \"style [^\"]*\" \(preview\)", 40, mark)
    check(edit is not None, "the companion's edit goes to the preview")
    time.sleep(2)
    q.screenshot_to(os.path.join(out, "6-warmer.png"))
    check("design applied from the preview" not in demo.log_text()[mark:], "still not applied")

    # compare with how it was
    before = demo.last_rect("preview-before")
    if before:
        q.click(*before)
        time.sleep(1.5)
        q.screenshot_to(os.path.join(out, "7-before.png"))
        q.click(*before)
        time.sleep(1)

    # make it real
    mark = len(demo.log_text())
    q.click(*demo.last_rect("preview-apply"))
    time.sleep(0.25)
    q.screenshot_to(os.path.join(out, "8-morphing.png"))
    check(wait(r"design applied from the preview", 10, mark) is not None, "Apply makes it the desktop")
    time.sleep(2.5)
    q.screenshot_to(os.path.join(out, "9-applied.png"))
    check(wait(r"shell: rect ui-lights ", 10, mark) is not None, "the desktop is the night design now")
    print("companion heard:", seen["companion"])
finally:
    q.quit()
    server.shutdown()
ok = all(r[0] for r in results if r[1])
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)

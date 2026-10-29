#!/usr/bin/env python3
"""The end of onboarding: the first design arrives in the preview, built as
it is written, and only becomes theirs when they say so.

A stand-in model plays a short first conversation (six questions, then
[[done]]), streams a design with its <style> last (as the prompt asks), and
as the companion makes a change asked for in the preview. Checks the design
is shown in the preview rather than applied, a change lands in the preview,
and "Make it mine" brings the desktop.

  scripts/vm-slot.sh python3 tests/onboard_preview_test.py [OUT_DIR]
"""
import http.server, json, os, re, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

PORT = int(os.environ.get("PREVIEW_MOCK_PORT", "4582"))
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(demo.ROOT, "build", "onboard-preview-test"))
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")

design = open(os.path.join(demo.ROOT, "tests", "designs", "night.xml")).read()
style = re.search(r"<style>.*?</style>\n?", design, re.S).group(0)
design = design.replace(style, "").replace("</facet>", style + "</facet>")  # style last, as asked
design = re.sub(r'tagline="[^"]*"', 'tagline="made-in-onboarding"', design, count=1)
QUESTIONS = ["Hello. What should I call you?", "What do you spend your days doing?", "When do you feel most at ease?",
             "What wears you out?", "Where do you feel at home?", "How should I be with you: quiet, or chatty?"]
seen = {"onboard": 0, "streamed": 0, "companion": []}


class Mock(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send_text(self, text):
        body = json.dumps({"choices": [{"index": 0, "message": {"role": "assistant", "content": text},
                                        "finish_reason": "stop"}]}).encode()
        self.send_response(200)
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
            self.chunk("data: " + json.dumps({"choices": [{"index": 0, "delta": {"content": text[i:i + step]}}]}) + "\n\n")
            time.sleep(pause)
        self.chunk("data: " + json.dumps({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}) + "\n\n")
        self.chunk("data: [DONE]\n\n")
        self.wfile.write(b"0\r\n\r\n")
        self.wfile.flush()

    def do_GET(self):
        self.send_text("")

    def do_POST(self):
        q = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        msgs = q.get("messages", [])
        system = next((m["content"] for m in msgs if m["role"] == "system"), "")
        talk = [m for m in msgs if m["role"] != "system"]
        last = talk[-1]["content"] if talk else ""
        if isinstance(last, list):
            last = " ".join(p.get("text", "") for p in last if p.get("type") == "text")
        if "FACET MARKUP" in system:
            if len(talk) == 1 and q.get("stream"):
                seen["streamed"] += 1
                return self.stream(design, 40, 0.3)
            return self.send_text(design)
        if "companion living in their personal operating system" in system:
            seen["companion"].append(last[:100])
            if "warmer" in last:
                m = re.search(r"selector: ([^)]+)\)", last)
                return self.send_text("Warmer. [[ui:style %s { background: rgba(255,150,90,0.6); }]]" %
                                      (m.group(1) if m else "dock"))
            return self.send_text("Here is your space. The dock keeps your work close; tell me anything to change.")
        if "field: value" in system:  # the portrait reading
            return self.send_text("work: films\nrhythm: late afternoons\ntaste: night sky\nvoice: quiet\napps: files")
        # the first conversation
        users = sum(1 for m in talk if m["role"] == "user")
        seen["onboard"] += 1
        if users >= 7:
            return self.send_text("Thank you, Sam. I know enough to make your space now. [[done]]")
        return self.send_text(QUESTIONS[min(users, len(QUESTIONS) - 1)])


server = http.server.ThreadingHTTPServer(("0.0.0.0", PORT), Mock)
threading.Thread(target=server.serve_forever, daemon=True).start()
mind = os.path.join(out, "mind.json")
open(mind, "w").write(json.dumps({"provider": "ollama", "base_url": f"http://10.0.2.2:{PORT}/v1",
                                  "api_key": "", "model": "mock"}))
os.environ.setdefault("OS_DEMO_DISK", "none")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}"])
results = []


def check(ok, what):
    results.append((ok, what))
    print(("ok   " if ok else "FAIL ") + what, flush=True)


def wait(pattern, timeout, since=0):
    try:
        return demo.wait_for(pattern, timeout, since)
    except TimeoutError:
        return None


answers = ["Sam", "I edit sound for films, mostly alone", "Late afternoons by the sea", "Too many notes from directors",
           "A small flat by the water", "Quiet, please", "That is all, I think"]
try:
    demo.wait_for(r"shell: screen onboard", 150)
    mark = 0
    for a in answers:
        if wait(r"shell: screen designing", 1, mark):
            break
        wait(r"shell: (ai|pick shown) ", 60, mark)
        time.sleep(1.5)
        mark = len(demo.log_text())
        q.type(a)
    m = wait(r"shell: screen designing", 120)
    check(m is not None, "the conversation hands over to the designer")
    at = m.start() if m else 0
    check(wait(r"preview: drafting", 20, at) is not None, "the design is drafted in the preview")
    time.sleep(6)
    q.screenshot_to(os.path.join(out, "1-pieces-arrive.png"))
    time.sleep(4)
    q.screenshot_to(os.path.join(out, "2-styling.png"))
    check(seen["streamed"] >= 1, "the design was asked for as a stream")
    check(wait(r"preview: draft ready", 240, at) is not None, "the draft lands in the preview")
    time.sleep(4)
    q.screenshot_to(os.path.join(out, "3-here-it-is.png"))
    check("shell: screen desktop" not in demo.log_text()[at:], "it is not the desktop until they say so")

    ix, iy, iw, ih = map(int, re.findall(r"shell: rect preview-image (-?\d+) (-?\d+) (\d+) (\d+)", demo.log_text())[-1])
    mark = len(demo.log_text())
    for fy in (51, 40, 62):
        q.click(ix + iw // 2, iy + ih - fy * ih // demo.H)
        time.sleep(1)
        if wait(r"preview: pointing at #", 2, mark):
            break
    check(wait(r"preview: pointing at #", 2, mark) is not None, "they can point at something")
    mark = len(demo.log_text())
    q.click(*demo.last_rect("preview-input"))
    q.type("make this warmer")
    check(wait(r"ui command \"style [^\"]*\" \(preview\)", 60, mark) is not None, "the change lands in the preview")
    time.sleep(2)
    q.screenshot_to(os.path.join(out, "4-warmer.png"))

    mark = len(demo.log_text())
    q.click(*demo.last_rect("preview-apply"))
    check(wait(r"shell: screen desktop", 15, mark) is not None, "\"Make it mine\" brings the desktop")
    time.sleep(3)
    q.screenshot_to(os.path.join(out, "5-theirs.png"))
    print("companion heard:", seen["companion"])
finally:
    q.quit()
    server.shutdown()
ok = all(r[0] for r in results)
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)

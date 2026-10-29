#!/usr/bin/env python3
"""A stand-in AI for tests: an OpenAI-style server that answers each chat
request with the next reply from a script (then "Okay."), and writes every
request's last messages to a log, so a test can see what the OS told it.
  python3 tests/fake_ai.py PORT REPLIES.json LOG"""
import json, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

port, script, log = int(sys.argv[1]), sys.argv[2], sys.argv[3]
replies = json.load(open(script))
lock = threading.Lock()

class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        body = json.dumps({"data": [{"id": "fake"}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        msgs = req.get("messages", [])
        with lock:
            reply = replies.pop(0) if replies else "Okay."
            with open(log, "a") as f:
                tail = [m for m in msgs[-4:] if m.get("role") != "system"]
                f.write(json.dumps({"tail": tail, "reply": reply[:200]}) + "\n")
        if req.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            chunk = {"choices": [{"delta": {"content": reply}, "index": 0}]}
            self.wfile.write(b"data: " + json.dumps(chunk).encode() + b"\n\n")
            self.wfile.write(b"data: [DONE]\n\n")
            return
        body = json.dumps({"choices": [{"message": {"role": "assistant", "content": reply}, "finish_reason": "stop",
                                        "index": 0}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()

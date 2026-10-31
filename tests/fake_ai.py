#!/usr/bin/env python3
"""A stand-in AI for tests: an OpenAI-style server that answers each chat
request with the next reply from a script (then "Okay."), and writes every
request's last messages to a log, so a test can see what the OS told it.

An agent's request (one with tools) whose message asks to read a file gets a
real tool call back: the tool that reads files, with that file's name in its
first string parameter; once the tool's result comes back, the answer quotes
its first line. So one whole agent turn can be tested without a model.
  python3 tests/fake_ai.py PORT REPLIES.json LOG"""
import json, re, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

port, script, log = int(sys.argv[1]), sys.argv[2], sys.argv[3]
replies = json.load(open(script))
lock = threading.Lock()


def text_of(content):
    if isinstance(content, str):
        return content
    return "".join(p.get("text", "") for p in content or [] if isinstance(p, dict))


def agent_step(req):
    """(reply text, tool call or None) for an agent's request, or None if it isn't one."""
    tools = req.get("tools") or []
    msgs = req.get("messages", [])
    if not tools or not msgs:
        return None
    last = msgs[-1]
    if last.get("role") == "tool":  # the tool ran: say what it found
        first = next((l for l in text_of(last.get("content")).splitlines() if l.strip()), "")
        first = re.sub(r"^\s*\d+[\s|:\t]+", "", first)  # a line number in front, if the tool adds one
        return f"The first line is: {first.strip()}", None
    user = text_of(last.get("content")) if last.get("role") == "user" else ""
    m = re.search(r"([\w./-]+\.(?:md|txt|json|c|py))", user)
    if not m:
        return None
    reader = None
    for t in tools:
        f = t.get("function", {})
        name = f.get("name", "")
        if re.search(r"read", name, re.I) and not re.search(r"many|multi|dir|list", name, re.I):
            reader = f
            break
    if not reader:
        return None
    params = reader.get("parameters", {}) or {}
    props = params.get("properties", {}) or {}
    first = next((p for p in params.get("required", []) if props.get(p, {}).get("type") == "string"), None)
    first = first or next((p for p, s in props.items() if s.get("type") == "string"), "path")
    call = {"id": "call_fake_1", "type": "function",
            "function": {"name": reader["name"], "arguments": json.dumps({first: m.group(1)})}}
    return "", call


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
        step = agent_step(req)
        with lock:
            if step:
                reply, call = step
            else:
                reply, call = (replies.pop(0) if replies else "Okay."), None
            with open(log, "a") as f:
                tail = [m for m in msgs[-4:] if m.get("role") != "system"]
                f.write(json.dumps({"tail": tail, "reply": reply[:200], "model": req.get("model"),
                                    "tools": [t.get("function", {}).get("name") for t in req.get("tools", [])],
                                    "tool_call": call}) + "\n")
        finish = "tool_calls" if call else "stop"
        if req.get("stream"):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            chunks = []
            if reply:
                chunks.append({"choices": [{"delta": {"role": "assistant", "content": reply}, "index": 0}]})
            if call:
                chunks.append({"choices": [{"delta": {"tool_calls": [dict(call, index=0)]}, "index": 0}]})
            chunks.append({"choices": [{"delta": {}, "index": 0, "finish_reason": finish}]})
            if (req.get("stream_options") or {}).get("include_usage"):
                chunks.append({"choices": [], "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15}})
            for c in chunks:
                self.wfile.write(b"data: " + json.dumps(c).encode() + b"\n\n")
            self.wfile.write(b"data: [DONE]\n\n")
            return
        message = {"role": "assistant", "content": reply or None}
        if call:
            message["tool_calls"] = [call]
        body = json.dumps({"choices": [{"message": message, "finish_reason": finish, "index": 0}]}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()

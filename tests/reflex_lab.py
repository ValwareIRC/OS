"""Trying ways for a tiny local model to judge events (for reflex.c). Same cases as reflex_try.py.
  python tests/reflex_lab.py VARIANT    (fewshot | yesno)"""
import json, math, sys, time, urllib.request
sys.stdout.reconfigure(encoding="utf-8")
from reflex_try import CASES

URL = "http://127.0.0.1:8092/v1/chat/completions"


def chat(messages, **kw):
    body = {"messages": messages, "temperature": 0, "chat_template_kwargs": {"enable_thinking": False}}
    body.update(kw)
    return json.loads(urllib.request.urlopen(urllib.request.Request(URL, json.dumps(body).encode(),
                                                                    {"Content-Type": "application/json"}), timeout=60).read())


def first_token_probs(r, labels):
    p = {k: 0.0 for k in labels}
    ch = r["choices"][0]
    for c in (ch.get("logprobs") or {}).get("content", [{}])[0].get("top_logprobs", []):
        tok = c["token"].strip().lower()
        for k in labels:
            if tok == k.lower():
                p[k] += math.exp(c["logprob"])
    s = sum(p.values()) or 1
    return {k: v / s for k, v in p.items()}, (ch["message"].get("content") or "")


SYS = ("You judge events on a personal computer for its built-in assistant. You answer each question about the "
       "event with yes or no.")

EXAMPLES = [
    ("The person is at the computer.", "they closed \"Remix\" (the person did this)", "no", "no", "yes"),
    ("The person is at the computer.", "the clock passed 15:00 (it happened by itself)", "no", "no", "no"),
    ("The person is away.", "a backup finished after 2 hours (it happened by itself)", "no", "no", "yes"),
    ("The person is at the computer.", "a calendar reminder: dentist in 15 minutes (it happened by itself)", "yes", "no", "yes"),
    ("The person is at the computer.", "the battery is at 3% (it happened by itself)", "yes", "yes", "yes"),
    ("The person is typing.", "Wi-Fi reconnected after 20 seconds (it happened by itself)", "no", "no", "no"),
]
Q_NOW = "Should the assistant interrupt the person about this right now?"
Q_HARM = "Is something about to be lost or damaged unless someone acts soon?"
Q_KEEP = "Is it worth remembering for later?"


def yesno(sit, ev):
    out = {}
    for name, q, idx in (("now", Q_NOW, 0), ("harm", Q_HARM, 1), ("keep", Q_KEEP, 2)):
        msgs = [{"role": "system", "content": SYS}]
        for es, ee, *ans in EXAMPLES:
            msgs.append({"role": "user", "content": f"{es}\nEvent: {ee}\n{q}"})
            msgs.append({"role": "assistant", "content": ans[idx]})
        msgs.append({"role": "user", "content": f"{sit}\nEvent: {ev}\n{q}"})
        p, _ = first_token_probs(chat(msgs, max_tokens=1, logprobs=True, top_logprobs=20), ["yes", "no"])
        out[name] = p["yes"]
    if out["harm"] > 0.5 and out["now"] > 0.5:
        return "D", out
    if out["now"] > 0.6:
        return "C", out
    if out["keep"] > 0.5:
        return "B", out
    return "A", out


def fewshot(sit, ev):
    msgs = [{"role": "system", "content": SYS.replace("with yes or no", "with one letter")}]
    for es, ee, now, harm, keep in EXAMPLES:
        letter = "D" if harm == "yes" else "C" if now == "yes" else "B" if keep == "yes" else "A"
        msgs.append({"role": "user", "content": f"{es}\nEvent: {ee}\nA ignore, B remember, C interrupt now, D urgent?"})
        msgs.append({"role": "assistant", "content": letter})
    msgs.append({"role": "user", "content": f"{sit}\nEvent: {ev}\nA ignore, B remember, C interrupt now, D urgent?"})
    p, _ = first_token_probs(chat(msgs, max_tokens=1, logprobs=True, top_logprobs=20), list("ABCD"))
    best = max(p, key=p.get)
    return best, p


variant = sys.argv[1] if len(sys.argv) > 1 else "yesno"
fn = yesno if variant == "yesno" else fewshot
good = 0
for sit, ev, ok in CASES:
    t = time.time()
    letter, info = fn(sit, ev)
    ms = (time.time() - t) * 1000
    hit = letter in ok
    good += hit
    print(f"{'ok ' if hit else 'BAD'} {letter} want {ok:3} {ms:5.0f}ms  {ev[:60]:60}  {' '.join(f'{k}{v:.2f}' for k, v in info.items())}")
print(f"{variant}: {good}/{len(CASES)}")

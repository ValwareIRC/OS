"""The reflex design reflex.c uses: one yes/no question, read as a probability ("interrupt now?"), and only when
that is yes, a second ("harm soon?"). Same cases as reflex_try.py.   python tests/reflex_now.py"""
import json, math, sys, time, urllib.request
sys.stdout.reconfigure(encoding="utf-8")
from reflex_try import CASES

SYSTEM = "You judge events on a personal computer for its built-in assistant. You answer each question with yes or no."
Q_NOW = "Should the assistant interrupt the person about this right now?"
Q_HARM = "Will something be lost or damaged soon unless someone acts?"
# (situation, event, interrupt now?, harm soon?)
EXAMPLES = [
    ("The person is at the computer.", "they closed \"Remix\" (the person did this)", "no", "no"),
    ("The person is at the computer.", "the clock passed 15:00 (it happened by itself)", "no", "no"),
    ("The person is away.", "a backup finished after 2 hours (it happened by itself)", "no", "no"),
    ("The person is at the computer.", "a calendar reminder: dentist in 15 minutes (it happened by itself)", "yes", "no"),
    ("The person is at the computer.", "a message from Sam: \"are you still coming tonight?\" (it happened by itself)", "yes", "no"),
    ("The person is at the computer.", "the battery is at 3% (it happened by itself)", "yes", "yes"),
    ("The person is typing.", "Wi-Fi reconnected after 20 seconds (it happened by itself)", "no", "no"),
    ("The person is at the computer.", "memory is at 85% (it happened by itself)", "no", "no"),
    ("The person is at the computer.", "an app finished updating (it happened by itself)", "no", "no"),
    ("The person is at the computer.", "the GPU is at 96°C (it happened by itself)", "yes", "yes"),
]


def p_yes(sit, ev, q, idx):
    msgs = [{"role": "system", "content": SYSTEM}]
    for es, ee, *ans in EXAMPLES:
        msgs += [{"role": "user", "content": f"{es}\nEvent: {ee}\n{q}"}, {"role": "assistant", "content": ans[idx]}]
    msgs.append({"role": "user", "content": f"{sit}\nEvent: {ev}\n{q}"})
    body = {"messages": msgs, "temperature": 0, "chat_template_kwargs": {"enable_thinking": False},
            "max_tokens": 1, "logprobs": True, "top_logprobs": 20}
    r = json.loads(urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:8092/v1/chat/completions",
                                                                 json.dumps(body).encode(), {"Content-Type": "application/json"}), timeout=60).read())
    y = n = 0.0
    for c in (r["choices"][0].get("logprobs") or {}).get("content", [{}])[0].get("top_logprobs", []):
        t = c["token"].strip().lower()
        if t == "yes": y += math.exp(c["logprob"])
        if t == "no": n += math.exp(c["logprob"])
    return y / (y + n) if y + n else 0.5


good = 0
for sit, ev, ok in CASES:
    t = time.time()
    now = p_yes(sit, ev, Q_NOW, 0)
    harm = p_yes(sit, ev, Q_HARM, 1) if now >= 0.8 else 0
    d = "D" if harm >= 0.8 else "C" if now >= 0.8 else "B"
    want_wake = any(x in ok for x in "CD") and not any(x in ok for x in "AB")
    no_wake = not any(x in ok for x in "CD")
    hit = (d in "CD" and not no_wake) if d in "CD" else not want_wake
    good += hit
    print(f"{'ok ' if hit else 'BAD'} {d} now {now:.2f} harm {harm:.2f} {(time.time()-t)*1000:5.0f}ms  {ev[:64]}  (want {ok})")
print(f"wake decisions right: {good}/{len(CASES)}")

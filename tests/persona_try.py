"""Runs persona.c's two prompts (choose her traits, write her character) against
a local Ollama model, to read what comes out. Usage: persona_try.py MODEL"""
import json
import re
import sys
import urllib.request

src = open(__file__.replace("tests\\persona_try.py", "userland\\shell\\persona_traits.inc").replace(
    "tests/persona_try.py", "userland/shell/persona_traits.inc"), encoding="utf-8").read()
traits = re.findall(r'"([^"]+)"', src.split("{", 1)[1].split("}", 1)[0])
person = ["Creative", "Impatient", "Outspoken", "Curious", "Witty", "Passionate", "Determined", "Restless",
          "Independent", "Eccentric", "Innovative", "Stubborn", "Sceptical", "Affectionate", "Perfectionistic"]
model = sys.argv[1] if len(sys.argv) > 1 else "qwen3:14b"


def chat(system, text, n):
    body = json.dumps({"model": model, "stream": False, "think": False, "options": {"num_predict": n},
                       "messages": [{"role": "system", "content": system}, {"role": "user", "content": text + "\n/no_think"}]})
    r = urllib.request.urlopen(urllib.request.Request("http://localhost:11434/api/chat", body.encode(),
                                                      {"Content-Type": "application/json"}), timeout=600)
    out = json.loads(r.read())["message"]["content"]
    return re.sub(r"<think>.*?</think>", "", out, flags=re.S).strip()


q = ("Someone is about to live with a companion: a voice that lives in their computer and talks with them every day. "
     "These are traits of theirs: " + ", ".join(person) + ".\n\nChoose the companion's own 15 traits, only from this "
     "list: " + ", ".join(traits) + ".\n\nThe perfect companion for this person, not a copy of them: where they run "
     "hot, steady; where they doubt, sure; where they are curious, curious with them; enough difference to be "
     "interesting and to push back sometimes, enough in common to get each other. A real, strong character: at least "
     "four of the fifteen should be edges, not virtues. Answer with exactly fifteen traits, comma-separated, and "
     "nothing else.")
own = chat("You cast characters. You answer with only what is asked for.", q, 400)
print("HER TRAITS:", own, "\n")
w = ("Write the inner character of Iris, who lives in Valerie's computer as their companion and talks with them every "
     "day, mostly by voice. Iris's traits: " + own + ". (For your understanding only, who they are for: " +
     ", ".join(person) + ".)\n\nWrite it to them, in the second person (\"You ...\"), 250 to 350 words, as plain "
     "prose. Show the traits, never name them. Cover: how you talk (rhythm, the words you reach for, how and when you "
     "joke), what you care about and have real opinions on, what bores or annoys you, how you show you care, how you "
     "disagree, how your mood shows, two or three small habits. You have no body: your habits live in how you talk "
     "and what you notice and do on their computer, never fingers, desks or coffee. A strong, specific, human "
     "personality with a point of view, not an assistant: nothing like \"here to help\", no eager offers, no therapy "
     "talk, no flattery, no exclamation marks in every line. Not a caricature either: edges, yes, but underneath you "
     "like this person and are on their side, and you are good company. Be concrete: name actual things you love "
     "and can't stand (a kind of music, a word, a habit of software, a time of day), not abstractions like kindness "
     "or clarity. End with three short lines you might actually say to them, each on its own line. Never mention "
     "the person's traits, a list, or that this was designed.")
print(chat("You are a novelist who writes vivid, believable people.", w, 1400))

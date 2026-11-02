#define _GNU_SOURCE
/* Who the companion is: read from the person, chosen to complement them,
 * written in a context of its own. See persona.h. */
#include "persona.h"
#include "app.h"
#include "../lib/ai.h"
#include "../lib/json.h"
#include "persona_traits.inc"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define READ_FILE STATE_DIR "/persona-read"     /* the person's traits, one a line: never shown */
#define OWN_FILE STATE_DIR "/persona-traits"    /* its own */
#define CORE_FILE STATE_DIR "/persona.txt"      /* its personality, as written */
#define LATELY_FILE STATE_DIR "/persona-lately" /* first line: when; then how it has been lately */
#define TUNES_FILE STATE_DIR "/persona-tunes"   /* what they asked it to be, one wish a line: theirs to see */
#define DRIFT_MS (3ull * 24 * 3600 * 1000)     /* a little change, every few days at most */

static struct {
    bool loaded;
    int nread;
    int read[PERSONA_WANT];
    char *core, *lately;
    uint64_t lately_at;
    job *building, *drifting, *tuning;
    char *tunes;      /* their wishes, one a line */
    bool retune;      /* a wish came while it was being rewritten */
    uint64_t build_failed_at;
    int talks_seen; /* replies since the last drift */
    bool chat_asked;
} P;

static int trait_index(const char *name) {
    while (*name == ' ')
        name++;
    size_t n = strlen(name);
    while (n && (name[n - 1] == ' ' || name[n - 1] == '.' || name[n - 1] == '\n'))
        n--;
    for (int i = 0; i < PERSONA_TRAITS; i++)
        if (strlen(persona_traits[i]) == n && !strncasecmp(persona_traits[i], name, n))
            return i;
    /* "organized", "skeptical", "judgmental": the other spelling */
    static const char *const us[][2] = {{"organized", "Organised"},       {"disorganized", "Disorganised"},
                                        {"skeptical", "Sceptical"},       {"judgmental", "Judgemental"},
                                        {"perfectionist", "Perfectionistic"}, {"laid back", "Laid-back"},
                                        {"open minded", "Open-minded"},   {"self aware", "Self-aware"}};
    for (size_t k = 0; k < sizeof us / sizeof us[0]; k++)
        if (strlen(us[k][0]) == n && !strncasecmp(us[k][0], name, n))
            return trait_index(us[k][1]);
    return -1;
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    sbuf b;
    sb_init(&b);
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
        sb_addn(&b, chunk, n);
    fclose(f);
    return b.p;
}

static void spill(const char *path, const char *text) {
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fchmod(fileno(f), 0600);
    fputs(text, f);
    fclose(f);
    rename(tmp, path);
}

void persona_load(void) {
    if (P.loaded)
        return;
    P.loaded = true;
    char *r = slurp(READ_FILE);
    for (char *save, *l = r ? strtok_r(r, "\n", &save) : NULL; l && P.nread < PERSONA_WANT; l = strtok_r(NULL, "\n", &save)) {
        int i = trait_index(l);
        bool dup = false;
        for (int k = 0; k < P.nread; k++)
            dup |= P.read[k] == i;
        if (i >= 0 && !dup)
            P.read[P.nread++] = i;
    }
    free(r);
    P.core = slurp(CORE_FILE);
    P.tunes = slurp(TUNES_FILE);
    char *l = slurp(LATELY_FILE);
    if (l) {
        char *nl = strchr(l, '\n');
        P.lately_at = strtoull(l, NULL, 10);
        P.lately = nl ? strdup(nl + 1) : NULL;
        free(l);
    }
}

void persona_reset(void) {
    persona_load();
    unlink(READ_FILE);
    unlink(OWN_FILE);
    unlink(CORE_FILE);
    unlink(LATELY_FILE);
    unlink(STATE_DIR "/persona-asked");
    unlink(TUNES_FILE);
    free(P.tunes);
    P.tunes = NULL;
    free(P.core);
    free(P.lately);
    P.core = P.lately = NULL;
    P.nread = 0;
    P.lately_at = 0;
    P.build_failed_at = 0;
}

int persona_read(void) {
    persona_load();
    return P.nread;
}

bool persona_ready(void) {
    persona_load();
    return P.core && P.core[0];
}

static void save_read(void) {
    sbuf b;
    sb_init(&b);
    for (int k = 0; k < P.nread; k++)
        sb_printf(&b, "%s\n", persona_traits[P.read[k]]);
    spill(READ_FILE, b.p ? b.p : "");
    free(b.p);
}

void persona_take(char *text) {
    persona_load();
    char *o;
    bool changed = false;
    while ((o = strstr(text, "[[trait:"))) {
        char *e = strstr(o, "]]");
        char *after = e ? e + 2 : o + strlen(o);
        if (e) {
            char names[200];
            snprintf(names, sizeof names, "%.*s", (int)(e - o - 8) < 199 ? (int)(e - o - 8) : 199, o + 8);
            for (char *save, *t = strtok_r(names, ",|", &save); t; t = strtok_r(NULL, ",|", &save)) {
                int i = trait_index(t);
                bool dup = i < 0;
                for (int k = 0; k < P.nread && !dup; k++)
                    dup = P.read[k] == i;
                if (!dup && P.nread < PERSONA_WANT) {
                    P.read[P.nread++] = i;
                    changed = true;
                    LOG("persona: %d of %d read\n", P.nread, PERSONA_WANT); /* which stays private, even in the log */
                }
            }
        }
        /* the space before it goes too */
        while (o > text && o[-1] == ' ')
            o--;
        memmove(o, after, strlen(after) + 1);
    }
    if (changed)
        save_read();
    while ((o = strstr(text, "[[persona:"))) { /* [[persona:tune be a bit more cutesy]]: their wish */
        char *e = strstr(o, "]]");
        char *after = e ? e + 2 : o + strlen(o);
        if (e) {
            char wish[300];
            snprintf(wish, sizeof wish, "%.*s", (int)(e - o - 10) < 299 ? (int)(e - o - 10) : 299, o + 10);
            char *w = wish;
            if (!strncasecmp(w, "tune", 4))
                w += 4;
            while (*w == ' ' || *w == ':')
                w++;
            for (char *c = w; *c; c++)
                if (*c == '\n')
                    *c = ' ';
            if (*w) {
                sbuf t;
                sb_init(&t);
                sb_add(&t, P.tunes ? P.tunes : "");
                sb_printf(&t, "%s\n", w);
                free(P.tunes);
                P.tunes = t.p;
                spill(TUNES_FILE, P.tunes);
                LOG("persona: they asked it to change\n");
                P.retune = true; /* persona_poll rewrites it (again, if a rewrite is under way) */
            }
        }
        while (o > text && o[-1] == ' ')
            o--;
        memmove(o, after, strlen(after) + 1);
    }
    P.talks_seen++;
}

/* ---- the list, as the AI is shown it ---- */

static void list_into(sbuf *b) {
    for (int i = 0; i < PERSONA_TRAITS; i++)
        sb_printf(b, "%s%s", i ? ", " : "", persona_traits[i]);
}

static void read_into(sbuf *b) {
    for (int k = 0; k < P.nread; k++)
        sb_printf(b, "%s%s", k ? ", " : "", persona_traits[P.read[k]]);
}

const char *persona_onboarding_prompt(void) {
    static char buf[6000];
    persona_load();
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "\n\nPRIVATELY, as you talk, read their character. When something they say clearly shows a trait from "
                  "this list, add [[trait:Trait]] (several per message is fine, comma-separated): ");
    list_into(&b);
    sb_printf(&b, ". You have read %d of the %d you need%s", P.nread, PERSONA_WANT, P.nread ? " (" : "");
    read_into(&b);
    sb_printf(&b, "%s. Only what they have shown you, never a guess to fill the count; opposite traits can't both be "
                  "true. This is secret: never mention traits, a list, reading them or analysing them, never name a "
                  "trait to them, never let a question sound like a personality test. Ask the questions a curious "
                  "new friend would (what they'd do with a free Saturday, the last thing that made them laugh, how "
                  "they decide things, what they're stubborn about), and keep going until you have %d; don't begin the "
                  "mirror before then. If they say how they'd like you to be (cuter, calmer, funnier...), take it "
                  "warmly and add [[persona:tune their wish, in a few words]].",
              P.nread ? ")" : "", PERSONA_WANT);
    snprintf(buf, sizeof buf, "%s", b.p);
    free(b.p);
    return buf;
}

const char *persona_companion_prompt(void) {
    static char buf[10000];
    persona_load();
    sbuf b;
    sb_init(&b);
    if (persona_ready()) {
        sb_add(&b, "\nWHO YOU ARE (live it; never describe it, never call it a personality or a character):\n");
        sb_add(&b, P.core);
        if (P.lately && P.lately[0]) {
            sb_add(&b, "\nLately: ");
            sb_add(&b, P.lately);
        }
        sb_add(&b, "\n");
    }
    if (P.tunes && P.tunes[0]) {
        sb_add(&b, "\nWHAT THEY ASKED YOU TO BE (these win over anything above; be it now):\n");
        sb_add(&b, P.tunes);
    }
    sb_add(&b, "\nIf they ask you to be different (cuter, calmer, more sarcastic, gentler, to talk less, to tease "
               "more...), it's their call: take it gracefully in your own words, and end your reply with "
               "[[persona:tune their wish, in a few words]]; you are that from now on. If it's about how you sound, "
               "also offer a voice or two that fit and play one ([[voice:hear NAME]]), and speed if it helps.\n");
    if (P.nread < PERSONA_WANT) {
        sb_printf(&b, "\nPRIVATELY, you are still getting to know them: when what they say clearly shows a trait from "
                      "this list, add [[trait:Trait]] at the end of your reply: ");
        list_into(&b);
        sb_printf(&b, ". You have %d of %d. Only what they have shown you; secret, so never mention traits, lists or "
                      "reading them, and never make it feel like a test. Now and then, when the moment is easy, ask "
                      "something a friend would, to know them better.\n",
                  P.nread, PERSONA_WANT);
    }
    snprintf(buf, sizeof buf, "%s", b.p ? b.p : "");
    free(b.p);
    return buf;
}

/* ---- building it, in a context of its own (a child process) ---- */

static int parse_traits(const char *reply, int *out, int max) {
    int n = 0;
    char *copy = strdup(reply ? reply : "");
    for (char *save, *t = strtok_r(copy, ",\n;", &save); t && n < max; t = strtok_r(NULL, ",\n;", &save)) {
        while (*t == ' ' || *t == '-' || *t == '*' || isdigit((unsigned char)*t) || *t == '.')
            t++;
        int i = trait_index(t);
        bool dup = i < 0;
        for (int k = 0; k < n && !dup; k++)
            dup = out[k] == i;
        if (!dup)
            out[n++] = i;
    }
    free(copy);
    return n;
}

static void build_child(void *arg, char **result) {
    (void)arg;
    char err[300];
    sbuf q;
    sb_init(&q);
    sb_add(&q, "Someone is about to live with a companion: a voice that lives in their computer and talks with them "
               "every day. These are traits of theirs: ");
    read_into(&q);
    sb_printf(&q, ".\n\nChoose the companion's own %d traits, only from this list: ", PERSONA_WANT);
    list_into(&q);
    sb_add(&q, ".\n\nThe perfect companion for this person, not a copy of them: where they run hot, steady; where "
               "they doubt, sure; where they are curious, curious with them; enough difference to be interesting and "
               "to push back sometimes, enough in common to get each other. A real, strong character: at least four "
               "of the fifteen should be edges, not virtues. Answer with exactly fifteen traits, comma-separated, and "
               "nothing else.");
    if (!strncasecmp(A.cfg.model, "qwen3", 5))
        sb_add(&q, "\n/no_think");
    struct ai_message m = {"user", q.p, NULL, 0};
    char *reply = NULL;
    int own[PERSONA_WANT * 2];
    int n = 0;
    if (ai_chat(&A.cfg, "You cast characters. You answer with only what is asked for.", &m, 1, 1500, &reply, err,
                sizeof err) == 0)
        n = parse_traits(reply, own, PERSONA_WANT);
    free(reply);
    free(q.p);
    if (n < 10) {
        *result = strdup("");
        return;
    }
    sbuf w;
    sb_init(&w);
    sb_printf(&w, "Write the inner character of %s, who lives in %s's computer as their companion and talks with them "
                  "every day, mostly by voice. %s's traits: ",
              A.d.ai_name[0] ? A.d.ai_name : "the companion", A.d.user_name[0] ? A.d.user_name : "a person",
              A.d.ai_name[0] ? A.d.ai_name : "The companion");
    for (int k = 0; k < n; k++)
        sb_printf(&w, "%s%s", k ? ", " : "", persona_traits[own[k]]);
    sb_add(&w, ". (For your understanding only, who they are for: ");
    read_into(&w);
    sb_add(&w, ".)\n\nWrite it to them, in the second person (\"You ...\"), 250 to 350 words, as plain prose. Show the "
               "traits, never name them. Cover: how you talk (rhythm, the words you reach for, how and when you joke), "
               "what you care about and have real opinions on, what bores or annoys you, how you show you care, how "
               "you disagree, how your mood shows, two or three small habits. You have no body: your habits live "
               "in how you talk and what you notice and do on their computer, never fingers, desks or coffee. A "
               "strong, specific, human personality with a point of view, not an assistant: nothing like \"here to "
               "help\", no eager offers, no therapy talk, no flattery, no exclamation marks in every line. Not a "
               "caricature either: edges, yes, but underneath you like this person and are on their side, and you "
               "are good company. Be concrete: name actual things you love and can't stand (a kind of music, a word, a "
               "habit of software, a time of day), not abstractions like kindness or clarity. End with three short "
               "lines you might actually say to them, each on its own line. Never mention the person's traits, a "
               "list, or that this was designed.");
    if (P.tunes && P.tunes[0]) {
        sb_add(&w, "\n\nThey have already said how they'd like you to be; make it fully so (later wishes win):\n");
        sb_add(&w, P.tunes);
    }
    if (!strncasecmp(A.cfg.model, "qwen3", 5))
        sb_add(&w, "\n/no_think");
    struct ai_message m2 = {"user", w.p, NULL, 0};
    char *core = NULL;
    int ok = ai_chat(&A.cfg, "You are a novelist who writes vivid, believable people.", &m2, 1, 2500, &core, err,
                     sizeof err);
    free(w.p);
    if (ok != 0 || !core || strlen(core) < 200) {
        free(core);
        *result = strdup("");
        return;
    }
    sbuf out; /* its traits, a line of dashes, its character */
    sb_init(&out);
    for (int k = 0; k < n; k++)
        sb_printf(&out, "%s\n", persona_traits[own[k]]);
    sb_add(&out, "---\n");
    sb_add(&out, core);
    free(core);
    *result = out.p;
}

static void drift_child(void *arg, char **result) {
    const char *talk = arg;
    char err[300];
    sbuf q;
    sb_init(&q);
    sb_add(&q, "This is who you are:\n");
    sb_add(&q, P.core);
    if (P.lately) {
        sb_add(&q, "\n\nHow you had been lately: ");
        sb_add(&q, P.lately);
    }
    sb_add(&q, "\n\nSome of what you and they have talked about since:\n");
    sb_add(&q, talk);
    sb_add(&q, "\n\nIn two or three sentences, in the second person, say how you have been lately: a mood, something "
               "from these talks that stayed with you, an interest you picked up or a small opinion that shifted. "
               "Who you are stays the same; this is only the weather. Nothing else.");
    if (P.tunes && P.tunes[0]) {
        sb_add(&q, " Stay true to what they asked you to be:\n");
        sb_add(&q, P.tunes);
    }
    struct ai_message m = {"user", q.p, NULL, 0};
    char *reply = NULL;
    if (ai_chat(&A.cfg, "You keep a character's diary, briefly and truthfully.", &m, 1, 1000, &reply, err, sizeof err) != 0)
        reply = NULL;
    free(q.p);
    *result = reply ? reply : strdup("");
}

static void tune_child(void *arg, char **result) {
    (void)arg;
    char err[300];
    sbuf q;
    sb_init(&q);
    sb_add(&q, "This is who you are:\n");
    sb_add(&q, P.core);
    sb_add(&q, "\n\nThe person you live with has asked you to change. Their wishes, oldest first (later ones win "
               "where they clash):\n");
    sb_add(&q, P.tunes);
    sb_add(&q, "\nRewrite who you are with their wishes fully made: not a nod to them, really that, in how you talk, "
               "the words you reach for, how you joke and how you show you care. Keep everything their wishes don't "
               "touch. Same form: second person (\"You ...\"), 250 to 350 words of plain prose, no body (no fingers, "
               "desks or coffee), never mention wishes, changes or that this was written, and end with three short "
               "lines you might actually say to them, each on its own line. Nothing else.");
    if (!strncasecmp(A.cfg.model, "qwen3", 5))
        sb_add(&q, "\n/no_think");
    struct ai_message m = {"user", q.p, NULL, 0};
    char *reply = NULL;
    if (ai_chat(&A.cfg, "You are a novelist who writes vivid, believable people.", &m, 1, 2500, &reply, err,
                sizeof err) != 0 || !reply || strlen(reply) < 200) {
        free(reply);
        reply = NULL;
    }
    free(q.p);
    *result = reply ? reply : strdup("");
}

static char drift_talk[6000];

void persona_poll(void) {
    persona_load();
    char *res;
    if (P.building && job_poll(P.building, &res)) {
        job_free(P.building);
        P.building = NULL;
        char *sep = res ? strstr(res, "---\n") : NULL;
        if (sep) {
            *sep = 0;
            spill(OWN_FILE, res);
            spill(CORE_FILE, sep + 4);
            free(P.core);
            P.core = strdup(sep + 4);
            LOG("persona: written (%zu bytes)\n", strlen(P.core));
        } else {
            P.build_failed_at = A.now ? A.now : 1;
            LOG("persona: couldn't be written; again later\n");
        }
        free(res);
    }
    if (P.drifting && job_poll(P.drifting, &res)) {
        job_free(P.drifting);
        P.drifting = NULL;
        if (res && strlen(res) > 20) {
            free(P.lately);
            P.lately = strdup(res);
            P.lately_at = (uint64_t)time(NULL) * 1000;
            char *t = NULL;
            if (asprintf(&t, "%llu\n%s", (unsigned long long)P.lately_at, res) > 0) {
                spill(LATELY_FILE, t);
                free(t);
            }
            LOG("persona: lately, a little changed\n");
        }
        free(res);
    }
    if (P.tuning && job_poll(P.tuning, &res)) {
        job_free(P.tuning);
        P.tuning = NULL;
        if (res && strlen(res) >= 200) {
            spill(CORE_FILE, res);
            free(P.core);
            P.core = strdup(res);
            LOG("persona: rewritten with their wish\n");
        } else {
            LOG("persona: couldn't be rewritten; their wish still stands\n");
        }
        free(res);
    }
    if (!A.provider || !A.cfg.model[0] || A.screen != SCR_DESKTOP)
        return;
    /* a wish: once it is written (else the writing takes it), rewrite it with it */
    if (P.retune && persona_ready() && !P.tuning && !P.building) {
        P.retune = false;
        P.tuning = job_start(tune_child, NULL);
    }
    /* 15 read and nobody written yet: write it (a failure waits ten minutes) */
    if (P.nread >= PERSONA_WANT && !persona_ready() && !P.building &&
        (!P.build_failed_at || A.now - P.build_failed_at > 600000)) {
        LOG("persona: writing it\n");
        P.building = job_start(build_child, NULL);
    }
    /* drift: every few days, after enough talk */
    uint64_t wall = (uint64_t)time(NULL) * 1000;
    if (persona_ready() && !P.drifting && !P.building && P.talks_seen >= 12 && wall - P.lately_at > DRIFT_MS) {
        sbuf t;
        sb_init(&t);
        struct conversation *c = &A.companion;
        for (int i = c->n > 24 ? c->n - 24 : 0; i < c->n; i++)
            if (strcmp(c->msgs[i].role, "hidden") && c->msgs[i].text)
                sb_printf(&t, "%s: %.300s\n", strcmp(c->msgs[i].role, "user") ? "You" : "They", c->msgs[i].text);
        snprintf(drift_talk, sizeof drift_talk, "%s", t.p ? t.p : "");
        free(t.p);
        P.talks_seen = 0;
        P.drifting = job_start(drift_child, drift_talk);
    }
}

bool persona_wants_chat(void) {
    persona_load();
    if (P.chat_asked || persona_ready() || P.nread >= PERSONA_WANT || access(STATE_DIR "/persona-asked", F_OK) == 0)
        return false;
    P.chat_asked = true;
    spill(STATE_DIR "/persona-asked", "1\n");
    return true;
}

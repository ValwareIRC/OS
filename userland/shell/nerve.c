#define _GNU_SOURCE
/* The companion's nervous system: see nerve.h. */
#include "nerve.h"
#include "app.h"
#include "alive.h"
#include "voice.h"
#include "agent.h"
#include "../lib/json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define D (A.d)
#define WAKE_MARK "(Your reflexes flagged this for you"

/* reflex.c */
void reflex_tick(void);
bool reflex_ready(void);
bool reflex_busy(void);
bool reflex_judge(const char *situation, const char *event);
bool reflex_result(enum nerve_verdict *v, float *confidence, char *why, size_t n);

#define NEV 96
static struct ev {
    time_t at;
    uint64_t ms;
    char actor;
    char kind[16];
    int weight;
    char text[220];
    enum nerve_verdict v;
    float conf;
    char why[120];
    bool delivered; /* woken for */
    bool judging;
} ev[NEV];
static int nev;

static bool ai_hands;
static uint64_t person_closed_at; /* they last closed a window: what ends right after is because of it */
static uint64_t touched_at;

void nerve_ai_hands(bool acting) { ai_hands = acting; }
void nerve_person_touched(void) { touched_at = A.now; }

/* Who did it, when the caller can't say: its body if it is acting, them if they just touched the computer,
 * else the system on its own. */
static char attribute(void) {
    if (ai_hands)
        return BY_AI;
    if (touched_at && A.now - touched_at < 1500)
        return BY_PERSON;
    return BY_SYSTEM;
}

/* Events whose meaning is fixed: settled here, no model asked. */
static enum nerve_verdict by_rule(const struct ev *e) {
    if (e->actor == BY_PERSON || e->actor == BY_AI)
        return V_REMEMBER; /* their own doing, or its own: known, never news */
    if (e->weight >= 3)
        return V_URGENT;
    static const char *const routine[] = {"window", "focus", "app", "design", "did", NULL};
    for (int i = 0; routine[i]; i++)
        if (!strcmp(e->kind, routine[i]))
            return V_REMEMBER; /* the system going about its ordinary business */
    return V_PENDING;
}

void nerve(char actor, const char *kind, int weight, const char *fmt, ...) {
    if (nev == NEV)
        memmove(&ev[0], &ev[1], sizeof ev[0] * (NEV - 1)), nev--;
    struct ev *e = &ev[nev++];
    memset(e, 0, sizeof *e);
    e->at = time(NULL);
    e->ms = A.now;
    /* who: told; or its hands; a key or click only claims what keys and clicks do (windows, apps); what ends
     * because they closed something is theirs too; else the system's own doing */
    bool input_kind = !strcmp(kind, "window") || !strcmp(kind, "app") || !strcmp(kind, "focus");
    if (actor)
        e->actor = actor;
    else if (ai_hands)
        e->actor = BY_AI;
    else if (input_kind)
        e->actor = attribute();
    else if (!strcmp(kind, "task") && person_closed_at && A.now - person_closed_at < 15000)
        e->actor = BY_PERSON;
    else
        e->actor = BY_SYSTEM;
    e->weight = weight;
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof e->text, fmt, ap);
    va_end(ap);
    if (!strcmp(kind, "window") && e->actor == BY_PERSON && strstr(e->text, "closed"))
        person_closed_at = A.now;
    e->v = by_rule(e);
    e->conf = 1;
    LOG("nerve: %c %s (%d) %s -> %s\n", e->actor, e->kind, weight, e->text,
        e->v == V_PENDING ? "for the reflexes" : e->v == V_REMEMBER ? "remember" : "urgent");
}

static const char *who(char a) { return a == BY_PERSON ? "they" : a == BY_AI ? "you" : "the system"; }

static void ago(time_t t, char *out, size_t n) {
    long s = (long)(time(NULL) - t);
    if (s < 60) snprintf(out, n, "%lds ago", s);
    else if (s < 3600) snprintf(out, n, "%ldm ago", s / 60);
    else snprintf(out, n, "%ldh %ldm ago", s / 3600, (s % 3600) / 60);
}

const char *nerve_context(void) {
    static char buf[4000];
    sbuf b;
    sb_init(&b);
    int shown = 0;
    time_t now = time(NULL);
    /* the latest first in mind, written oldest first; two hours back at most */
    int from = nev;
    for (int i = nev - 1; i >= 0 && shown < 24; i--)
        if (ev[i].v >= V_REMEMBER && now - ev[i].at < 7200)
            from = i, shown++;
    for (int i = from; i < nev; i++) {
        if (ev[i].v < V_REMEMBER || now - ev[i].at >= 7200)
            continue;
        char a[24];
        ago(ev[i].at, a, sizeof a);
        sb_printf(&b, "\n  %s, %s: %s", a, who(ev[i].actor), ev[i].text);
    }
    snprintf(buf, sizeof buf, "%s", b.p ? b.p : "");
    free(b.p);
    return buf;
}

bool nerve_is_wake_note(const char *text) { return text && !strncmp(text, WAKE_MARK, strlen(WAKE_MARK)); }

/* ---- the moment around them, for the reflexes ---- */

static void situation(char *out, size_t n) {
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char when[40];
    strftime(when, sizeof when, "%A %H:%M", &tm);
    const struct senses *s = senses_now();
    long idle = s && s->last_input ? (long)((A.now - s->last_input) / 60000) : 0;
    char recent[600] = "";
    size_t o = 0;
    for (int i = nev - 1, k = 0; i >= 0 && k < 4; i--)
        if (ev[i].actor != BY_SYSTEM && A.now - ev[i].ms < 120000) {
            o += (size_t)snprintf(recent + o, sizeof recent - o, "%s%s %s", k ? "; " : " Just before: ", who(ev[i].actor),
                                  ev[i].text);
            k++;
        }
    snprintf(out, n, "It is %s. The person is %s%s%s. %s%s", when,
             senses_fullscreen() ? "watching or playing something full screen" : senses_typing() ? "typing"
             : idle >= 5 ? "away from the computer" : "at the computer",
             idle >= 5 ? "" : "", idle >= 5 ? " for a while" : "",
             alive_prefs.quiet ? "They asked for quiet." : "", recent);
}

/* ---- waking it ---- */

static uint64_t woke_at[8];
static int nwoke;

static bool moment_allows(bool urgent) {
    if (A.screen != SCR_DESKTOP || A.now - A.screen_since < 25000)
        return false;
    if (urgent)
        return !A.companion.pending;
    if (!alive_prefs.proactive || alive_prefs.quiet)
        return false;
    if (senses_typing() || senses_fullscreen())
        return false;
    struct conversation *c = &A.companion;
    if (c->pending || agent_busy() || actions_confirming() || voice_listening() || voice_hearing() || voice_speaking())
        return false;
    for (int i = c->n - 1; i >= 0 && i >= c->n - 2; i--) /* in the middle of talking: a lull first */
        if (!strcmp(c->msgs[i].role, "user") && A.now - c->pending_since < 90000)
            return false;
    if (A.caption[0] && A.now < A.caption_until)
        return false;
    /* a small budget: a few times an hour */
    int last_hour = 0;
    uint64_t last = 0;
    for (int i = 0; i < nwoke; i++) {
        if (A.now - woke_at[i] < 3600000)
            last_hour++;
        if (woke_at[i] > last)
            last = woke_at[i];
    }
    return last_hour < 3 && (!last || A.now - last >= 5 * 60000);
}

static void wake(void) {
    int pick[4], np = 0;
    bool urgent = false;
    for (int i = 0; i < nev && np < 4; i++) {
        struct ev *e = &ev[i];
        if ((e->v != V_WAKE && e->v != V_URGENT) || e->delivered)
            continue;
        if (A.now - e->ms > (e->v == V_URGENT ? 30 * 60000ull : 15 * 60000ull)) { /* stale: only remembered */
            e->delivered = true;
            continue;
        }
        pick[np++] = i;
        urgent |= e->v == V_URGENT;
    }
    if (!np || !moment_allows(urgent))
        return;
    sbuf note;
    sb_init(&note);
    sb_add(&note, WAKE_MARK ", just now; it is not something they said):");
    for (int k = 0; k < np; k++) {
        struct ev *e = &ev[pick[k]];
        sb_printf(&note, "\n- %s%s%s", e->text, e->why[0] ? " (" : "", e->why);
        if (e->why[0])
            sb_add(&note, ")");
        e->delivered = true;
    }
    sb_add(&note, "\nYou are the system this happened in. Decide, as yourself: if something needs doing, do it with "
                  "your body; if they would want to know, tell them in a sentence; if neither, answer with nothing at "
                  "all (no words, no tags).");
    if (urgent)
        sb_add(&note, " It matters: be clear, not alarming.");
    sb_add(&note, ")");
    if (nwoke == 8)
        memmove(&woke_at[0], &woke_at[1], sizeof woke_at[0] * 7), nwoke--;
    woke_at[nwoke++] = A.now;
    LOG("nerve: waking it: %s\n", note.p);
    agent_new_request();
    companion_note(note.p, true);
    free(note.p);
}

/* For tests (and for scripts of theirs): lines "actor|kind|weight|text" put in /tmp/nerve-inject arrive as events */
static void injected(void) {
    static uint64_t looked;
    if (A.now - looked < 1000)
        return;
    looked = A.now;
    FILE *f = fopen("/tmp/nerve-inject", "r");
    if (!f)
        return;
    char line[400];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *p1 = strchr(line, '|'), *p2 = p1 ? strchr(p1 + 1, '|') : NULL, *p3 = p2 ? strchr(p2 + 1, '|') : NULL;
        if (!p3)
            continue;
        *p1 = *p2 = *p3 = 0;
        char actor = !strcmp(line, "person") ? BY_PERSON : !strcmp(line, "ai") ? BY_AI : BY_SYSTEM;
        LOG("nerve: injected\n");
        nerve(actor, p1 + 1, atoi(p2 + 1), "%s", p3 + 1);
    }
    fclose(f);
    unlink("/tmp/nerve-inject");
}

void nerve_tick(void) {
    injected();
    reflex_tick();
    /* a verdict back: kept */
    enum nerve_verdict v;
    float conf;
    char why[120];
    if (reflex_result(&v, &conf, why, sizeof why)) {
        for (int i = 0; i < nev; i++)
            if (ev[i].judging) {
                ev[i].judging = false;
                /* not sure enough to bother it: kept as something it knows instead */
                if ((v == V_WAKE && conf < 0.5f) || v == V_PENDING)
                    v = V_REMEMBER;
                if (v == V_URGENT && conf < 0.6f)
                    v = V_WAKE;
                ev[i].v = v;
                ev[i].conf = conf;
                snprintf(ev[i].why, sizeof ev[i].why, "%s", why);
                LOG("nerve: reflex: %s -> %s (%.2f) %s\n", ev[i].text,
                    v == V_IGNORE ? "ignore" : v == V_REMEMBER ? "remember" : v == V_WAKE ? "wake" : "urgent", conf, why);
                break;
            }
    }
    /* the next one to judge */
    if (!reflex_busy())
        for (int i = 0; i < nev; i++) {
            struct ev *e = &ev[i];
            if (e->v != V_PENDING || e->judging)
                continue;
            if (!reflex_ready()) { /* no model to ask: the rules alone, carefully */
                e->v = e->weight >= 2 ? V_WAKE : V_REMEMBER;
                continue;
            }
            char sit[1000], what[300];
            situation(sit, sizeof sit);
            snprintf(what, sizeof what, "%s (%s)", e->text, e->actor == BY_SYSTEM ? "it happened by itself" : who(e->actor));
            if (reflex_judge(sit, what))
                e->judging = true;
            break;
        }
    wake();
}

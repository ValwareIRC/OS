/* The screen reader: the companion reads what has their attention, anywhere on the computer, in its own voice and
 * words. Not "button, Uninstall, 3 of 5": "Uninstall Firefox, it asks before removing it".
 *
 * What has their attention, in this order: an open right-click menu (what is chosen in it); in another app's
 * window, what has focus there (os-a11y: the accessibility bus, userland/a11y); what the pointer rests on (a
 * control of the OS's own, which says what it is as it is drawn: a11y_note, or an element of the design); a field
 * being typed in. And what happens by itself: a notification, a message on the screen, a window coming forward.
 * Briefly as they move; when they stop on something and it says more (Settings), the companion's mind describes it
 * fully. Ctrl+Alt+R on or off (anywhere, from the very first screen, which offers it), Ctrl+Alt+W where am I,
 * Ctrl+Alt+A say it again, Ctrl+Alt+D the whole screen, described; Ctrl+Alt+Right / Left the next thing on the
 * screen or the one before, Ctrl+Alt+Enter uses it (everything the OS draws is reached by the keys). Any key cuts
 * it off. */
#include "reader.h"
#include "app.h"
#include "persona.h"
#include "settings.h"
#include "voice.h"
#include "fm/fm.h"
#include "../lib/json.h"

#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

#define A11Y_HELPER "/usr/libexec/os-a11y"

/* ---- what the OS's own controls say they are ---- */

#define MAX_NOTES 320
static struct note {
    rect r;
    char role[12], label[96], state[120];
    bool focused;
} notes[MAX_NOTES];
static int nnotes;

void a11y_frame_begin(void) { nnotes = 0; }

void a11y_note(rect r, const char *role, const char *label, const char *state, bool focused) {
    if (nnotes == MAX_NOTES || r.w <= 0 || r.h <= 0)
        return;
    struct note *n = &notes[nnotes++];
    n->r = r;
    snprintf(n->role, sizeof n->role, "%s", role ? role : "");
    snprintf(n->label, sizeof n->label, "%s", label ? label : "");
    snprintf(n->state, sizeof n->state, "%s", state ? state : "");
    n->focused = focused;
}

/* A control drawn without words of its own (a switch beside its row's label): the label on its line, to its left. */
static const char *label_for(const struct note *n) {
    if (n->label[0])
        return n->label;
    const struct note *best = NULL;
    for (int i = 0; i < nnotes; i++) {
        const struct note *l = &notes[i];
        if (strcmp(l->role, "label") || !l->label[0])
            continue;
        bool same_line = l->r.y < n->r.y + n->r.h && n->r.y < l->r.y + l->r.h;
        if (same_line && l->r.x <= n->r.x && (!best || l->r.x > best->r.x))
            best = l;
    }
    return best ? best->label : "";
}

static void words_of_note(const struct note *n, char *out, size_t sz) {
    const char *l = label_for(n);
    if (!strcmp(n->role, "switch") || !strcmp(n->role, "tile"))
        snprintf(out, sz, "%s, %s", l[0] ? l : "A switch", !strcmp(n->state, "on") ? "on" : "off");
    else if (!strcmp(n->role, "field"))
        snprintf(out, sz, "%s box, %s", l[0] ? l : "A", n->state[0] ? n->state : "empty");
    else if (!strcmp(n->role, "slider"))
        snprintf(out, sz, "%s, at %s", l[0] ? l : "A slider", n->state);
    else if (!strcmp(n->role, "choice"))
        snprintf(out, sz, "%s%s", l, !strcmp(n->state, "on") ? ", chosen" : "");
    else if (!strcmp(n->role, "label"))
        snprintf(out, sz, "%s%s%s", l, n->state[0] ? ". " : "", n->state);
    else
        snprintf(out, sz, "%s%s%s", l, n->state[0] && strcmp(n->state, "on") ? ", " : "",
                 n->state[0] && strcmp(n->state, "on") ? n->state : "");
}

/* ---- on, off ---- */

static int on = -1, full = -1;

bool reader_on(void) {
    if (on < 0)
        on = !strcmp(settings_get("reader.on", "0"), "1");
    return on;
}
bool reader_full(void) {
    if (full < 0)
        full = !strcmp(settings_get("reader.full", "1"), "1");
    return full;
}
void reader_set_full(bool f) {
    full = f;
    settings_set("reader.full", f ? "1" : "0");
}

static char last_said[600];
static uint64_t said_at;

static void say(const char *text) {
    if (!text || !*text)
        return;
    snprintf(last_said, sizeof last_said, "%s", text);
    said_at = A.now;
    voice_read(text);
    LOG("reader: \"%.200s\"\n", text);
}

/* ---- other apps: os-a11y ---- */

static struct {
    pid_t pid;
    int in, out; /* (its stdin, its stdout) */
    char buf[16384];
    size_t len;
    uint64_t started_at, retry_at;
    int quick_ends;
} H = {0, -1, -1, {0}, 0, 0, 0, 0};

static void helper_stop(void) {
    if (H.pid > 0) {
        if (H.in >= 0 && write(H.in, "quit\n", 5) < 0) {
        }
        kill(H.pid, SIGTERM);
        waitpid(H.pid, NULL, 0);
    }
    if (H.in >= 0)
        close(H.in);
    if (H.out >= 0)
        close(H.out);
    H.pid = 0, H.in = H.out = -1, H.len = 0;
}

static void helper_start(void) {
    if (H.pid > 0 || access(A11Y_HELPER, X_OK) != 0 || A.now < H.retry_at)
        return;
    int to[2], from[2];
    if (pipe2(to, O_CLOEXEC) < 0)
        return;
    if (pipe2(from, O_CLOEXEC) < 0) {
        close(to[0]), close(to[1]);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(to[0], 0);
        dup2(from[1], 1);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0)
            dup2(nul, 2);
        become_session_user(); /* (the apps' accessibility bus is the person's session's) */
        execl(A11Y_HELPER, "os-a11y", (char *)NULL);
        _exit(127);
    }
    close(to[0]), close(from[1]);
    if (pid < 0) {
        close(to[1]), close(from[0]);
        return;
    }
    fcntl(from[0], F_SETFL, O_NONBLOCK);
    H.pid = pid, H.in = to[1], H.out = from[0], H.len = 0, H.started_at = A.now;
    LOG("reader: os-a11y started (pid %d)\n", pid);
}

static void helper_send(const char *cmd) {
    if (H.in >= 0 && write(H.in, cmd, strlen(cmd)) < 0) {
    }
}

/* ---- the companion describing, fully ---- */

static job *describing;
static unsigned describing_for; /* the subject it was asked about (a newer one: its answer isn't said) */
static unsigned subject_gen;

struct ask {
    struct ai_config cfg;
    char *system, *user;
};

static void ask_child(void *arg, char **result) {
    struct ask *q = arg;
    struct ai_message m = {"user", q->user, NULL, 0};
    char *reply = NULL, err[200];
    if (ai_chat(&q->cfg, q->system, &m, 1, 220, &reply, err, sizeof err) == 0)
        *result = reply;
    else
        free(reply);
    free(q->system), free(q->user), free(q);
}

static void describe(const char *what, bool whole_screen) {
    if (!A.provider || describing)
        return;
    struct ask *q = calloc(1, sizeof *q);
    if (!q)
        return;
    sbuf s;
    sb_init(&s);
    const char *persona = persona_companion_prompt();
    if (persona && *persona) {
        sb_add(&s, persona);
        sb_add(&s, "\n\n");
    }
    sb_printf(&s,
              "You are reading the screen aloud for %s, who can't see it well or at all. %s Speak plainly, in your own "
              "way of talking, in one to three short sentences (said aloud: no lists, no symbols, no markdown). Name "
              "what matters and what they can do with it; leave out what doesn't. Never say \"button\" or \"element\" "
              "or positions like \"3 of 5\" unless it helps.",
              their_name()[0] ? their_name() : "the person",
              whole_screen ? "Tell them what is on the whole screen and where they are." : "Tell them about the thing "
              "they have stopped on.");
    q->system = s.p;
    q->user = strdup(what);
    q->cfg = A.cfg;
    describing = job_start_thread(ask_child, q);
    describing_for = subject_gen;
    if (!describing)
        free(q->system), free(q->user), free(q);
}

/* ---- what has their attention ---- */

static char subject[600];      /* what it is said to be now (the brief words) */
static char subject_more[900]; /* what is known of it, for a fuller description */
static uint64_t subject_at;
static bool described;
static char pending_tree_for[16]; /* "screen": a whole-screen description waits for an app's window contents */

static void new_subject(const char *brief, const char *more, bool now) {
    if (!brief || !*brief || !strcmp(brief, subject))
        return;
    snprintf(subject, sizeof subject, "%s", brief);
    snprintf(subject_more, sizeof subject_more, "%s", more ? more : brief);
    subject_at = A.now;
    described = false;
    subject_gen++;
    if (now)
        say(brief);
}

/* the natural words for what an app says it is (os-a11y's focus) */
static void app_words(json *j, char *brief, size_t bn, char *more, size_t mn) {
    const char *role = json_str(json_get(j, "role"), ""), *name = json_str(json_get(j, "name"), ""),
               *desc = json_str(json_get(j, "desc"), ""), *value = json_str(json_get(j, "value"), ""),
               *text = json_str(json_get(j, "text"), ""), *app = json_str(json_get(j, "app"), ""),
               *win = json_str(json_get(j, "window"), "");
    char st[200] = "";
    bool checked = false, unavailable = false, editable = false, selected = false, expanded = false, collapsed = false;
    for (json *s = json_get(j, "states") ? json_get(j, "states")->child : NULL; s; s = s->next) {
        const char *w = json_str(s, "");
        checked |= !strcmp(w, "checked") || !strcmp(w, "pressed");
        unavailable |= !strcmp(w, "unavailable");
        editable |= !strcmp(w, "editable");
        selected |= !strcmp(w, "selected");
        expanded |= !strcmp(w, "expanded");
        collapsed |= !strcmp(w, "collapsed");
    }
    int idx = (int)json_num(json_get(j, "index"), 0), of = (int)json_num(json_get(j, "of"), 0);
    const char *nm = name[0] ? name : desc;
    if (strstr(role, "check") || strstr(role, "toggle") || strstr(role, "switch"))
        snprintf(brief, bn, "%s, %s", nm, checked ? "on" : "off");
    else if (strstr(role, "radio"))
        snprintf(brief, bn, "%s%s", nm, checked ? ", chosen" : "");
    else if (editable || strstr(role, "entry") || strstr(role, "text"))
        snprintf(brief, bn, "%s%s, %s%.120s", nm[0] ? nm : "A", strstr(role, "password") ? " password box" : " box",
                 text[0] ? "says " : "empty", strstr(role, "password") ? "" : text);
    else if (strstr(role, "slider") || strstr(role, "spin") || strstr(role, "scroll bar") || strstr(role, "progress"))
        snprintf(brief, bn, "%s, at %s", nm, value);
    else if (strstr(role, "link"))
        snprintf(brief, bn, "Link: %s", nm);
    else if (strstr(role, "heading"))
        snprintf(brief, bn, "Heading: %s", nm);
    else if (strstr(role, "page tab") || !strcmp(role, "tab"))
        snprintf(brief, bn, "%s tab%s", nm, selected ? ", open" : "");
    else if (strstr(role, "combo"))
        snprintf(brief, bn, "%s, %s", nm, text[0] ? text : value);
    else if (strstr(role, "menu item") || strstr(role, "list item") || strstr(role, "tree item"))
        snprintf(brief, bn, "%s%s%s", nm, selected ? ", chosen" : "", expanded ? ", open" : collapsed ? ", closed" : "");
    else if (strstr(role, "image") || strstr(role, "icon"))
        snprintf(brief, bn, "Picture%s%s", nm[0] ? ": " : "", nm);
    else if (strstr(role, "alert") || strstr(role, "notification"))
        snprintf(brief, bn, "Alert: %s", nm);
    else
        snprintf(brief, bn, "%s", nm);
    if (unavailable)
        snprintf(st, sizeof st, ", can't be used now");
    size_t bl = strlen(brief);
    snprintf(brief + bl, bn - bl, "%s", st);
    snprintf(more, mn, "In %s, the window \"%s\": a %s called \"%s\"%s%s%s%s%s. Position %d of %d among its neighbours.%s%s",
             app, win, role, name, desc[0] ? " (" : "", desc, desc[0] ? ")" : "", text[0] ? ", with the text: " : "",
             text, idx, of, value[0] ? " Its value: " : "", value);
}

static void helper_poll(void) {
    if (H.pid > 0) {
        int st = 0;
        if (waitpid(H.pid, &st, WNOHANG) == H.pid) { /* ended: started again, later and later if it keeps ending */
            uint64_t ran = A.now - H.started_at;
            H.quick_ends = ran < 30000 ? H.quick_ends + 1 : 0;
            H.retry_at = A.now + (H.quick_ends ? 2000ull << (H.quick_ends < 6 ? H.quick_ends : 6) : 0);
            H.pid = 0;
            close(H.in), close(H.out);
            H.in = H.out = -1;
            LOG("reader: os-a11y ended after %llu s\n", (unsigned long long)ran / 1000);
        }
    }
    if (H.out < 0)
        return;
    for (;;) {
        ssize_t n = read(H.out, H.buf + H.len, sizeof H.buf - 1 - H.len);
        if (n <= 0)
            break;
        H.len += (size_t)n;
    }
    H.buf[H.len] = 0;
    char *line = H.buf, *nl;
    while ((nl = strchr(line, '\n'))) {
        *nl = 0;
        json *j = json_parse(line, strlen(line));
        const char *ev = j ? json_str(json_get(j, "ev"), "") : "";
        int fw = desktop_focused_window();
        bool app_in_front = fw >= WIN_COUNT && A.win[fw].view; /* (its words are theirs only while it is in front) */
        if (!strcmp(ev, "focus") || !strcmp(ev, "changed") || !strcmp(ev, "where")) {
            char brief[400], more[900];
            app_words(j, brief, sizeof brief, more, sizeof more);
            if (!strcmp(ev, "where")) {
                char w[600];
                snprintf(w, sizeof w, "%s, %s. %s", json_str(json_get(j, "app"), ""), json_str(json_get(j, "window"), ""),
                         brief);
                say(w);
            } else if (app_in_front) {
                if (!strcmp(ev, "changed"))
                    subject[0] = 0; /* (the same thing, changed: said again) */
                new_subject(brief, more, true);
            }
        } else if (!strcmp(ev, "caret") && app_in_front) {
            static uint64_t moved_by_keys_at;
            extern uint64_t reader_arrows_at;
            moved_by_keys_at = reader_arrows_at;
            if (A.now - moved_by_keys_at < 400) /* (moved with the arrows: the line it is on; not while typing) */
                say(json_str(json_get(j, "line"), ""));
        } else if (!strcmp(ev, "window") && app_in_front) {
            char w[300];
            snprintf(w, sizeof w, "%s, %s", json_str(json_get(j, "app"), ""), json_str(json_get(j, "name"), ""));
            say(w);
        } else if (!strcmp(ev, "announce")) {
            say(json_str(json_get(j, "text"), ""));
        } else if (!strcmp(ev, "tree") && pending_tree_for[0]) {
            pending_tree_for[0] = 0;
            sbuf s;
            sb_init(&s);
            char *ui = A.ui ? fm_describe(A.ui) : NULL;
            sb_add(&s, "The window in front, what it is made of:\n");
            for (json *l = json_get(j, "lines") ? json_get(j, "lines")->child : NULL; l; l = l->next) {
                sb_add(&s, json_str(l, ""));
                sb_add(&s, "\n");
            }
            sb_add(&s, "\nThe desktop around it:\n");
            sb_add(&s, ui ? ui : "");
            free(ui);
            describe(s.p ? s.p : "", true);
            sb_free(&s);
        }
        if (j)
            json_free(j);
        line = nl + 1;
    }
    size_t rest = strlen(line);
    memmove(H.buf, line, rest + 1);
    H.len = rest;
    if (H.len >= sizeof H.buf - 2) /* (a line longer than all of it: dropped) */
        H.len = 0;
}

void reader_set(bool o) {
    on = o;
    settings_set("reader.on", o ? "1" : "0");
    if (o) {
        helper_start();
        say("The screen reader is on. Control and Alt with the arrows moves from one thing to the next, with Enter "
            "uses it. Control, Alt and W says where you are, D describes the screen, A says it again, R turns me off.");
    } else {
        helper_stop();
        voice_hush();
        voice_read("The screen reader is off.");
    }
    LOG("reader: %s\n", o ? "on" : "off");
}

/* ---- the reading cursor: everything on the screen, one after another, by the keys ----
 * Ctrl+Alt+Right / Left: the next or the one before (every control of the OS's own as drawn, every part of the
 * design that does something or says something); Ctrl+Alt+Enter: uses it (a click on it, as the pointer would). */

static bool design_words(const fm_node *n, char *out, size_t sz);

#define MAX_STOPS 400
static struct stop {
    rect r;
    char words[300];
} stops[MAX_STOPS];
static int nstops, at_stop = -1;
static int click_x = -1, click_y = -1; /* a use of it, done next turn (a click there) */

static void design_stops(const fm_node *n) {
    for (const fm_node *c = n ? n->first : NULL; c && nstops < MAX_STOPS; c = c->next) {
        if (c->kind != FM_ELEM || c->st.hidden || !c->laid_out || c->vis.w <= 0 || !strcmp(c->id, "_menu"))
            continue;
        bool pop = !strcmp(c->tag, "popover") || !strcmp(c->tag, "menu") || !strcmp(c->tag, "sheet");
        if (pop && !c->open)
            continue;
        bool acts = fm_attr_get(c, "what") || fm_attr_get(c, "app") || !strcmp(c->tag, "search") ||
                    !strcmp(c->tag, "clock") || (c->win >= 0 && !strcmp(c->tag, "item"));
        char w[300];
        if (acts && design_words(c, w, sizeof w)) {
            stops[nstops].r = c->vis;
            snprintf(stops[nstops++].words, sizeof stops[0].words, "%s", w);
            continue; /* (what is inside it is it) */
        }
        design_stops(c);
    }
}

static void gather_stops(void) {
    nstops = 0;
    for (int i = 0; i < nnotes && nstops < MAX_STOPS; i++) { /* the OS's own controls, as drawn */
        if (!strcmp(notes[i].role, "label") && !notes[i].state[0])
            continue;
        stops[nstops].r = notes[i].r;
        words_of_note(&notes[i], stops[nstops].words, sizeof stops[0].words);
        if (stops[nstops].words[0])
            nstops++;
    }
    if (A.screen == SCR_DESKTOP && A.ui)
        design_stops(A.ui->desktop);
}

static void move_stop(int d) {
    gather_stops();
    if (!nstops) {
        say("There is nothing here to move to.");
        return;
    }
    at_stop = at_stop < 0 ? (d > 0 ? 0 : nstops - 1) : (at_stop + d + nstops) % nstops;
    if (at_stop >= nstops)
        at_stop = 0;
    say(stops[at_stop].words);
}

static void use_stop(void) {
    if (at_stop < 0 || at_stop >= nstops) {
        say("Nothing chosen yet: Control, Alt and the right arrow moves to the first thing.");
        return;
    }
    rect r = stops[at_stop].r;
    click_x = r.x + r.w / 2, click_y = r.y + r.h / 2;
}

/* ---- the keys ---- */

uint64_t reader_arrows_at; /* (an arrow key: the line the caret moved to is said) */
static bool alt_down;

static void drop_key(int ch) { /* the chord's own letter, not typed into anything */
    for (int i = 0; i < A.in.nkeys; i++)
        if ((A.in.keymod[i] & KM_CTRL) && tolower(A.in.keys[i]) == ch) {
            memmove(&A.in.keys[i], &A.in.keys[i + 1], sizeof A.in.keys[0] * (size_t)(A.in.nkeys - i - 1));
            memmove(&A.in.keymod[i], &A.in.keymod[i + 1], (size_t)(A.in.nkeys - i - 1));
            A.in.nkeys--, i--;
        }
}

static void where_am_i(void) {
    int fw = desktop_focused_window();
    if (fw >= WIN_COUNT && A.win[fw].view && H.pid > 0) {
        helper_send("where\n"); /* (its answer is said when it comes) */
        return;
    }
    char w[700];
    const char *place = fw >= 0 ? desktop_window_title(fw) : "the desktop";
    snprintf(w, sizeof w, "%s%s. %s", place, fw >= 0 ? " window" : "", subject[0] ? subject : "");
    say(w);
}

static void describe_screen(void) {
    int fw = desktop_focused_window();
    if (!A.provider) {
        say("I can't describe the whole screen without my mind connected; Control, Alt and W says where you are.");
        return;
    }
    say("Looking.");
    if (fw >= WIN_COUNT && A.win[fw].view && H.pid > 0) { /* the app's own contents first (os-a11y), then */
        snprintf(pending_tree_for, sizeof pending_tree_for, "screen");
        helper_send("describe\n");
        return;
    }
    sbuf s;
    sb_init(&s);
    char *ui = A.ui ? fm_describe(A.ui) : NULL;
    sb_add(&s, "Windows open:");
    for (int w = 0; w < WIN_MAX; w++)
        if (A.win[w].open)
            sb_printf(&s, " %s%s%s;", desktop_window_title(w), w == fw ? " (in front)" : "",
                      A.win[w].minimized ? " (minimized)" : "");
    sb_add(&s, "\nWhat the pointer or the keys are on now: ");
    sb_add(&s, subject[0] ? subject : "nothing in particular");
    sb_add(&s, "\nThe desktop:\n");
    sb_add(&s, ui ? ui : "");
    free(ui);
    describe(s.p ? s.p : "", true);
    sb_free(&s);
}

void reader_keys(void) {
    if (click_x >= 0) { /* Ctrl+Alt+Enter, a turn ago: a click there, as the pointer's would be */
        A.in.mx = click_x, A.in.my = click_y;
        A.in.pressed = A.in.clicked = A.in.moved = true;
        A.in.down = false;
        click_x = click_y = -1;
    }
    bool hush = false;
    for (int i = 0; i < A.in.nraw; i++) {
        int code = A.in.raw[i].code;
        bool down = A.in.raw[i].down;
        if (code == 56 || code == 100) { /* KEY_LEFTALT, KEY_RIGHTALT */
            alt_down = down;
            continue;
        }
        if (!down || code == 29 || code == 97 || code == 42 || code == 54) /* (ctrl and shift alone: no) */
            continue;
        if (code == 103 || code == 108 || code == 105 || code == 106 || code == 102 || code == 107 || code == 104 ||
            code == 109) /* up down left right home end page up/down */
            reader_arrows_at = A.now;
        if (A.in.ctrl && alt_down) {
            if (reader_on() && (code == 106 || code == 105 || code == 28 || code == 96)) { /* right left enter */
                memmove(&A.in.raw[i], &A.in.raw[i + 1], sizeof A.in.raw[0] * (size_t)(A.in.nraw - i - 1));
                A.in.nraw--, i--;
                for (int k = 0; k < A.in.nkeys; k++) /* (not also moving a caret, or pressing Enter in a field) */
                    if (A.in.keys[k] == K_RIGHT || A.in.keys[k] == K_LEFT || A.in.keys[k] == K_ENTER) {
                        memmove(&A.in.keys[k], &A.in.keys[k + 1], sizeof A.in.keys[0] * (size_t)(A.in.nkeys - k - 1));
                        memmove(&A.in.keymod[k], &A.in.keymod[k + 1], (size_t)(A.in.nkeys - k - 1));
                        A.in.nkeys--, k--;
                    }
                if (code == 106)
                    move_stop(1);
                else if (code == 105)
                    move_stop(-1);
                else
                    use_stop();
                continue;
            }
            int ch = code == 19 ? 'r' : code == 17 ? 'w' : code == 30 ? 'a' : code == 32 ? 'd' : 0;
            if (ch) {
                drop_key(ch);
                memmove(&A.in.raw[i], &A.in.raw[i + 1], sizeof A.in.raw[0] * (size_t)(A.in.nraw - i - 1));
                A.in.nraw--, i--;
                if (ch == 'r')
                    reader_set(!reader_on());
                else if (reader_on() && ch == 'w')
                    where_am_i();
                else if (reader_on() && ch == 'a')
                    say(last_said);
                else if (reader_on() && ch == 'd')
                    describe_screen();
                continue;
            }
        }
        hush = true;
    }
    if (hush && reader_on() && voice_speaking() && A.now - said_at > 150)
        voice_hush(); /* any key cuts it off */
}

/* ---- each turn ---- */

static bool design_words(const fm_node *n, char *out, size_t sz) {
    out[0] = 0;
    for (const fm_node *e = n; e && !out[0]; e = e->parent) {
        if (e->kind != FM_ELEM)
            continue;
        const char *app = fm_attr_get(e, "app"), *title = fm_attr_get(e, "title"), *what = fm_attr_get(e, "what");
        if (app && *app) {
            int k = fm_app_index(app);
            snprintf(out, sz, "%s", k >= 0 ? fm_host_app_label(k) : app);
        } else if (e->win >= 0 && e->win < WIN_MAX && A.win[e->win].open) {
            snprintf(out, sz, "%s window%s", desktop_window_title(e->win), A.win[e->win].minimized ? ", minimized" : "");
        } else if (!strcmp(e->tag, "search")) {
            const char *v = fm_attr_get(e, "value");
            snprintf(out, sz, "Search box, %s%s", v && *v ? "says " : "empty", v && *v ? v : "");
        } else if (!strcmp(e->tag, "clock")) {
            char *t = fm_text_of(e);
            snprintf(out, sz, "The time, %s", t ? t : "");
            free(t);
        } else if (title && *title) {
            snprintf(out, sz, "%s", title);
        } else if (!strcmp(e->tag, "button") || !strcmp(e->tag, "item") || !strcmp(e->tag, "text") ||
                   !strcmp(e->tag, "label")) {
            char *t = fm_text_of(e);
            const char *s = t ? t + strspn(t, " \t\r\n") : "";
            if (*s)
                snprintf(out, sz, "%s", s);
            else if (what && !strncmp(what, "open:", 5)) {
                int k = fm_app_index(what + 5);
                snprintf(out, sz, "%s", k >= 0 ? fm_host_app_label(k) : what + 5);
            } else if (what && *what)
                snprintf(out, sz, "%s", what);
            free(t);
        } else if (!strcmp(e->tag, "image") && fm_attr_get(e, "alt")) {
            snprintf(out, sz, "Picture: %s", fm_attr_get(e, "alt"));
        }
        if (!strcmp(e->tag, "desktop"))
            break;
    }
    return out[0] != 0;
}

void reader_tick(void) {
    char *res = NULL;
    if (describing && job_poll(describing, &res)) { /* the companion's fuller words, if they still fit */
        job_free(describing);
        describing = NULL;
        if (res && describing_for == subject_gen)
            say(res);
        free(res);
    }
    if (!reader_on())
        return;
    helper_start();
    helper_poll();
    /* 1. a right-click menu that is up: what is chosen in it */
    char words[600], more[900];
    if (ctx_menu_reading(words, sizeof words)) {
        new_subject(words, words, true);
        return;
    }
    int fw = desktop_focused_window();
    bool app_in_front = fw >= WIN_COUNT && A.win[fw].view;
    /* 2. a window come forward */
    static int front = -2;
    if (fw != front) {
        front = fw;
        if (fw >= 0 && !app_in_front) {
            snprintf(words, sizeof words, "%s window", desktop_window_title(fw));
            new_subject(words, words, true);
        }
    }
    /* 3. what the pointer rests on, after a moment (a control of the OS's own, or the design's) */
    static int px = -1, py = -1;
    static uint64_t rest_at;
    if (A.in.mx != px || A.in.my != py)
        px = A.in.mx, py = A.in.my, rest_at = A.now;
    words[0] = 0;
    if (A.now - rest_at > 350) {
        for (int i = nnotes - 1; i >= 0 && !words[0]; i--) /* (the last drawn is on top) */
            if (pt_in(notes[i].r, px, py))
                words_of_note(&notes[i], words, sizeof words);
        if (!words[0] && A.screen == SCR_DESKTOP && A.ui && A.ui->hovered)
            design_words(A.ui->hovered, words, sizeof words);
        if (words[0])
            new_subject(words, words, true);
    }
    /* 4. a field being typed in, when it gets the keys */
    static char focused_field[200];
    for (int i = 0; i < nnotes; i++)
        if (notes[i].focused) {
            char key[200];
            snprintf(key, sizeof key, "%s|%d|%d", notes[i].label, notes[i].r.x, notes[i].r.y);
            if (strcmp(key, focused_field)) {
                snprintf(focused_field, sizeof focused_field, "%s", key);
                words_of_note(&notes[i], words, sizeof words);
                snprintf(more, sizeof more, "%s (they are typing in it)", words);
                new_subject(words, more, true);
            }
            break;
        }
    /* 5. stopped on something: the companion says more */
    if (reader_full() && subject[0] && !described && A.now - subject_at > 2200 && !voice_speaking()) {
        described = true;
        char ctx[1400];
        snprintf(ctx, sizeof ctx, "They stopped on: %s\nWhat is known of it: %s\nWhere: %s", subject, subject_more,
                 fw >= 0 ? desktop_window_title(fw) : "the desktop");
        describe(ctx, false);
    }
}

void reader_announce(const char *text) {
    if (reader_on() && text && *text)
        say(text);
}

/* ---- the first screens: the offer, said and shown ---- */

void reader_draw(void) {
    if (A.screen == SCR_DESKTOP || A.have_design)
        return;
    static bool offered;
    if (!offered && A.screen != SCR_SPLASH) {
        offered = true;
        if (!reader_on())
            voice_read("Hello. If you'd like me to read the screen out loud, press Control, Alt and R.");
    }
    if (reader_on() || A.screen == SCR_SPLASH)
        return;
    font *f = font_get("Inter", 15);
    const char *t = "Press Ctrl+Alt+R to have the screen read out loud";
    int w = text_width(f, t) + 32, h = font_line_height(f) + 16;
    rect r = {(A.W - w) / 2, A.H - h - 18, w, h};
    fill_round(A.frame, r, h / 2.0f, RGBA(0, 0, 0, 150));
    draw_text(A.frame, f, r.x + 16, r.y + 8, t, RGBA(255, 255, 255, 235));
}

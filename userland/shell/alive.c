#define _GNU_SOURCE
/* What the companion makes of its senses.
 *
 * A short world model for every prompt; a log of notable moments ("the CPU
 * went from 62 to 86°C in two minutes"); the rare times it speaks up on its
 * own; how its orb looks; the PC's lights following the desktop or showing
 * when it thinks; and the panel that shows the person, plainly, everything
 * it can sense, with a switch for each.
 *
 * Speaking up is deliberately hard to earn. Each kind of moment has its own
 * cooldown, all of them share a small hourly budget, and nothing is said
 * while they type, watch something full screen, talk to it, or asked it to
 * be quiet. The words come from the model, so they sound like it. */
#include "alive.h"
#include "voice.h"
#include "fm/fm.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define D (A.d)
#define CONF STATE_DIR "/alive.conf"

struct alive_prefs alive_prefs = {.proactive = true};

/* ---- preferences, kept in /etc/os/alive.conf ---- */

static void prefs_load(void) {
    FILE *f = fopen(CONF, "r");
    if (!f)
        return;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        bool on = atoi(eq + 1) != 0;
        if (!strcmp(line, "proactive")) alive_prefs.proactive = on;
        else if (!strcmp(line, "quiet")) alive_prefs.quiet = on;
        else if (!strcmp(line, "lights.follow")) alive_prefs.lights_follow = on;
        else if (!strcmp(line, "lights.show")) alive_prefs.lights_show = on;
        else if (!strncmp(line, "sense.", 6))
            for (int s = 0; s < SENSE_COUNT; s++)
                if (!strcmp(line + 6, sense_info[s].key))
                    sense_set(s, on);
    }
    fclose(f);
}

void alive_prefs_save(void) {
    FILE *f = fopen(CONF, "w");
    if (!f)
        return;
    fprintf(f, "proactive=%d\nquiet=%d\nlights.follow=%d\nlights.show=%d\n", alive_prefs.proactive, alive_prefs.quiet,
            alive_prefs.lights_follow, alive_prefs.lights_show);
    for (int s = 0; s < SENSE_COUNT; s++)
        fprintf(f, "sense.%s=%d\n", sense_info[s].key, sense_on(s));
    fclose(f);
}

/* ---- notable moments ---- */

#define MAX_EVENTS 16
static struct event {
    uint64_t at;
    time_t wall;
    int level;
    char kind[16];
    char text[220];
    bool spoken; /* said, or no longer worth saying */
} ev[MAX_EVENTS];
static int nev;

void alive_event(int level, const char *kind, const char *fmt, ...) {
    if (nev == MAX_EVENTS) {
        memmove(&ev[0], &ev[1], sizeof ev[0] * (MAX_EVENTS - 1));
        nev--;
    }
    struct event *e = &ev[nev++];
    memset(e, 0, sizeof *e);
    e->at = A.now;
    e->wall = time(NULL);
    e->level = level;
    e->spoken = level < 1;
    snprintf(e->kind, sizeof e->kind, "%s", kind);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->text, sizeof e->text, fmt, ap);
    va_end(ap);
    LOG("alive: event (%d, %s) %s\n", level, kind, e->text);
}

/* ---- watching for moments worth noticing ---- */

#define HIST 32 /* one sample every 10 s: about five minutes */
static struct {
    uint64_t at;
    double cpu, gpu;
} hist[HIST];
static int nhist;

static double temp_ago(uint64_t ms, bool gpu) {
    for (int i = nhist - 1; i >= 0; i--)
        if (A.now - hist[i].at >= ms)
            return gpu ? hist[i].gpu : hist[i].cpu;
    return UNKNOWN;
}

static bool cpu_hot_armed = true, cpu_crit_armed = true, cpu_rise_armed = true, gpu_hot_armed = true;
static bool mem_armed = true, vm_mem_armed = true;

static void watch_temperature(const struct senses *s) {
    double cpu = s->host.up ? s->host.cpu_temp : UNKNOWN;
    double gpu = s->host.up && s->host.ngpu ? s->host.gpu[0].temp : UNKNOWN;
    static uint64_t last_sample;
    if (A.now - last_sample >= 10000) {
        last_sample = A.now;
        if (nhist == HIST)
            memmove(&hist[0], &hist[1], sizeof hist[0] * (HIST - 1)), nhist--;
        hist[nhist++] = (typeof(hist[0])){A.now, cpu, gpu};
    }
    if (!sense_on(SENSE_HARDWARE))
        return;
    if (cpu > -900) {
        double before = temp_ago(120000, false);
        char was[80] = "";
        if (before > -900)
            snprintf(was, sizeof was, " (it was %.0f°C two minutes ago)", before);
        /* Each alarm fires once, then waits for things to cool well below
         * it before it can fire again. */
        if (cpu >= 95 && cpu_crit_armed) {
            alive_event(3, "temp", "their PC's CPU is at %.0f°C, close to its limit%s", cpu, was);
            cpu_crit_armed = cpu_hot_armed = false;
        } else if (cpu >= 85 && cpu_hot_armed) {
            alive_event(2, "temp", "their PC's CPU is running hot: %.0f°C%s", cpu, was);
            cpu_hot_armed = false;
        } else if (before > -900 && cpu - before >= 15 && cpu >= 70 && cpu_rise_armed) {
            alive_event(1, "temp", "their PC's CPU is heating up fast: %.0f°C and rising%s", cpu, was);
            cpu_rise_armed = false;
        }
        if (cpu < 88)
            cpu_crit_armed = true;
        if (cpu < 75)
            cpu_hot_armed = true;
        if (before > -900 && cpu - before < 5)
            cpu_rise_armed = true;
    }
    if (gpu > -900) {
        if (gpu >= 87 && gpu_hot_armed) {
            alive_event(gpu >= 93 ? 3 : 2, "temp", "their GPU (%s) is at %.0f°C", s->host.gpu[0].name, gpu);
            gpu_hot_armed = false;
        }
        if (gpu < 78)
            gpu_hot_armed = true;
    }
}

static void watch_memory(const struct senses *s) {
    if (sense_on(SENSE_HARDWARE) && s->host.up && s->host.mem_total_mb > 0 && s->host.mem_used_mb > -900) {
        double pct = 100 * s->host.mem_used_mb / s->host.mem_total_mb;
        if (pct >= 92 && mem_armed) {
            alive_event(1, "memory", "their PC's memory is nearly full (%.0f%% used)", pct);
            mem_armed = false;
        } else if (pct < 85) {
            mem_armed = true;
        }
    }
    if (sense_on(SENSE_SYSTEM) && s->vm_mem_total_mb > 0) {
        double free_pct = 100 * (s->vm_mem_total_mb - s->vm_mem_used_mb) / s->vm_mem_total_mb;
        if (free_pct < 8 && vm_mem_armed) {
            alive_event(1, "memory", "this computer is almost out of memory (%.0f MB left; %s is the biggest)",
                        s->vm_mem_total_mb - s->vm_mem_used_mb, s->ntop ? s->top[0].name : "nothing in particular");
            vm_mem_armed = false;
        } else if (free_pct > 15) {
            vm_mem_armed = true;
        }
    }
}

static void watch_them(const struct senses *s) {
    if (!sense_on(SENSE_ACTIVITY))
        return;
    bool here = A.now - s->last_input < 2 * 60000;
    static uint64_t session_seen, nudged_at;
    if (s->session_start != session_seen) { /* they had a break: start over */
        session_seen = s->session_start;
        nudged_at = 0;
    }
    uint64_t long_ms = 2 * 3600000ull, again_ms = 90 * 60000ull;
    if (here && A.now - s->session_start >= long_ms && (!nudged_at || A.now - nudged_at >= again_ms)) {
        nudged_at = A.now;
        alive_event(1, "session", "they've been at the computer for %llu hours without a real break",
                    (unsigned long long)((A.now - s->session_start) / 3600000));
    }
    if (!sense_on(SENSE_TIME))
        return;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    static int noted_night = -1;
    if (here && tm.tm_hour >= 1 && tm.tm_hour < 5 && noted_night != tm.tm_yday) {
        noted_night = tm.tm_yday;
        alive_event(1, "late", "it's %d:%02d at night and they're still up", tm.tm_hour, tm.tm_min);
    }
}

static void watch_devices(const struct senses *s) {
    static char ids[HOST_MAX_LIGHTS][64], names[HOST_MAX_LIGHTS][140];
    static int nids = -1;
    static bool was_up;
    if (s->host.up != was_up) {
        if (s->host.up)
            alive_event(0, "host", "you can feel their real PC now (%s)", s->host.name[0] ? s->host.name : "it answered");
        else if (nids >= 0)
            alive_event(0, "host", "you lost touch with their real PC: %s", s->host.why_down);
        was_up = s->host.up;
    }
    if (!s->host.up || !sense_on(SENSE_LIGHTS))
        return;
    if (nids >= 0) {
        for (int i = 0; i < s->host.nlight; i++) {
            bool old = false;
            for (int j = 0; j < nids && !old; j++)
                old = !strcmp(ids[j], s->host.light[i].id);
            if (!old)
                alive_event(1, "device", "a new %s%s%s appeared on their PC: %s", s->host.light[i].vendor,
                            s->host.light[i].vendor[0] ? " " : "", s->host.light[i].kind, s->host.light[i].name);
        }
        for (int j = 0; j < nids; j++) {
            bool still = false;
            for (int i = 0; i < s->host.nlight && !still; i++)
                still = !strcmp(ids[j], s->host.light[i].id);
            if (!still)
                alive_event(1, "device", "%s was unplugged or switched off", names[j]);
        }
    }
    nids = s->host.nlight;
    for (int i = 0; i < nids; i++) {
        snprintf(ids[i], sizeof ids[i], "%s", s->host.light[i].id);
        snprintf(names[i], sizeof names[i], "%s%s%s %s", s->host.light[i].vendor, s->host.light[i].vendor[0] ? " " : "",
                 s->host.light[i].kind, s->host.light[i].name);
    }
}

static void watch_network(const struct senses *s) {
    static int was = -1;
    static uint64_t down_at;
    if (!sense_on(SENSE_NETWORK) || !s->net_known)
        return;
    if (was == 1 && !s->net_up) {
        down_at = A.now;
        alive_event(2, "network", "the internet just dropped");
    } else if (was == 0 && s->net_up) {
        uint64_t m = (A.now - down_at) / 60000;
        alive_event(m >= 2 ? 1 : 0, "network", "the internet is back (it was down for %llu minute%s)",
                    (unsigned long long)(m ? m : 1), m == 1 || !m ? "" : "s");
    }
    was = s->net_up;
}

/* ---- speaking up ---- */

static uint64_t spoken_at[8]; /* the budget: when it last spoke up */
static int nspoken;

static uint64_t cooldown(const char *kind) {
    static const struct {
        const char *kind;
        uint64_t ms;
    } c[] = {{"temp", 20 * 60000}, {"memory", 30 * 60000}, {"session", 90 * 60000}, {"late", 6 * 3600000ull},
             {"device", 60000},    {"task", 0},            {"network", 10 * 60000}};
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++)
        if (!strcmp(c[i].kind, kind))
            return c[i].ms;
    return 15 * 60000;
}

/* When it last spoke about each kind of moment. */
static uint64_t kind_said[16];
static char kind_said_name[16][16];

static uint64_t *said_slot(const char *kind) {
    for (int i = 0; i < 16; i++)
        if (!strcmp(kind_said_name[i], kind) || !kind_said_name[i][0]) {
            snprintf(kind_said_name[i], sizeof kind_said_name[i], "%s", kind);
            return &kind_said[i];
        }
    return &kind_said[15];
}

static bool convo_busy(void) {
    struct conversation *c = &A.companion;
    if (c->pending)
        return true;
    /* In the middle of talking with them: wait for a lull. */
    for (int i = c->n - 1; i >= 0 && i >= c->n - 2; i--)
        if (!strcmp(c->msgs[i].role, "user") && A.now - c->pending_since < 90000)
            return true;
    return false;
}

static void maybe_speak(void) {
    /* What is still worth saying? Old news isn't. */
    int pick[3], np = 0, top = 0;
    for (int i = 0; i < nev; i++) {
        struct event *e = &ev[i];
        if (e->spoken)
            continue;
        uint64_t stale = !strcmp(e->kind, "task") ? 20 * 60000 : 10 * 60000;
        if (A.now - e->at > stale) {
            e->spoken = true;
            continue;
        }
        uint64_t *said = said_slot(e->kind);
        if (*said && A.now - *said < cooldown(e->kind) && e->level < 3) {
            e->spoken = true; /* said something like it recently */
            continue;
        }
        if (np < 3)
            pick[np++] = i;
        if (e->level > top)
            top = e->level;
    }
    if (!np)
        return;
    bool urgent = top >= 3;
    if (A.screen != SCR_DESKTOP || A.now - A.screen_since < 25000)
        return;
    if (!alive_prefs.proactive && !urgent)
        return;
    /* They asked for quiet: only danger gets through, and silently. */
    if (alive_prefs.quiet) {
        if (urgent) {
            for (int k = 0; k < np; k++)
                if (ev[pick[k]].level >= 3) {
                    char body[240];
                    snprintf(body, sizeof body, "%s.", ev[pick[k]].text);
                    body[0] = (char)toupper((unsigned char)body[0]);
                    if (notify)
                        notify("Your PC", body, D.ai_name);
                    else
                        toast("%s", body);
                    ev[pick[k]].spoken = true;
                }
        }
        return;
    }
    /* Never over them: not while typing, watching something full screen,
     * talking with it, or with a question or words already on screen. */
    if (!urgent && (senses_typing() || senses_fullscreen()))
        return;
    if (convo_busy() || actions_confirming() || voice_listening() || voice_hearing() || voice_speaking())
        return;
    if (A.caption[0] && A.now < A.caption_until)
        return;
    /* A small budget, shared by everything: a few times an hour at most. */
    int last_hour = 0;
    uint64_t last = 0;
    for (int i = 0; i < nspoken; i++) {
        if (A.now - spoken_at[i] < 3600000)
            last_hour++;
        if (spoken_at[i] > last)
            last = spoken_at[i];
    }
    bool reserved = strcasestr(A.prof.autonomy, "quiet") || strcasestr(A.prof.autonomy, "don't") ||
                    strcasestr(A.prof.autonomy, "leave me");
    if (!urgent && (last_hour >= (reserved ? 2 : 3) || (last && A.now - last < 6 * 60000) || (reserved && top < 2)))
        return;

    char note[1400];
    size_t o = (size_t)snprintf(note, sizeof note,
                                "(Not something they said: you noticed this on your own, just now. ");
    for (int k = 0; k < np; k++) {
        struct event *e = &ev[pick[k]];
        o += (size_t)snprintf(note + o, o < sizeof note ? sizeof note - o : 0, "%s%s", k ? "; also, " : "", e->text);
        e->spoken = true;
        *said_slot(e->kind) = A.now;
    }
    snprintf(note + o, o < sizeof note ? sizeof note - o : 0,
             ". Mention it to them in one short, natural sentence, the way you would; offer one small, concrete thing "
             "only if it would really help. %sNo tags, no question unless you need one.)",
             urgent ? "It matters: be clear, not alarming. " : "Keep it light; don't lecture or nag. ");
    if (nspoken == 8)
        memmove(&spoken_at[0], &spoken_at[1], sizeof spoken_at[0] * 7), nspoken--;
    spoken_at[nspoken++] = A.now;
    LOG("alive: speaking up: %s\n", note);
    companion_prompt(note, 200);
}

/* ---- the PC's lights, as part of the room ---- */

static bool lights_touched; /* this session changed the lights: hand them back on exit */

static void hex(char out[8], color c) { snprintf(out, 8, "#%02x%02x%02x", C_R(c), C_G(c), C_B(c)); }

static bool has_light(const struct senses *s, const char *kind) {
    for (int i = 0; i < s->host.nlight; i++)
        if (s->host.light[i].controllable && (!kind || !strcmp(s->host.light[i].kind, kind)))
            return true;
    return false;
}

static void ambient_lights(const struct senses *s) {
    static color sent_a, sent_b;
    static bool following, showing;
    static uint64_t last_send, show_since;
    bool usable = s->host.up && sense_on(SENSE_LIGHTS) && has_light(s, NULL);
    char body[160], a[8], b[8];
    hex(a, D.accent), hex(b, D.accent2);
    /* Lights follow the desktop: its accent everywhere, the second accent
     * on the fans, changing when the space is redesigned. */
    if (alive_prefs.lights_follow && usable) {
        if ((!following || D.accent != sent_a || D.accent2 != sent_b) && A.now - last_send > 2000) {
            snprintf(body, sizeof body, "{\"target\":\"all\",\"color\":\"%s\"}", a);
            senses_lights_post("/lights", body);
            if (has_light(s, "fan") && D.accent2 != D.accent) {
                snprintf(body, sizeof body, "{\"target\":\"fan\",\"color\":\"%s\"}", b);
                senses_lights_post("/lights", body);
            }
            sent_a = D.accent, sent_b = D.accent2;
            following = lights_touched = true;
            last_send = A.now;
            actions_log("lights follow the desktop: %s and %s", a, b);
        }
    } else if (following) {
        following = false;
        senses_lights_post("/lights/restore", "{}");
        actions_log("lights handed back to their own apps");
    }
    /* Lights show me: a soft pulse on the keyboard while it thinks or talks. */
    bool busy = A.companion.pending || voice_speaking() || voice_listening() || voice_hearing();
    if (alive_prefs.lights_show && usable && has_light(s, "keyboard")) {
        if (busy && !showing) {
            snprintf(body, sizeof body, "{\"target\":\"keyboard\",\"effect\":\"pulse\",\"color\":\"%s\"}", a);
            senses_lights_post("/lights", body);
            showing = lights_touched = true;
            show_since = A.now;
        } else if (!busy && showing && A.now - show_since > 1200) {
            if (following)
                snprintf(body, sizeof body, "{\"target\":\"keyboard\",\"color\":\"%s\"}", a);
            else
                snprintf(body, sizeof body, "{\"target\":\"keyboard\",\"effect\":\"restore\"}");
            senses_lights_post("/lights", body);
            showing = false;
        }
    } else if (showing) {
        senses_lights_post("/lights", following ? "{\"target\":\"keyboard\",\"effect\":\"static\"}"
                                                : "{\"target\":\"keyboard\",\"effect\":\"restore\"}");
        showing = false;
    }
}

void alive_shutdown(void) {
    if (lights_touched || alive_prefs.lights_follow || alive_prefs.lights_show)
        senses_lights_restore_now();
}

/* ---- the orb: how it feels, at a glance ---- */

void alive_orb(color *a, color *b, bool *active) {
    const struct senses *s = senses_now();
    /* Warmer as the PC's CPU heats up: amber from 60°C, ember by 90°C. */
    if (s->host.up && sense_on(SENSE_HARDWARE) && s->host.cpu_temp > -900) {
        float warm = (float)((s->host.cpu_temp - 60) / 30);
        warm = warm < 0 ? 0 : warm > 1 ? 1 : warm;
        *a = color_mix(*a, RGBA(255, 120, 48, 255), 0.6f * warm);
        *b = color_mix(*b, RGBA(255, 196, 120, 255), 0.45f * warm);
    }
    /* Attentive while it listens; hushed when asked to be quiet. */
    if (voice_listening() || voice_hearing() || voice_speaking())
        *active = true;
    if (alive_prefs.quiet) {
        color grey = RGBA(150, 150, 160, 255);
        *a = color_mix(*a, grey, 0.35f);
        *b = color_mix(*b, grey, 0.35f);
    }
}

/* ---- what goes into its prompt ---- */

const char *alive_prompt(void) {
    static char buf[6000];
    char sensed[2600];
    senses_write(sensed, sizeof sensed);
    size_t o = (size_t)snprintf(buf, sizeof buf,
                                "\n\nWhat you can sense right now (live; this is all of it, so never guess a reading "
                                "that isn't here, and if they ask about one, say plainly you can't feel it and why):\n%s",
                                sensed);
    int shown = 0;
    for (int i = nev - 1; i >= 0 && shown < 6; i--) {
        if (A.now - ev[i].at > 6 * 3600000ull)
            break;
        char when[16];
        struct tm tm;
        localtime_r(&ev[i].wall, &tm);
        strftime(when, sizeof when, "%H:%M", &tm);
        o += (size_t)snprintf(buf + o, o < sizeof buf ? sizeof buf - o : 0, "%s- %s %s\n", shown ? "" : "Lately:\n", when,
                              ev[i].text);
        shown++;
    }
    snprintf(buf + o, o < sizeof buf ? sizeof buf - o : 0,
             "You also have hands beyond the desktop. Use them only when they ask, or clearly would want it, and "
             "mention what you did in plain words:\n"
             "[[light:all #ff8800]] [[light:keyboard breathe #00aaff]] [[light:fan pulse #ff0000]] [[light:off]] "
             "[[light:restore]] change the RGB lights on their PC (target: all, a kind such as keyboard, mouse, fan, "
             "or a device's name; effects: static, breathe, pulse, off; restore gives them back to their own apps).\n"
             "[[set:volume 40]] [[set:dark on]] [[set:voice off]] [[set:text larger]] [[set:contrast on]] "
             "[[set:motion less]] [[set:quiet on]] (you stop speaking up on your own) [[set:lights-follow on]] (the "
             "lights take the desktop's colours) [[set:lights-show on]] (the keyboard glows while you think or talk) "
             "[[set:sense hardware off]] (any sense by name).\n"
             "[[notify:Title|text]] posts a notification. [[open:/root/notes.txt]] or [[open:/root/Documents]] opens "
             "a file or folder; [[open:senses]] shows them everything you can sense, with a switch for each.\n"
             "[[look:processes]] [[look:hardware]] [[look:lights]] [[look:windows]] [[look:screen]] [[look:network]] "
             "[[look:notifications]] [[look:files /root]] [[look:file /root/notes.txt]] [[look:actions]] ask this "
             "computer something read-only. When you need one, reply with just the look tag; the answer comes back "
             "at once and you continue (three looks at most).\n"
             "[[run:command]] runs a shell command (busybox) after they confirm it on screen; nothing runs without "
             "their yes, so never say it ran until you're told. [[undo]] reverses your last change to the lights or "
             "settings.\n");
    return buf;
}

const char *alive_onboarding_note(void) {
    static char buf[600];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char when[48];
    strftime(when, sizeof when, "%A, %H:%M", &tm);
    snprintf(buf, sizeof buf,
             "\n\nIt is %s right now. Once their space exists you will be able to sense this computer (and, if "
             "their PC runs its small senses helper, its temperatures, fans and RGB lights) and act on some of it. "
             "Not yet: in this conversation you can't feel or change anything, so don't claim to.",
             sense_on(SENSE_TIME) ? when : "(time hidden)");
    return buf;
}

/* ---- the "what I can sense" panel ---- */

static bool panel_open;
static input_state held; /* the real input, while an overlay holds it */
static bool holding;

void alive_panel_open(bool open) { panel_open = open; }

static bool pill(rect r, bool on, font *f, const char *label) {
    bool hover = ui_hover(r);
    fill_round(A.frame, r, r.h / 2.0f, on ? color_alpha(D.accent, hover ? 1 : 0.9f) : color_alpha(D.surface_text, hover ? 0.18f : 0.1f));
    float kx = on ? r.x + r.w - r.h / 2.0f : r.x + r.h / 2.0f;
    fill_circle(A.frame, kx, r.y + r.h / 2.0f, r.h / 2.0f - 3, on ? readable_on(D.accent) : color_alpha(D.surface_text, 0.6f));
    if (label)
        draw_text_centered(A.frame, f, (rect){r.x - 64, r.y, 60, r.h}, label, D.muted);
    return hover && A.in.clicked;
}

static void reading(enum sense s, char *out, size_t n) {
    const struct senses *v = senses_now();
    char a[32], b[32];
    switch (s) {
    case SENSE_HARDWARE:
        if (!v->host.up)
            snprintf(out, n, "Not connected: %s.", v->host.why_down[0] ? v->host.why_down : "the PC hasn't answered yet");
        else {
            size_t o = (size_t)snprintf(out, n, "%s:", v->host.name[0] ? v->host.name : "Your PC");
            if (v->host.cpu_temp > -900)
                o += (size_t)snprintf(out + o, n - o, " CPU %.0f°C", v->host.cpu_temp);
            else
                o += (size_t)snprintf(out + o, n - o, " CPU temperature unknown");
            if (v->host.ngpu && v->host.gpu[0].temp > -900 && o < n)
                o += (size_t)snprintf(out + o, n - o, ", GPU %.0f°C", v->host.gpu[0].temp);
            if (v->host.nfan && o < n)
                snprintf(out + o, n - o, ", %d fan%s", v->host.nfan, v->host.nfan > 1 ? "s" : "");
        }
        break;
    case SENSE_LIGHTS:
        if (!v->host.up)
            snprintf(out, n, "Can't see them until the PC answers.");
        else
            snprintf(out, n, "%d lit device%s found.", v->host.nlight, v->host.nlight == 1 ? "" : "s");
        break;
    case SENSE_SYSTEM:
        snprintf(a, sizeof a, "%.1f", v->vm_mem_used_mb / 1024), snprintf(b, sizeof b, "%.1f", v->vm_mem_total_mb / 1024);
        snprintf(out, n, "CPU %.0f%%, memory %s of %s GB, %d programs.", v->vm_cpu, a, b, v->nprocs);
        break;
    case SENSE_WINDOWS: {
        int nw = fm_host_window_count(), front = -1;
        for (int i = 0; i < nw; i++)
            if (fm_host_window_focused(i))
                front = i;
        snprintf(out, n, "%d window%s open%s%s%s.", nw, nw == 1 ? "" : "s", front >= 0 ? ", " : "",
                 front >= 0 ? fm_host_window_title(front) : "", front >= 0 ? " in front" : "");
        break;
    }
    case SENSE_ACTIVITY: {
        uint64_t m = (A.now - v->session_start) / 60000;
        snprintf(out, n, "%s, for %llu h %llu min.", senses_typing() ? "Typing" : "Here", (unsigned long long)(m / 60),
                 (unsigned long long)(m % 60));
        break;
    }
    case SENSE_NETWORK:
        snprintf(out, n, "%s", !v->net_known ? "Checking..." : v->net_up ? "Online." : "Offline.");
        break;
    case SENSE_TIME: {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(out, n, "%H:%M, %A.", &tm);
        break;
    }
    case SENSE_FILES: snprintf(out, n, "Only the file or folder you ask about."); break;
    default: out[0] = 0;
    }
}

int alive_panel(rect r) {
    font *h = font_get(D.font_heading, D.heading_px * 0.55f), *f = font_get(D.font_body, D.body_px),
         *small = font_get(D.font_body, D.body_px - 3);
    int lh = font_line_height(f), sl = font_line_height(small), y = r.y;
    char line[300];
    snprintf(line, sizeof line, "What %s can sense", D.ai_name[0] ? D.ai_name : "your companion");
    draw_text(A.frame, h, r.x, y, line, D.surface_text);
    y += (int)(D.heading_px * 0.55f * 1.5f);
    y += draw_text_wrapped(A.frame, small, (rect){r.x, y, r.w, 80},
                           "Live, and only on this computer. Switch off anything you'd rather it didn't notice; it will "
                           "say so instead of guessing.",
                           D.muted, 1.3f) + 12;
    bool changed = false;
    for (int s = 0; s < SENSE_COUNT; s++) {
        int rowh = lh + sl + 14;
        bool on = sense_on(s);
        draw_text(A.frame, f, r.x, y + 4, sense_info[s].name, on ? D.surface_text : D.muted);
        if (on)
            reading(s, line, sizeof line);
        else
            snprintf(line, sizeof line, "Off. It can't see %s.", sense_info[s].what);
        draw_text(A.frame, small, r.x, y + 6 + lh, line, D.muted);
        if (pill((rect){r.x + r.w - 56, y + (rowh - 28) / 2, 52, 28}, on, small, NULL)) {
            sense_set(s, !on);
            changed = true;
        }
        y += rowh;
    }
    y += 10;
    draw_text(A.frame, h, r.x, y, "How it behaves", D.surface_text);
    y += (int)(D.heading_px * 0.55f * 1.5f);
    struct {
        const char *name, *what;
        bool *v;
    } opts[] = {
        {"Speak up on its own", "Rarely: heat, a long day, a finished task.", &alive_prefs.proactive},
        {"Be quiet", "Nothing unprompted, unless something's about to go wrong.", &alive_prefs.quiet},
        {"Lights follow the desktop", "Your RGB takes this design's colours.", &alive_prefs.lights_follow},
        {"Lights show when it thinks", "A soft pulse on the keyboard while it thinks or talks.", &alive_prefs.lights_show},
    };
    for (size_t i = 0; i < sizeof opts / sizeof opts[0]; i++) {
        int rowh = lh + sl + 14;
        draw_text(A.frame, f, r.x, y + 4, opts[i].name, D.surface_text);
        draw_text(A.frame, small, r.x, y + 6 + lh, opts[i].what, D.muted);
        if (pill((rect){r.x + r.w - 56, y + (rowh - 28) / 2, 52, 28}, *opts[i].v, small, NULL)) {
            *opts[i].v = !*opts[i].v;
            changed = true;
            actions_log("%s: %s (from the senses panel)", opts[i].name, *opts[i].v ? "on" : "off");
        }
        y += rowh;
    }
    y += 8;
    int bh = lh + 14;
    if (ui_button((rect){r.x, y, 300, bh}, "Give the lights back to their apps", small, color_alpha(D.surface_text, 0.12f),
                  D.surface_text, bh / 2.0f, senses_now()->host.up)) {
        alive_prefs.lights_follow = alive_prefs.lights_show = false;
        senses_lights_post("/lights/restore", "{}");
        actions_log("lights handed back to their own apps");
        changed = true;
    }
    y += bh + 16;
    char recent[3][160];
    int nr = actions_recent(recent, 3);
    if (nr) {
        draw_text(A.frame, f, r.x, y, "What it did lately", D.surface_text);
        y += lh + 6;
        for (int i = 0; i < nr; i++) {
            y += draw_text_wrapped(A.frame, small, (rect){r.x, y, r.w, 60}, recent[i], D.muted, 1.25f) + 4;
        }
        if (ui_button((rect){r.x, y + 6, 180, bh}, "Undo the last one", small, color_alpha(D.surface_text, 0.12f),
                      D.surface_text, bh / 2.0f, true))
            actions_undo();
        y += bh + 12;
    }
    if (changed)
        alive_prefs_save();
    return y - r.y;
}

static rect panel_rect(void) {
    int w = 640, hgt = A.H - 120 < 860 ? A.H - 120 : 860;
    return (rect){(A.W - w) / 2, (A.H - hgt) / 2, w, hgt};
}

bool alive_host_action(const char *a) {
    if (!strcasecmp(a, "senses") || !strcasecmp(a, "open:senses")) {
        panel_open = true;
        return true;
    }
    if (!strncasecmp(a, "quiet:", 6)) {
        if (!strcasecmp(a + 6, "toggle"))
            alive_prefs.quiet = !alive_prefs.quiet;
        else
            alive_prefs.quiet = !strcasecmp(a + 6, "on");
        alive_prefs_save();
        toast("%s", alive_prefs.quiet ? "Quiet: it won't speak up on its own." : "It may speak up again, rarely.");
        return true;
    }
    if (!strncasecmp(a, "lights:", 7)) {
        toast("%s", actions_light(a + 7, true));
        return true;
    }
    return false;
}

/* ---- every frame ---- */

void alive_tick(void) {
    static bool loaded;
    if (!loaded) {
        loaded = true;
        prefs_load();
        atexit(alive_shutdown);
    }
    if (holding) { /* last frame's overlay didn't hand the input back */
        A.in.mx = held.mx, A.in.my = held.my;
        holding = false;
    }
    senses_tick();
    actions_tick();
    const struct senses *s = senses_now();
    static uint64_t last;
    if (A.now - last >= 1000) {
        last = A.now;
        watch_temperature(s);
        watch_memory(s);
        watch_them(s);
        watch_devices(s);
        watch_network(s);
        maybe_speak();
        ambient_lights(s);
    }
    /* An open panel or a question on screen holds the pointer and keys:
     * nothing underneath reacts until it's answered. */
    if (panel_open || actions_confirming()) {
        held = A.in;
        holding = true;
        A.in.clicked = A.in.pressed = A.in.right_clicked = A.in.down = false;
        A.in.wheel = 0;
        A.in.nkeys = A.in.nraw = 0;
        A.in.mx = A.in.my = -10000;
    }
}

void alive_draw(void) {
    if (!holding)
        return;
    input_state rest = A.in;
    A.in = held;
    if (panel_open) {
        rect r = panel_rect();
        fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(D.bg, 0.35f));
        shadow_round(A.frame, (rect){r.x, r.y + 14, r.w, r.h}, D.radius + 6, 46, RGBA(0, 0, 0, 90));
        blur_fast(A.frame, r, D.radius + 6, 14);
        color top = color_mix(D.surface, RGBA(255, 255, 255, 255), color_luma(D.surface) < 0.3f ? 0.06f : 0.2f);
        fill_round_vgradient(A.frame, r, D.radius + 6, color_alpha(top, 0.96f), color_alpha(D.surface, 0.96f));
        stroke_round(A.frame, r, D.radius + 6, 1, RGBA(255, 255, 255, 50));
        static int scroll;
        if (ui_hover(r) && A.in.wheel)
            scroll -= A.in.wheel * 40;
        if (scroll < 0)
            scroll = 0;
        rect body = rect_inset(r, 28);
        canvas_clip(A.frame, body);
        int used = alive_panel((rect){body.x, body.y - scroll, body.w - 8, 4000});
        canvas_unclip(A.frame);
        if (scroll > used - body.h)
            scroll = used > body.h ? used - body.h : 0;
        rect close = {r.x + r.w - 52, r.y + 16, 36, 36};
        if (ui_button(close, "x", font_get(D.font_body, D.body_px), color_alpha(D.surface_text, 0.1f), D.surface_text, 18, true))
            panel_open = false;
        if (A.in.clicked && !ui_hover(r))
            panel_open = false;
        for (int i = 0; i < A.in.nkeys; i++)
            if (A.in.keys[i] == K_ESC)
                panel_open = false;
        static uint64_t logged;
        if (A.now - logged > 1000) {
            logged = A.now;
            LOGRECT("senses-panel", r);
        }
    }
    actions_draw();
    A.in = rest;
    A.in.mx = held.mx, A.in.my = held.my; /* the pointer stays where it really is */
    holding = false;
}

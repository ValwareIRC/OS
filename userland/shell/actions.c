#define _GNU_SOURCE
/* What the companion can do beyond the desktop, from tags in its replies:
 *
 *   [[light:TARGET COLOR|EFFECT]]  the RGB lights on their PC
 *   [[set:THING VALUE]]            volume, dark, voice, comfort, quiet, senses
 *   [[notify:Title|text]]          a notification
 *   [[open:/path]] [[open:senses]] a file, a folder, or the senses panel
 *   [[look:WHAT]]                  a read-only question to the system; the
 *                                  answer goes straight back to the model
 *                                  (at most three in a row)
 *   [[run:command]]                a shell command, only after they say yes
 *                                  on screen; never run on its own
 *   [[write:~/path]]...[[/write]]  a file in their home, written as given:
 *                                  never a hidden one (no way to run
 *                                  code); a file it replaces is kept for
 *                                  [[undo]]
 *   [[undo]]                       reverses its last change
 *
 * Acting tags only count in answer to something the person said, never in
 * words it volunteered on its own. Every change to a device or a setting is
 * written to /etc/os/actions.log and, where it can be, undone. */
#include "alive.h"
#include "settings.h"
#include "web.h"
#include "emblem.h"
#include "voice.h"
#include "fm/fm.h"
#include "../lib/json.h"

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define D (A.d)
#define LOG_FILE STATE_DIR "/actions.log"
#define LOOK_MARK "(What you looked up"
#define MAX_LOOKS 30

static void web_tick(void);

/* ---- the log ---- */

static char recent[8][160];
static int nrecent;

void actions_log(const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char when[24];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        fprintf(f, "%s  %s\n", when, line);
        fclose(f);
    }
    if (nrecent == 8)
        memmove(&recent[0], &recent[1], sizeof recent[0] * 7), nrecent--;
    strftime(when, sizeof when, "%H:%M", &tm);
    snprintf(recent[nrecent++], sizeof recent[0], "%s  %s", when, line);
    LOG("action: %s\n", line);
}

int actions_recent(char out[][160], int max) {
    int k = 0;
    for (int i = nrecent - 1; i >= 0 && k < max; i--)
        snprintf(out[k++], 160, "%s", recent[i]);
    return k;
}

/* ---- undo: the last few changes, and how to take each back ---- */

enum undo_kind { U_LIGHT, U_VOLUME, U_DARK, U_VOICE, U_TEXT, U_CONTRAST, U_MOTION, U_PREF, U_SENSE, U_FILE };
static struct undo {
    enum undo_kind kind;
    char what[300]; /* light target, pref name, a file's path */
    float prev;     /* for a file: 1 if it was there before (kept as STATE_DIR/undo-file) */
} undo[8];
static int nundo;

static void push_undo(enum undo_kind k, const char *what, float prev) {
    if (nundo == 8)
        memmove(&undo[0], &undo[1], sizeof undo[0] * 7), nundo--;
    undo[nundo++] = (struct undo){k, "", prev};
    snprintf(undo[nundo - 1].what, sizeof undo[0].what, "%s", what ? what : "");
}

/* ---- lights ---- */

static const struct {
    const char *name, *hex;
} named[] = {{"red", "#ff1a1a"},    {"orange", "#ff8800"}, {"amber", "#ffb000"}, {"yellow", "#ffdd00"},
             {"green", "#22ff55"},  {"teal", "#00c8b4"},   {"cyan", "#00e5ff"},  {"blue", "#1f5bff"},
             {"purple", "#8a2be2"}, {"violet", "#9b5cff"}, {"pink", "#ff4fa3"},  {"magenta", "#ff00cc"},
             {"white", "#ffffff"},  {"warm", "#ffc580"}};

static const char *kind_of(const char *w) {
    static const struct {
        const char *word, *kind;
    } k[] = {{"keyboard", "keyboard"}, {"keyboards", "keyboard"}, {"mouse", "mouse"},     {"mice", "mouse"},
             {"mouses", "mouse"},      {"fan", "fan"},            {"fans", "fan"},         {"motherboard", "motherboard"},
             {"board", "motherboard"}, {"ram", "ram"},            {"memory", "ram"},       {"ssd", "ssd"},
             {"strip", "strip"},       {"strips", "strip"},       {"case", "strip"},       {"headset", "headset"},
             {"cooler", "cooler"},     {"gpu", "gpu"},            {"graphics", "gpu"},     {"other", "other"}};
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
        if (!strcasecmp(w, k[i].word))
            return k[i].kind;
    return NULL;
}

const char *actions_light(const char *spec, bool log) {
    static char said[200];
    const struct senses *s = senses_now();
    if (!sense_on(SENSE_LIGHTS)) {
        snprintf(said, sizeof said, "They've switched off the lights sense, so I can't touch the lights.");
        return said;
    }
    if (!s->host.up) {
        snprintf(said, sizeof said, "I can't reach your PC's lights right now: %s.",
                 s->host.why_down[0] ? s->host.why_down : "it isn't connected");
        if (log)
            actions_log("lights %s: not done (%s)", spec, s->host.why_down);
        return said;
    }
    char buf[200], target[96] = "", colour[8] = "", effect[12] = "";
    snprintf(buf, sizeof buf, "%s", spec);
    for (char *tok = strtok(buf, " |,"); tok; tok = strtok(NULL, " |,")) {
        if (tok[0] == '#' && strlen(tok) == 7) {
            snprintf(colour, sizeof colour, "%s", tok);
            continue;
        }
        bool done = false;
        for (size_t i = 0; i < sizeof named / sizeof named[0] && !done; i++)
            if (!strcasecmp(tok, named[i].name))
                snprintf(colour, sizeof colour, "%s", named[i].hex), done = true;
        static const char *const effects[] = {"static", "breathe", "pulse", "off", "restore", NULL};
        for (int i = 0; effects[i] && !done; i++)
            if (!strcasecmp(tok, effects[i]))
                snprintf(effect, sizeof effect, "%s", effects[i]), done = true;
        if (done)
            continue;
        const char *k = kind_of(tok);
        if (!strcasecmp(tok, "all") || !strcasecmp(tok, "everything"))
            snprintf(target, sizeof target, "all");
        else if (k)
            snprintf(target, sizeof target, "%s", k);
        else { /* a device, by id or part of its name */
            for (int i = 0; i < s->host.nlight; i++)
                if (!strcasecmp(tok, s->host.light[i].id) || strcasestr(s->host.light[i].name, tok)) {
                    snprintf(target, sizeof target, "%s", s->host.light[i].id);
                    break;
                }
        }
    }
    if (!target[0])
        snprintf(target, sizeof target, "all");
    if (!colour[0] && !effect[0]) {
        snprintf(said, sizeof said, "I didn't understand which colour or effect you meant (\"%s\").", spec);
        return said;
    }
    sbuf b;
    sb_init(&b);
    if (!strcmp(effect, "restore") && !strcmp(target, "all")) {
        senses_lights_post("/lights/restore", "{}");
        snprintf(said, sizeof said, "Your lights are back with their own apps.");
    } else {
        sb_add(&b, "{\"target\":");
        sb_json_str(&b, target);
        if (colour[0])
            sb_printf(&b, ",\"color\":\"%s\"", colour);
        if (effect[0])
            sb_printf(&b, ",\"effect\":\"%s\"", effect);
        sb_add(&b, "}");
        senses_lights_post("/lights", b.p);
        snprintf(said, sizeof said, "Asked your PC to set %s%s%s%s%s.", !strcmp(target, "all") ? "all the lights" : target,
                 colour[0] ? " to " : "", colour, effect[0] && strcmp(effect, "static") ? ", " : "",
                 effect[0] && strcmp(effect, "static") ? effect : "");
        if (strcmp(effect, "restore"))
            push_undo(U_LIGHT, target, 0);
    }
    sb_free(&b);
    if (log)
        actions_log("lights: %s%s%s%s%s", target, colour[0] ? " " : "", colour, effect[0] ? " " : "", effect);
    return said;
}

/* ---- settings ---- */

#define MIXER_READ_VOLUME 0x80044d00u
#define MIXER_WRITE_VOLUME 0xc0044d00u

static int volume_get(void) {
    if (settings_get_volume)
        return settings_get_volume();
    int fd = open("/dev/mixer", O_RDONLY), v = -1;
    if (fd >= 0) {
        if (ioctl(fd, MIXER_READ_VOLUME, &v) < 0)
            v = -1;
        close(fd);
    }
    return v < 0 ? -1 : v & 0xff;
}

static bool volume_set(int pct) {
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    if (settings_set_volume) {
        settings_set_volume(pct);
        return true;
    }
    int fd = open("/dev/mixer", O_RDWR), v = pct | pct << 8;
    bool ok = fd >= 0 && ioctl(fd, MIXER_WRITE_VOLUME, &v) >= 0;
    if (fd >= 0)
        close(fd);
    return ok;
}

static bool word_on(const char *v) {
    return !strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "1") ||
           !strcasecmp(v, "larger") || !strcasecmp(v, "bigger") || !strcasecmp(v, "less") || !strcasecmp(v, "dark");
}

static void comfort_changed(void) {
    portrait_save(&A.prof, STATE_DIR "/portrait.txt");
    if (A.ui)
        A.ui->changed = true;
}

static bool *pref_named(const char *name) {
    if (!strcasecmp(name, "quiet")) return &alive_prefs.quiet;
    if (!strcasecmp(name, "proactive") || !strcasecmp(name, "speak-up")) return &alive_prefs.proactive;
    if (!strcasecmp(name, "lights-follow") || !strcasecmp(name, "lights_follow")) return &alive_prefs.lights_follow;
    if (!strcasecmp(name, "lights-show") || !strcasecmp(name, "lights_show")) return &alive_prefs.lights_show;
    return NULL;
}

static int sense_named(const char *name) {
    for (int s = 0; s < SENSE_COUNT; s++)
        if (!strcasecmp(name, sense_info[s].key))
            return s;
    return -1;
}

/* Returns what happened, plainly. */
static const char *do_set(const char *spec) {
    static char said[160];
    char thing[32] = "", val[64] = "", extra[32] = "";
    sscanf(spec, "%31s %63s %31s", thing, val, extra);
    if (!strcasecmp(thing, "reflex") || !strcasecmp(thing, "reflexes")) { /* how its reflexes decide */
        const char *e = !strcasecmp(val, "llm") || !strcasecmp(val, "model") ? "llm"
                      : !strcasecmp(val, "rules") || !strcasecmp(val, "off") ? "rules" : "jev";
        settings_set("reflex.engine", e);
        snprintf(said, sizeof said, "reflexes: %s", e);
        return said;
    }
    if (!strcasecmp(thing, "reflex-model")) {
        settings_set("reflex.model", val);
        snprintf(said, sizeof said, "reflex model: %s", val);
        return said;
    }
    if (!strcasecmp(thing, "volume")) {
        int prev = volume_get(), v = atoi(val);
        if (!strcasecmp(val, "up")) v = (prev < 0 ? 50 : prev) + 15;
        else if (!strcasecmp(val, "down")) v = (prev < 0 ? 50 : prev) - 15;
        else if (!strcasecmp(val, "mute") || !strcasecmp(val, "off")) v = 0;
        if (!volume_set(v)) {
            actions_log("volume %d: not done (no sound device)", v);
            return "There's no sound device here to turn up or down.";
        }
        push_undo(U_VOLUME, NULL, (float)prev);
        actions_log("volume %d%% (was %d%%)", v < 0 ? 0 : v > 100 ? 100 : v, prev);
        snprintf(said, sizeof said, "Volume set to %d%%.", v < 0 ? 0 : v > 100 ? 100 : v);
        return said;
    }
    bool on = word_on(val);
    if (!strcasecmp(thing, "dark") || !strcasecmp(thing, "light") || !strcasecmp(thing, "theme")) {
        bool dark = !strcasecmp(thing, "light") ? !on : !strcasecmp(thing, "theme") ? !strcasecmp(val, "dark") : on;
        bool was = color_luma(D.bg) < 0.35f;
        if (theme_set_dark) {
            theme_set_dark(dark);
            push_undo(U_DARK, NULL, was);
        } else {
            /* No light/dark switch in this build: the designer remakes the
             * same space in the other key. */
            design_begin(dark ? "Keep everything about this space, but make it dark: deep backgrounds, light text, the same accents."
                              : "Keep everything about this space, but make it light: bright backgrounds, dark text, the same accents.");
        }
        actions_log("%s mode", dark ? "dark" : "light");
        return dark ? "Going dark." : "Going light.";
    }
    if (!strcasecmp(thing, "voice")) {
        push_undo(U_VOICE, NULL, voice_enabled());
        voice_set_enabled(on);
        actions_log("voice %s", on ? "on" : "off");
        return on ? "I'll speak aloud again." : "I'll stay silent and only write.";
    }
    if (!strcasecmp(thing, "text")) {
        push_undo(U_TEXT, NULL, A.prof.text_scale);
        A.prof.text_scale = on ? 1.25f : 1.0f;
        comfort_changed();
        actions_log("text %s", on ? "larger" : "as designed");
        return on ? "Text is larger now." : "Text is back to its designed size.";
    }
    if (!strcasecmp(thing, "contrast")) {
        push_undo(U_CONTRAST, NULL, A.prof.high_contrast);
        A.prof.high_contrast = on;
        fm_contrast_floor = on ? 7.0f : 4.5f;
        comfort_changed();
        actions_log("strong contrast %s", on ? "on" : "off");
        return on ? "Stronger contrast is on." : "Contrast is back to normal.";
    }
    if (!strcasecmp(thing, "motion")) {
        push_undo(U_MOTION, NULL, A.prof.reduce_motion);
        A.prof.reduce_motion = on;
        D.motion = on ? 0 : 0.6f;
        comfort_changed();
        actions_log("less movement %s", on ? "on" : "off");
        return on ? "Things will move less now." : "Movement is back.";
    }
    if (!strcasecmp(thing, "sense")) {
        int s = sense_named(val);
        if (s < 0)
            return "I don't have a sense by that name.";
        bool want = word_on(extra);
        push_undo(U_SENSE, val, sense_on(s));
        sense_set(s, want);
        alive_prefs_save();
        actions_log("sense %s %s", val, want ? "on" : "off");
        snprintf(said, sizeof said, "%s: %s.", sense_info[s].name, want ? "I can sense it again" : "I won't sense it any more");
        return said;
    }
    bool *p = pref_named(thing);
    if (p) {
        push_undo(U_PREF, thing, *p);
        *p = on;
        alive_prefs_save();
        actions_log("%s %s", thing, on ? "on" : "off");
        snprintf(said, sizeof said, "%s %s.", thing, on ? "on" : "off");
        return said;
    }
    actions_log("set %s: not something I can change", spec);
    return "That isn't a setting I can change yet.";
}

bool actions_undo(void) {
    if (!nundo) {
        toast("Nothing to undo.");
        return false;
    }
    struct undo u = undo[--nundo];
    char body[160];
    switch (u.kind) {
    case U_LIGHT:
        snprintf(body, sizeof body, "{\"target\":\"%s\",\"effect\":\"restore\"}", u.what);
        senses_lights_post(strcmp(u.what, "all") ? "/lights" : "/lights/restore", strcmp(u.what, "all") ? body : "{}");
        break;
    case U_VOLUME:
        if (u.prev >= 0)
            volume_set((int)u.prev);
        break;
    case U_DARK:
        if (theme_set_dark)
            theme_set_dark(u.prev > 0.5f);
        break;
    case U_VOICE: voice_set_enabled(u.prev > 0.5f); break;
    case U_TEXT: A.prof.text_scale = u.prev; comfort_changed(); break;
    case U_CONTRAST:
        A.prof.high_contrast = u.prev > 0.5f;
        fm_contrast_floor = A.prof.high_contrast ? 7.0f : 4.5f;
        comfort_changed();
        break;
    case U_MOTION:
        A.prof.reduce_motion = u.prev > 0.5f;
        D.motion = A.prof.reduce_motion ? 0 : 0.6f;
        comfort_changed();
        break;
    case U_PREF: {
        bool *p = pref_named(u.what);
        if (p)
            *p = u.prev > 0.5f;
        alive_prefs_save();
        break;
    }
    case U_SENSE:
        sense_set(sense_named(u.what), u.prev > 0.5f);
        alive_prefs_save();
        break;
    case U_FILE: /* the file it wrote goes; one it replaced comes back */
        if (u.prev > 0.5f)
            rename(STATE_DIR "/undo-file", u.what);
        else
            unlink(u.what);
        break;
    }
    static const char *const names[] = {"lights", "volume", "light/dark", "voice", "text size", "contrast", "movement",
                                        "preference", "sense", "file"};
    actions_log("undid the last change to the %s%s%s", names[u.kind], u.what[0] ? " " : "", u.what);
    toast("Undone: %s.", names[u.kind]);
    return true;
}

/* ---- writing a file in their home ---- */

#define HOME_DIR "/root"
#define WRITE_MAX (512 * 1024)

/* Where it may go: inside their home, nothing hidden on the way (no .bashrc, .config/autostart...), no "..".
 * Returns the full path in out, or a reason it may not. */
static const char *write_path(const char *given, char *out, size_t n) {
    while (*given == ' ')
        given++;
    if (!strncmp(given, "~/", 2))
        snprintf(out, n, HOME_DIR "/%s", given + 2);
    else if (!strncmp(given, HOME_DIR "/", strlen(HOME_DIR) + 1))
        snprintf(out, n, "%s", given);
    else if (given[0] != '/' && given[0])
        snprintf(out, n, HOME_DIR "/%s", given);
    else
        return "only files in their home folder (~/...)";
    char *e = out + strlen(out);
    while (e > out && (e[-1] == ' ' || e[-1] == '\n'))
        *--e = 0;
    if (strstr(out, "..") || strstr(out, "//"))
        return "no .. in the path";
    for (const char *p = out + strlen(HOME_DIR); *p; p++)
        if (*p == '/' && p[1] == '.')
            return "not a hidden file or folder";
    if (out[strlen(out) - 1] == '/')
        return "that is a folder, not a file";
    return NULL;
}

static bool make_parents(const char *path) {
    char d[700];
    snprintf(d, sizeof d, "%s", path);
    for (char *p = d + strlen(HOME_DIR) + 1; (p = strchr(p, '/')); p++) {
        *p = 0;
        if (mkdir(d, 0755) != 0 && errno != EEXIST)
            return false;
        *p = '/';
    }
    return true;
}

static char write_report[1200]; /* what [[write]] did, for the note it reads next (actions_report) */

const char *actions_report(void) {
    static char out[1200];
    snprintf(out, sizeof out, "%s", write_report);
    write_report[0] = 0;
    return out;
}

/* Writes it; what happened, in words, for the summary it is told. */
static const char *write_file(const char *given, const char *text, size_t len) {
    static char said[400];
    char path[700];
    const char *no = write_path(given, path, sizeof path);
    if (no) {
        snprintf(said, sizeof said, "couldn't write %.100s: %s", given, no);
        return said;
    }
    if (len > WRITE_MAX) {
        snprintf(said, sizeof said, "couldn't write %.100s: it's over 512 KB", given);
        return said;
    }
    struct stat st;
    bool was = stat(path, &st) == 0;
    if (was && !S_ISREG(st.st_mode)) {
        snprintf(said, sizeof said, "couldn't write %.100s: something that isn't a file is there", given);
        return said;
    }
    if (!make_parents(path)) {
        snprintf(said, sizeof said, "couldn't make the folders for %.100s", given);
        return said;
    }
    if (was) { /* kept, for [[undo]] */
        unlink(STATE_DIR "/undo-file");
        if (link(path, STATE_DIR "/undo-file") != 0) {
            FILE *in = fopen(path, "rb"), *out = fopen(STATE_DIR "/undo-file", "wb");
            char buf[8192];
            size_t k;
            while (in && out && (k = fread(buf, 1, sizeof buf, in)) > 0)
                fwrite(buf, 1, k, out);
            if (in) fclose(in);
            if (out) fclose(out);
        }
    }
    char tmp[720];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(text, 1, len, f) == len;
    if (f)
        fclose(f);
    ok = ok && rename(tmp, path) == 0;
    if (!ok) {
        unlink(tmp);
        snprintf(said, sizeof said, "couldn't write %.100s", given);
        return said;
    }
    push_undo(U_FILE, path, was ? 1 : 0);
    actions_log("%s %s (%zu bytes)", was ? "replaced" : "wrote", path, len);
    LOG("write: %s %s (%zu bytes)\n", was ? "replaced" : "wrote", path, len);
    snprintf(said, sizeof said, "%s ~%s (%zu bytes)%s", was ? "replaced" : "wrote", path + strlen(HOME_DIR), len,
             was ? "; [[undo]] brings the old one back" : "");
    if (was)
        toast("%s replaced %s (it can be undone).", D.ai_name[0] ? D.ai_name : "Your companion",
              path + strlen(HOME_DIR) + 1);
    return said;
}

/* ---- running a command, only with their yes ---- */

static char asking[600];  /* the command on screen, waiting for an answer */
static char running[600]; /* the command running now */
static job *run_job;
static uint64_t run_started;
static char run_result[3600]; /* waiting to be told to the companion */

bool actions_confirming(void) { return asking[0] != 0; }

/* Its body asks for this one (it can't be undone): the question goes on screen */
void actions_ask_run(const char *cmd) {
    if (!asking[0] && !run_job)
        snprintf(asking, sizeof asking, "%s", cmd);
}

static void run_child(void *arg, char **result) {
    const char *cmd = arg;
    int p[2];
    if (pipe(p) < 0) {
        *result = strdup("could not start it");
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0)
            dup2(nul, 0);
        for (int i = 3; i < 256; i++)
            close(i);
        chdir("/root");
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    /* Its output, up to a few kilobytes; five minutes at most. */
    char out[3000];
    size_t n = 0;
    bool timed_out = false, more = false;
    uint64_t start = (uint64_t)time(NULL);
    for (;;) {
        struct pollfd pf = {.fd = p[0], .events = POLLIN};
        int r = poll(&pf, 1, 1000);
        if ((uint64_t)time(NULL) - start > 300) {
            timed_out = true;
            kill(-pid, SIGKILL);
            break;
        }
        if (r <= 0)
            continue;
        char tmp[512];
        ssize_t got = read(p[0], tmp, sizeof tmp);
        if (got <= 0)
            break;
        size_t take = (size_t)got < sizeof out - 1 - n ? (size_t)got : sizeof out - 1 - n;
        more |= take < (size_t)got;
        memcpy(out + n, tmp, take);
        n += take;
    }
    out[n] = 0;
    close(p[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    sbuf b;
    sb_init(&b);
    if (timed_out)
        sb_add(&b, "stopped after five minutes (still running then)");
    else if (WIFEXITED(status))
        sb_printf(&b, "exit code %d", WEXITSTATUS(status));
    else
        sb_add(&b, "it was stopped by a signal");
    sb_printf(&b, "\n%s%s", n ? out : "(no output)", more ? "\n... (more output cut)" : "");
    *result = b.p;
}

static bool dangerous(const char *cmd) {
    static const char *const words[] = {"rm ", "rm\t", "rmdir", "mkfs", "dd ", "shred", "reboot", "poweroff", "halt",
                                        "kill", "> ", ">/", "chmod", "chown", "mv ", "truncate", "format", NULL};
    for (int i = 0; words[i]; i++)
        if (strstr(cmd, words[i]))
            return true;
    return false;
}

void actions_draw(void) {
    if (!asking[0])
        return;
    font *f = font_get(D.font_body, D.body_px), *small = font_get(D.font_body, D.body_px - 3),
         *mono = font_get("JetBrainsMono", D.body_px - 2), *h = font_get(D.font_heading, D.heading_px * 0.6f);
    int w = 640, cmd_h = draw_text_wrapped(NULL, mono, (rect){0, 0, w - 88, 400}, asking, 0, 1.3f);
    bool risky = dangerous(asking);
    int hgt = 210 + cmd_h + (risky ? 30 : 0);
    rect r = {(A.W - w) / 2, A.H - hgt - 150, w, hgt};
    if (r.y < 20)
        r.y = 20;
    fill_rect(A.frame, (rect){0, 0, A.W, A.H}, color_alpha(D.bg, 0.3f));
    shadow_round(A.frame, (rect){r.x, r.y + 12, r.w, r.h}, D.radius + 6, 40, RGBA(0, 0, 0, 100));
    blur_fast(A.frame, r, D.radius + 6, 12);
    fill_round(A.frame, r, D.radius + 6, color_alpha(D.surface, 0.97f));
    stroke_round(A.frame, r, D.radius + 6, 1.5f, color_alpha(D.accent, 0.8f));
    char title[96];
    snprintf(title, sizeof title, "%s wants to run this on your computer", D.ai_name[0] ? D.ai_name : "Your companion");
    int y = r.y + 24;
    draw_text(A.frame, h, r.x + 28, y, title, D.surface_text);
    y += (int)(D.heading_px * 0.6f * 1.5f);
    rect box = {r.x + 28, y, r.w - 56, cmd_h + 24};
    fill_round(A.frame, box, 10, color_alpha(D.surface_text, 0.08f));
    draw_text_wrapped(A.frame, mono, rect_inset(box, 12), asking, D.surface_text, 1.3f);
    y += box.h + 12;
    if (risky) {
        draw_text(A.frame, small, r.x + 28, y, "This can delete or change things, and it can't be undone.", RGBA(232, 96, 80, 255));
        y += 30;
    }
    draw_text(A.frame, small, r.x + 28, y, "Nothing runs unless you say yes. Only do it if you know what it does.", D.muted);
    int bh = font_line_height(f) + 18, by = r.y + r.h - bh - 22;
    bool no = ui_button((rect){r.x + 28, by, 180, bh}, "Not now", f, color_alpha(D.surface_text, 0.12f), D.surface_text,
                        D.radius * 0.6f, true);
    bool yes = ui_button((rect){r.x + r.w - 208, by, 180, bh}, "Run it", f, risky ? RGBA(200, 70, 60, 255) : D.accent,
                         risky ? RGBA(255, 255, 255, 255) : readable_on(D.accent), D.radius * 0.6f, true);
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_ESC)
            no = true;
    static uint64_t logged;
    if (A.now - logged > 1000) {
        logged = A.now;
        LOGRECT("run-yes", ((rect){r.x + r.w - 208, by, 180, bh}));
        LOGRECT("run-no", ((rect){r.x + 28, by, 180, bh}));
    }
    char note[800];
    if (yes && !run_job) {
        snprintf(running, sizeof running, "%s", asking);
        run_job = job_start(run_child, running);
        run_started = A.now;
        actions_log("ran (they said yes): %.120s", running);
        toast("Running it...");
        asking[0] = 0;
    } else if (no) {
        actions_log("didn't run (they said no): %.120s", asking);
        snprintf(note, sizeof note, "(They chose not to run `%s`. Don't run it.)", asking);
        convo_add(&A.companion, "hidden", note);
        asking[0] = 0;
    }
}

void actions_tick(void) {
    web_tick();
    emblem_busy_hint = emblem_busy_hint || run_job != NULL; /* its light circles while it works */
    char *res = NULL;
    if (run_job && job_poll(run_job, &res)) {
        job_free(run_job);
        run_job = NULL;
        uint64_t secs = (A.now - run_started) / 1000;
        snprintf(run_result, sizeof run_result,
                 "(The command they let you run, `%.300s`, finished after %llu s: %.2800s\nTell them briefly, in plain "
                 "words, what happened and what it means. No tags.)",
                 running, (unsigned long long)secs, res ? res : "(nothing came back)");
        free(res);
        if (secs >= 60)
            alive_event(0, "task", "the command they ran (%.80s) finished after %llu minutes", running,
                        (unsigned long long)(secs / 60));
    }
    /* Told as soon as the companion is free to listen. */
    if (run_result[0] && !A.companion.pending) {
        companion_prompt(run_result, 400);
        run_result[0] = 0;
    }
}

/* ---- reading the reply ---- */

/* Finds the next [[NAME:...]] (or [[NAME]]) tag; returns its start and puts
 * the inside in arg. */
static char *find_tag(char *text, const char *name, char *arg, size_t n) {
    char open[24];
    snprintf(open, sizeof open, "[[%s", name);
    for (char *t = strstr(text, open); t; t = strstr(t + 1, open)) {
        char *after = t + strlen(open);
        if (*after != ':' && *after != ']')
            continue;
        char *end = strstr(after, "]]");
        size_t len = end ? (size_t)(end - after) : strlen(after);
        if (*after == ':')
            after++, len--;
        snprintf(arg, n, "%.*s", (int)len, after);
        return t;
    }
    return NULL;
}

static void cut_tag(char *t) {
    char *end = strstr(t, "]]");
    char *after = end ? end + 2 : t + strlen(t);
    memmove(t, after, strlen(after) + 1);
}

static bool blank(const char *s) {
    /* nothing but spaces and tags left */
    while (*s) {
        if (s[0] == '[' && s[1] == '[') {
            const char *e = strstr(s, "]]");
            if (!e)
                return true;
            s = e + 2;
            continue;
        }
        if (!isspace((unsigned char)*s))
            return false;
        s++;
    }
    return true;
}

static int looks;         /* in a row, for one thing they asked */
static bool chain_person; /* that thing came from the person */

/* ---- the web ([[search:...]], [[read:...]]) ----
 * Like a look, but it waits on the network: it runs in a job, and what it
 * found goes to the companion when it comes back. */
static struct {
    job *job;
    char *requests;
    char *found;
} web;

static void web_child(void *arg, char **result) { *result = web_lookup(arg); }

static void web_tick(void) {
    emblem_busy_hint = web.job != NULL;
    char *res = NULL;
    if (web.job && job_poll(web.job, &res)) {
        job_free(web.job);
        web.job = NULL;
        free(web.requests);
        web.requests = NULL;
        web.found = res ? res : strdup("Nothing came back.");
        LOG("web: %.300s\n", web.found);
    }
    if (web.found && !A.companion.pending) {
        sbuf note;
        sb_init(&note);
        sb_printf(&note, "%s on the web just now; only you see this):\n%.7000s\n(Now answer them, using this, and say "
                         "where it came from in a few words. %s)",
                  LOOK_MARK, web.found, looks >= MAX_LOOKS ? "No more looks this time." : "You may look again if you really need to.");
        free(web.found);
        web.found = NULL;
        companion_prompt(note.p, 900);
        sb_free(&note);
    }
}

bool actions_take(struct conversation *c, bool *from_person) {
    struct msg *m = &c->msgs[c->n - 1];
    /* The answer to a look continues whatever began it. */
    bool continuing = c->n >= 2 && !strcmp(c->msgs[c->n - 2].role, "hidden") &&
                      !strncmp(c->msgs[c->n - 2].text, LOOK_MARK, strlen(LOOK_MARK));
    if (continuing) {
        *from_person = chain_person;
        /* The answer has what was looked up; the conversation goes on from
         * the person's own words, so the looks themselves are let go. */
        int k = c->n - 2;
        while (k >= 0 && !strcmp(c->msgs[k].role, "hidden") && !strncmp(c->msgs[k].text, LOOK_MARK, strlen(LOOK_MARK)))
            free(c->msgs[k--].text);
        c->msgs[k + 1] = c->msgs[c->n - 1];
        c->n = k + 2;
        m = &c->msgs[c->n - 1];
    } else {
        looks = 0;
    }

    char arg[700];
    char *t;
    /* The web: a question too, answered when the network has. */
    if (strstr(m->text, "[[search") || strstr(m->text, "[[read")) {
        sbuf req;
        sb_init(&req);
        int n = 0;
        static const char *const kinds[] = {"search", "read"};
        for (int k = 0; k < 2; k++)
            while ((t = find_tag(m->text, kinds[k], arg, sizeof arg))) {
                cut_tag(t);
                for (char *p = arg; *p; p++)
                    if (*p == '\t' || *p == '\n')
                        *p = ' ';
                if (n < 3 && looks < MAX_LOOKS && arg[0])
                    sb_printf(&req, "%s\t%s\n", kinds[k], arg), n++;
            }
        if (n && !web.job && !web.found) {
            looks++;
            chain_person = *from_person;
            free(m->text); /* the half-reply goes: its answer comes with what was found */
            c->n--;
            web.requests = req.p;
            web.job = job_start(web_child, web.requests);
            LOG("web: looking up %s", web.requests);
            toast("Looking it up...");
            return true;
        }
        sb_free(&req);
    }
    /* Looks first: they're questions, so the reply isn't finished yet. */
    if (strstr(m->text, "[[look")) {
        sbuf answers;
        sb_init(&answers);
        int asked = 0;
        while ((t = find_tag(m->text, "look", arg, sizeof arg))) {
            cut_tag(t);
            if (looks >= MAX_LOOKS || asked >= 8)
                continue;
            char out[3000];
            if (!strcasecmp(arg, "actions") || !strcasecmp(arg, "log")) {
                char r[8][160];
                int k = actions_recent(r, 8);
                size_t o = (size_t)snprintf(out, sizeof out, "%s", k ? "What you did lately:" : "You haven't done anything yet.");
                for (int i = 0; i < k && o < sizeof out; i++)
                    o += (size_t)snprintf(out + o, sizeof out - o, "\n- %s", r[i]);
            } else if (!senses_look(arg, out, sizeof out)) {
                snprintf(out, sizeof out, "There's nothing called \"%s\" to look at. You can look at: processes, "
                         "hardware, lights, windows, screen, network, time, activity, notifications, files PATH, file PATH, actions.", arg);
            }
            sb_printf(&answers, "%s[look:%s]\n%s", asked ? "\n\n" : "", arg, out);
            asked++;
        }
        if (asked) {
            looks++;
            chain_person = *from_person;
            LOG("look (%d): %.300s\n", looks, answers.p);
            /* The half-reply goes: its answer is coming, with what it saw. */
            free(m->text);
            c->n--;
            sbuf note;
            sb_init(&note);
            sb_printf(&note, "%s just now; only you see this):\n%s\n(Now answer them, using this. %s)", LOOK_MARK,
                      answers.p, looks >= MAX_LOOKS ? "No more looks this time." : "You may look again if you really need to.");
            companion_prompt(note.p, 700);
            sb_free(&note);
            sb_free(&answers);
            return true;
        }
        sb_free(&answers);
    }

    /* Hands: only for what the person asked. */
    bool acted = false;
    char summary[400] = "";
    size_t so = 0;
#define SAY(...) (so += (size_t)snprintf(summary + so, so < sizeof summary ? sizeof summary - so : 0, __VA_ARGS__))
    while ((t = find_tag(m->text, "light", arg, sizeof arg))) {
        cut_tag(t);
        if (*from_person)
            SAY("%s ", actions_light(arg, true)), acted = true;
    }
    while ((t = find_tag(m->text, "set", arg, sizeof arg))) {
        cut_tag(t);
        if (*from_person)
            SAY("%s ", do_set(arg)), acted = true;
    }
    while ((t = find_tag(m->text, "notify", arg, sizeof arg))) {
        cut_tag(t);
        if (!*from_person)
            continue;
        char *bar = strchr(arg, '|');
        if (bar)
            *bar = 0;
        const char *body = bar ? bar + 1 : "";
        if (notify)
            notify(arg, body, D.ai_name);
        else
            toast("%s%s%s", arg, *body ? ": " : "", body);
        senses_note_notification(arg, body, D.ai_name);
        actions_log("notification: %.100s", arg);
        acted = true;
    }
    /* [[write:~/path]]...[[/write]] (what is written may hold "]]" of its own) */
    for (char *o; (o = strstr(m->text, "[[write:"));) {
        char *name_end = strstr(o, "]]");
        char *close = name_end ? strstr(name_end, "[[/write]]") : NULL;
        char name[400] = "";
        if (name_end)
            snprintf(name, sizeof name, "%.*s", (int)(name_end - o - 8) < 399 ? (int)(name_end - o - 8) : 399, o + 8);
        char *body = name_end ? name_end + 2 : o + strlen(o);
        if (close && *body == '\n')
            body++; /* the line break after the tag is not the file's */
        size_t len = close ? (size_t)(close - body) : 0;
        if (*from_person && close) {
            const char *r = write_file(name, body, len);
            SAY("(%s) ", r), acted = true;
            size_t wl = strlen(write_report);
            snprintf(write_report + wl, sizeof write_report - wl, "- [[write:%.80s]] -> %s\n", name, r);
        }
        else if (*from_person && name_end)
            SAY("(couldn't write %.80s: the [[/write]] that ends it is missing) ", name), acted = true;
        char *after = close ? close + 10 : name_end ? name_end + 2 : o + strlen(o);
        memmove(o, after, strlen(after) + 1);
    }
    while ((t = find_tag(m->text, "undo", arg, sizeof arg))) {
        cut_tag(t);
        if (*from_person)
            acted |= actions_undo();
    }
    while ((t = find_tag(m->text, "run", arg, sizeof arg))) {
        cut_tag(t);
        if (*from_person && !asking[0] && !run_job && arg[0]) {
            snprintf(asking, sizeof asking, "%s", arg); /* the question appears on screen */
            LOG("run asked: %s\n", asking);
            acted = true;
        }
    }
    /* [[open:]] of a path or the senses panel; apps are opened as before. */
    for (char *o = m->text; (o = strstr(o, "[[open:"));) {
        char *what = o + 7;
        if (*what != '/' && *what != '~' && strncasecmp(what, "senses", 6)) {
            o += 7;
            continue;
        }
        char *end = strstr(what, "]]");
        snprintf(arg, sizeof arg, "%.*s", end ? (int)(end - what) : (int)strlen(what), what);
        cut_tag(o);
        if (!*from_person)
            continue;
        if (!strncasecmp(arg, "senses", 6))
            alive_panel_open(true);
        else {
            char path[700];
            snprintf(path, sizeof path, "%s%s", arg[0] == '~' ? "/root" : "", arg[0] == '~' ? arg + 1 : arg);
            desktop_open_path(path);
        }
        acted = true;
    }
#undef SAY
    /* Only tags and nothing said: say what happened instead. */
    if (acted && blank(m->text)) {
        size_t l = strlen(m->text);
        char *t2 = malloc(l + strlen(summary) + 8);
        if (t2) {
            sprintf(t2, "%s%s", summary[0] ? summary : "Done.", m->text);
            free(m->text);
            m->text = t2;
        }
    }
    looks = 0; /* a finished answer ends the chain */
    return false;
}

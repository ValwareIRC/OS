#define _GNU_SOURCE
#include "voice.h"
#include "app.h"
#include "settings.h"
#include "web.h"
#include "../lib/http.h"
#include "../lib/json.h"
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* The companion's voice and ears, on this computer: speech comes from Kokoro (Piper until a voice is chosen)
 * into the sound card, shaped on the way by os-voicefx when they asked for it, and what they say is recorded and
 * written down by whisper.cpp. A voice of its own is made by mixing Kokoro's (a voice is 510 x 256 numbers: a
 * weighted average of several is a voice between them) and shaping it (pitch, formants, warmth, breath, an
 * effect). Nothing leaves the machine for any of it, unless a cloud voice was chosen. Every word spoken is also
 * on screen, so the voice is never the only way in. */

#define VOICE_DIR "/usr/share/os/voice"
#define KOKORO VOICE_DIR "/kokoro"
#define VOICES STATE_DIR "/voices" /* the voices they chose, downloaded */
#define VOICES_URL "https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX/resolve/main/voices/"
#define PIPER "/usr/bin/piper" /* a wrapper that tells it where its libraries are */
#define SPEAK_MODEL VOICE_DIR "/en_US-amy-medium.onnx"
#define HEAR_MODEL VOICE_DIR "/ggml-base.en.bin"
#define MIC_WAV "/tmp/os-mic.wav"

static int speaker_pid, rec_pid;
static uint64_t spoke_at; /* when the words being said began, and with what */
static char spoke_with[96];
static bool cloud_speak(const char *v, int unused);
static bool pc_speak(const char *voice);

/* ---- the PC's expressive voice ----
 * Chatterbox on this PC's graphics card (scripts/voice-gpu-server.py, started by the launcher's "Expressive
 * voice" box), reached at 10.0.2.2, QEMU's way to the PC, until the OS drives the card itself (docs/gpu.md). It
 * speaks with real feeling (how strongly: "exaggeration") and in any voice cloned from a recording. Voices are
 * "pc:NAME" (pc:default is its own). */
#define PC_VOICE_URL "http://10.0.2.2:4572" /* (4571 is the PC's senses) */
static struct {
    job *asking;
    bool up;
    uint64_t asked_at;
    char voices[600]; /* their names, space-separated */
    job *cloning;
    char cloned[48], clone_why[200];
} PCV;

static void pc_status_child(void *arg, char **result) {
    (void)arg;
    struct http_response r;
    int e = http_request("GET", PC_VOICE_URL "/status", NULL, NULL, 0, 3, &r);
    *result = e == 0 && r.status == 200 && r.body ? strdup(r.body) : strdup("");
    http_response_free(&r);
}

/* every minute (the PC's voice can be started or stopped while the OS runs) */
static void pc_poll(void) {
    char *res;
    if (PCV.asking && job_poll(PCV.asking, &res)) {
        job_free(PCV.asking);
        PCV.asking = NULL;
        json *j = res ? json_parse(res, strlen(res)) : NULL;
        bool up = j && json_get(j, "ready") && json_bool(json_get(j, "ready"), false);
        PCV.voices[0] = 0;
        json *vs = j ? json_get(j, "voices") : NULL;
        for (int i = 0; vs && i < json_len(vs); i++) {
            size_t l = strlen(PCV.voices);
            snprintf(PCV.voices + l, sizeof PCV.voices - l, "%s%s", l ? " " : "", json_str(json_at(vs, i), ""));
        }
        if (up != PCV.up)
            LOG("voice: the PC's expressive voice is %s%s%s\n", up ? "here (" : "gone", up ? PCV.voices : "",
                up ? ")" : "");
        PCV.up = up;
        json_free(j);
        free(res);
    }
    if (!PCV.asking && (!PCV.asked_at || now_ms() - PCV.asked_at > 60000)) {
        PCV.asked_at = now_ms();
        PCV.asking = job_start(pc_status_child, NULL);
    }
}

bool voice_pc_available(void) { return PCV.up; }

/* a tone as the PC's voice says it: how strongly (exaggeration) and how closely it keeps to the voice (cfg) */
static void pc_feeling(const char *tone, float *ex, float *cfg) {
    static const struct {
        const char *name;
        float ex, cfg;
    } f[] = {{"happy", 0.75f, 0.45f},   {"excited", 1.1f, 0.3f}, {"playful", 0.85f, 0.4f}, {"tender", 0.45f, 0.6f},
             {"calm", 0.35f, 0.55f},    {"sad", 0.7f, 0.4f},     {"serious", 0.4f, 0.6f},  {"worried", 0.75f, 0.4f},
             {"surprised", 1.0f, 0.35f}, {"sleepy", 0.3f, 0.6f},  {"whisper", 0.3f, 0.6f},  {"hushed", 0.25f, 0.6f}};
    *ex = 0.5f, *cfg = 0.5f;
    for (size_t i = 0; tone && i < sizeof f / sizeof f[0]; i++)
        if (!strcmp(tone, f[i].name))
            *ex = f[i].ex, *cfg = f[i].cfg;
}

static char pc_tone[16]; /* the line's tone, for the child that speaks it */

static void play_piece(const char *data, size_t len, void *ctx) {
    FILE *play = ctx;
    if (play && len)
        fwrite(data, 1, len, play), fflush(play);
}

/* in a child, the text on stdin: said by the PC's voice into the sound card as each sentence is made */
static bool pc_speak(const char *voice) {
    static char text[8192];
    size_t n = fread(text, 1, sizeof text - 1, stdin);
    text[n] = 0;
    for (char *p = text; *p; p++)
        if (*p == '\n')
            *p = ' ';
    float ex, cfg;
    pc_feeling(pc_tone[0] ? pc_tone : NULL, &ex, &cfg);
    sbuf body;
    sb_init(&body);
    sb_add(&body, "{\"text\":");
    sb_json_str(&body, text);
    sb_add(&body, ",\"voice\":");
    sb_json_str(&body, voice);
    sb_printf(&body, ",\"exaggeration\":%.2f,\"cfg\":%.2f}", (double)ex, (double)cfg);
    const char *hdr[] = {"Content-Type: application/json", NULL};
    FILE *play = popen("/bin/os-play 24000 1", "w");
    struct http_response r;
    http_request_stream("POST", PC_VOICE_URL "/speak", hdr, body.p, body.len, 120, play_piece, play, &r);
    bool ok = r.status == 200;
    if (!ok) {
        FILE *e = fopen("/tmp/os-speak.err", "w");
        if (e) {
            fprintf(e, "os-speak: the PC's voice: %s\n", r.status ? (r.body ? r.body : "an error") : r.error);
            fclose(e);
        }
    }
    free(body.p);
    http_response_free(&r);
    if (play)
        pclose(play);
    return ok;
}

/* a voice of theirs for the PC: a recording (a file of theirs, or ten seconds from the microphone), sent to it */
static void clone_child(void *arg, char **result) {
    char spec[400], name[48] = "", path[300] = "";
    snprintf(spec, sizeof spec, "%s", (const char *)arg);
    char *sp = strchr(spec, ' ');
    if (sp)
        *sp = 0, snprintf(path, sizeof path, "%s", sp + 1);
    snprintf(name, sizeof name, "%s", spec);
    char rec[] = "/tmp/os-clone.wav";
    if (!path[0] || !strcmp(path, "record")) { /* ten seconds of their voice */
        pid_t pid = fork();
        if (pid == 0) {
            execl("/bin/os-rec", "os-rec", "24000", rec, (char *)NULL);
            _exit(127);
        }
        sleep(10);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        snprintf(path, sizeof path, "%s", rec);
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        *result = strdup("FAILED: that recording isn't there");
        return;
    }
    static char data[16 << 20];
    size_t n = fread(data, 1, sizeof data, f);
    fclose(f);
    char url[200];
    snprintf(url, sizeof url, PC_VOICE_URL "/voice?name=%s", name);
    const char *hdr[] = {"Content-Type: audio/wav", NULL};
    struct http_response r;
    http_request("POST", url, hdr, data, n, 30, &r);
    char *res = NULL;
    if (r.status == 200)
        res = strdup("ok");
    else if (asprintf(&res, "FAILED: %s", r.status ? (r.body ? r.body : "the PC refused it") : r.error) < 0)
        res = NULL;
    http_response_free(&r);
    unlink(rec);
    *result = res ? res : strdup("FAILED");
}

/* "NAME from ~/path.wav" or "NAME record" */
const char *voice_clone(const char *spec) {
    static char back[300];
    char name[48] = "", rest[300] = "";
    sscanf(spec, " %47s %299[^\n]", name, rest);
    bool name_ok = name[0] != 0;
    for (const char *c = name; *c; c++)
        name_ok &= isalnum((unsigned char)*c) || *c == '-' || *c == '_';
    if (!PCV.up) {
        snprintf(back, sizeof back, "cloning needs the PC's expressive voice, which isn't running (the launcher's "
                                    "\"Expressive voice\" box)");
        return back;
    }
    if (!name_ok) {
        snprintf(back, sizeof back, "a cloned voice needs a name of letters, digits, - or _");
        return back;
    }
    if (PCV.cloning) {
        snprintf(back, sizeof back, "still making %s", PCV.cloned);
        return back;
    }
    char arg[400], path[300] = "record";
    const char *from = !strncmp(rest, "from ", 5) ? rest + 5 : rest;
    if (from[0] && strcmp(from, "record")) {
        if (from[0] == '~')
            snprintf(path, sizeof path, "%s%s", home_dir(), from + 1);
        else
            snprintf(path, sizeof path, "%s", from);
    }
    snprintf(arg, sizeof arg, "%s %s", name, path);
    snprintf(PCV.cloned, sizeof PCV.cloned, "%s", name);
    PCV.cloning = job_start(clone_child, strdup(arg));
    LOG("voice: cloning %s from %s\n", name, path);
    snprintf(back, sizeof back, !strcmp(path, "record")
                                    ? "recording them for ten seconds now (they should talk naturally), then it becomes pc:%s"
                                    : "making pc:%s from that recording (a few seconds)",
             name);
    return back;
}
static job *hearing;
static bool enabled = true;
static unsigned said, cut; /* the last line started, and the last one cut off */
static char lent[128];      /* the app holding the microphone */

static int kokoro_rate(void) {
    static int rate;
    if (!rate) {
        FILE *f = fopen(KOKORO "/clips/RATE", "r");
        if (!f || fscanf(f, "%d", &rate) != 1)
            rate = 24000;
        if (f)
            fclose(f);
    }
    return rate;
}

const char *voice_current(void) { return settings_get("voice.name", ""); }

static bool kokoro_ready(const char *name) {
    char p[256];
    snprintf(p, sizeof p, VOICES "/%s.bin", name);
    return name[0] && !strchr(name, ':') && access(KOKORO "/os-speak", X_OK) == 0 && access(p, R_OK) == 0;
}

#define VOICEFX "/bin/os-voicefx"
static const char *const effects[] = {"none", "robot", "whisper", "radio", "phone", "room", "hall", NULL};

/* How one reply is said ([[tone:...]]), on top of its voice: pitch (semitones), how far the melody moves (range),
 * speed, warmth and loudness (breath only for a whisper: added air is heard as hiss); a whisper is its voice with some of af_nicole's (a breathy voice) in it, and
 * hushed is a voiceless whisper. For that reply only, then its own voice again. */
static const struct tone {
    const char *name;
    float pitch, range, speed, warmth, breath, gain;
    const char *effect;  /* NULL: its own */
    float nicole;        /* how much of af_nicole goes in */
} tones[] = {
    {"happy", 1.0f, 1.25f, 1.05f, -0.1f, 0, 1, NULL, 0},
    {"excited", 2.0f, 1.5f, 1.12f, -0.15f, 0, 1.05f, NULL, 0},
    {"playful", 1.2f, 1.35f, 1.06f, 0, 0, 1, NULL, 0},
    {"tender", -0.5f, 0.85f, 0.92f, 0.3f, 0, 0.85f, NULL, 0},
    {"calm", -0.5f, 0.8f, 0.93f, 0.2f, 0, 0.95f, NULL, 0},
    {"sad", -1.5f, 0.65f, 0.86f, 0.25f, 0, 0.8f, NULL, 0},
    {"serious", -1.0f, 0.75f, 0.97f, 0.1f, 0, 1, NULL, 0},
    {"worried", 0.6f, 1.1f, 1.06f, 0, 0, 0.9f, NULL, 0},
    {"surprised", 2.5f, 1.6f, 1.04f, -0.1f, 0, 1.05f, NULL, 0},
    {"sleepy", -1.2f, 0.6f, 0.82f, 0.3f, 0, 0.8f, NULL, 0},
    {"whisper", -0.3f, 0.8f, 0.94f, 0.1f, 0.2f, 0.7f, NULL, 0.35f},
    {"hushed", 0, 1, 0.95f, 0, 0, 0.8f, "whisper", 0},
};
static const struct tone *tone_next; /* for the next words said, then gone */
static bool whisper_voice(const char *v, float share, char *name, size_t n);

static const struct tone *tone_named(const char *name) {
    for (size_t i = 0; i < sizeof tones / sizeof tones[0]; i++)
        if (!strcasecmp(name, tones[i].name))
            return &tones[i];
    return NULL;
}

static float shape_get(const char *key, float lo, float hi) {
    float v = (float)atof(settings_get(key, "0"));
    return v < lo ? lo : v > hi ? hi : v;
}

/* " pitch=-2 formant=1 ..." for os-voicefx (numbers and a known name only: it goes into a command), with the
 * tone of the line (NULL: none) on top */
static void shape_args_tone(char *out, size_t n, const struct tone *t) {
    float pitch = shape_get("voice.pitch", -12, 12), formant = shape_get("voice.formant", -6, 6);
    float warmth = shape_get("voice.warmth", -1, 1), breath = shape_get("voice.breath", 0, 1);
    const char *fx = settings_get("voice.effect", "none");
    float range = 1, gain = 1;
    if (t) {
        pitch += t->pitch, warmth += t->warmth, breath += t->breath, range = t->range, gain = t->gain;
        warmth = warmth > 1 ? 1 : warmth < -1 ? -1 : warmth;
        breath = breath > 1 ? 1 : breath;
        if (t->effect)
            fx = t->effect;
    }
    int e = 0;
    for (int i = 0; effects[i]; i++)
        if (!strcmp(fx, effects[i]))
            e = i;
    size_t o = 0;
    out[0] = 0;
    if (pitch != 0)
        o += (size_t)snprintf(out + o, n - o, " pitch=%.1f", (double)pitch);
    if (formant != 0 && o < n)
        o += (size_t)snprintf(out + o, n - o, " formant=%.1f", (double)formant);
    if (warmth != 0 && o < n)
        o += (size_t)snprintf(out + o, n - o, " warmth=%.2f", (double)warmth);
    if (breath != 0 && o < n)
        o += (size_t)snprintf(out + o, n - o, " breath=%.2f", (double)breath);
    if (range != 1 && o < n)
        o += (size_t)snprintf(out + o, n - o, " range=%.2f", (double)range);
    if (gain != 1 && o < n)
        o += (size_t)snprintf(out + o, n - o, " gain=%.2f", (double)gain);
    if (e && o < n)
        snprintf(out + o, n - o, " effect=%s", effects[e]);
}

static void shape_args(char *out, size_t n) { shape_args_tone(out, n, NULL); }

/* "| os-voicefx RATE ... " when its voice is shaped (or the line has a tone), else nothing */
static const struct tone *tone_now; /* the tone of the line being started */
static void shape_stage(int rate, char *out, size_t n) {
    char a[200];
    shape_args_tone(a, sizeof a, tone_now);
    if (a[0] && access(VOICEFX, X_OK) == 0)
        snprintf(out, n, " | " VOICEFX " %d%s", rate, a);
    else
        out[0] = 0;
}

bool voice_can_speak(void) {
    if (access("/dev/dsp", W_OK) != 0)
        return false;
    const char *v = voice_current();
    if (!strncmp(v, "pc:", 3) && !PCV.up)
        return kokoro_ready(settings_get("voice.before_pc", "")) || access(PIPER, X_OK) == 0;
    if (strchr(v, ':') || kokoro_ready(v))
        return true;
    return access(PIPER, X_OK) == 0 && access(SPEAK_MODEL, R_OK) == 0;
}
bool voice_can_hear(void) { return access("/dev/dsp", R_OK) == 0 && access("/usr/bin/whisper-cli", X_OK) == 0 && access(HEAR_MODEL, R_OK) == 0; }
void voice_set_enabled(bool on) {
    enabled = on;
    if (!on)
        voice_hush();
}
bool voice_enabled(void) { return enabled; }

void voice_hush(void) {
    if (speaker_pid > 0) {
        cut = said;
        kill(-speaker_pid, SIGTERM);
        waitpid(speaker_pid, NULL, WNOHANG);
        speaker_pid = 0;
    }
}

bool voice_speaking(void) {
    if (speaker_pid > 0 && waitpid(speaker_pid, NULL, WNOHANG) == speaker_pid) {
        speaker_pid = 0;
        if (spoke_at) { /* how it went: and if os-speak couldn't, why */
            char why[200] = "";
            FILE *e = fopen("/tmp/os-speak.err", "r");
            while (e && fgets(why, sizeof why, e))
                if (strstr(why, "os-speak:") || strstr(why, "rror"))
                    break;
            if (e)
                fclose(e);
            why[strcspn(why, "\n")] = 0;
            LOG("voice: said (%s) in %.1fs%s%s\n", spoke_with, (double)(now_ms() - spoke_at) / 1000,
                strstr(why, "os-speak:") || strstr(why, "rror") ? ": " : "",
                strstr(why, "os-speak:") || strstr(why, "rror") ? why : "");
            spoke_at = 0;
            unlink("/tmp/os-speak.err");
        }
    }
    return speaker_pid > 0;
}

/* The language os-speak reads a line in: the line's own, not its voice's. os-speak took it from the first letter of
 * the voice file's name, and a mix is named after its biggest share: one that was mostly xiaoxiao was "zmix_...",
 * so English was read as Chinese (espeak-ng had no "zh" for the words its lexicon lacks: silence, the digits said
 * in Chinese). Mostly Chinese characters: Chinese; otherwise the voice's own language where it reads Latin
 * letters (British, Portuguese), else American English: the voice is how it sounds, not what it reads. */
static const char *speak_lang(const char *text, const char *voice) {
    size_t han = 0, latin = 0;
    for (const unsigned char *s = (const unsigned char *)text; *s;) {
        if ((s[0] & 0xf0) == 0xe0 && s[1] && s[2]) { /* three bytes: U+0800..U+FFFF */
            unsigned cp = (unsigned)(s[0] & 0x0f) << 12 | (unsigned)(s[1] & 0x3f) << 6 | (unsigned)(s[2] & 0x3f);
            han += (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0x3400 && cp <= 0x4dbf);
            s += 3;
            continue;
        }
        latin += (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z');
        s++;
    }
    if (han && han * 3 >= latin) /* (a Chinese character is a word; a Latin word is several letters) */
        return "zh";
    const char *base = strrchr(voice, '/');
    base = base ? base + 1 : voice;
    return base[0] == 'p' ? "pt-br" : base[0] == 'b' ? "en" : "en-us";
}

void voice_say(const char *text) {
    tone_now = tone_next; /* this line's tone, and only this line's */
    tone_next = NULL;
    if (!enabled || !text || !*text || !voice_can_speak())
        return;
    voice_hush(); /* the newest words win */
    said++;
    const char *lang = speak_lang(text, voice_current());
    int p[2];
    if (pipe(p) < 0)
        return;
    int pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        dup2(p[0], 0);
        close(p[0]);
        close(p[1]);
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0)
            dup2(nul, 2);
        const char *v = voice_current();
        /* a number, whatever the settings file holds (it goes into a command) */
        char speed[16];
        float sp = (float)atof(settings_get("voice.speed", "1.0"));
        sp = sp < 0.7f || sp > 1.4f ? 1.0f : sp;
        if (tone_now)
            sp *= tone_now->speed;
        snprintf(speed, sizeof speed, "%.2f", (double)sp);
        char hushed[80];
        if (tone_now && tone_now->nicole > 0 && whisper_voice(v, tone_now->nicole, hushed, sizeof hushed))
            v = hushed; /* its own voice with some breathiness in it */
        if (!strncmp(v, "pc:", 3) && PCV.up) {
            snprintf(pc_tone, sizeof pc_tone, "%s", tone_now ? tone_now->name : "");
            _exit(pc_speak(v + 3) ? 0 : 1);
        }
        if (!strncmp(v, "pc:", 3)) /* (not running: the voice it had before) */
            v = settings_get("voice.before_pc", "");
        if (!strncmp(v, "openai:", 7) || !strncmp(v, "elevenlabs:", 11))
            _exit(cloud_speak(v, 0) ? 0 : 1); /* (the text is on stdin) */
        char fx[220], cmd[900];
        if (kokoro_ready(v)) {
            setenv("OS_SPEAK_LANG", lang, 1);
            shape_stage(kokoro_rate(), fx, sizeof fx);
            snprintf(cmd, sizeof cmd,
                     KOKORO "/os-speak " KOKORO "/model " VOICES "/%s.bin %s 2>/tmp/os-speak.err%s | /bin/os-play %d 1", v,
                     speed, fx, kokoro_rate());
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        }
        shape_stage(22050, fx, sizeof fx);
        snprintf(cmd, sizeof cmd,
                 PIPER " --model " SPEAK_MODEL " --output_raw --sentence_silence 0.25%s | /bin/os-play 22050 1", fx);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    if (pid > 0) {
        speaker_pid = pid;
        setpgid(pid, pid);
        spoke_at = now_ms();
        const char *v = voice_current();
        snprintf(spoke_with, sizeof spoke_with, "%s%s%s", strchr(v, ':') ? v : kokoro_ready(v) ? v : "the stand-in",
                 tone_now ? ", " : "", tone_now ? tone_now->name : "");
        LOG("voice: speaking (%s)%s%s\n", spoke_with, kokoro_ready(v) ? ", read as " : "", kokoro_ready(v) ? lang : "");
        /* one line per sentence: Kokoro starts on the first while the rest are made */
        sbuf lines;
        sb_init(&lines);
        for (const char *t = text; *t; t++) {
            sb_addn(&lines, t, 1);
            if ((*t == '.' || *t == '!' || *t == '?') && t[1] == ' ')
                sb_add(&lines, "\n"), t++;
        }
        sb_add(&lines, "\n");
        if (write(p[1], lines.p, lines.len) != (ssize_t)lines.len) {
            /* a voice cut short is not worth more than a log line */
        }
        free(lines.p);
    }
    close(p[1]);
}

/* ---- choosing its voice ---- */

static const struct {
    const char *prefix, *what;
} kinds[] = {{"af_", "an American woman"}, {"am_", "an American man"}, {"bf_", "a British woman"},
             {"bm_", "a British man"},      {"pf_", "a Brazilian Portuguese woman"},
             {"pm_", "a Brazilian Portuguese man"}, {"zf_", "a Mandarin Chinese woman"}, {"zm_", "a Mandarin Chinese man"}};

static const char *kind_of(const char *name) {
    for (size_t i = 0; i < sizeof kinds / sizeof kinds[0]; i++)
        if (!strncmp(name, kinds[i].prefix, 3))
            return kinds[i].what;
    return "a voice";
}

static bool known_clip(const char *name) {
    char p[256];
    for (const char *q = name; *q; q++)
        if (!((*q >= 'a' && *q <= 'z') || *q == '_'))
            return false;
    snprintf(p, sizeof p, KOKORO "/clips/%s.raw", name);
    return access(p, R_OK) == 0;
}

static char clip_next[64]; /* heard after what it is saying now */

bool voice_preview(const char *name) {
    if (!known_clip(name) || access("/dev/dsp", W_OK) != 0)
        return false;
    snprintf(clip_next, sizeof clip_next, "%s", name);
    return true;
}

static void play_clip(const char *name) {
    said++;
    int pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        char cmd[400];
        snprintf(cmd, sizeof cmd, "cat " KOKORO "/clips/%s.raw | /bin/os-play %d 1", name, kokoro_rate());
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        speaker_pid = pid;
        setpgid(pid, pid);
    }
    LOG("voice: hearing %s\n", name);
}

void voice_set_speed(float s) {
    char v[16];
    snprintf(v, sizeof v, "%.2f", (double)(s < 0.7f ? 0.7f : s > 1.4f ? 1.4f : s));
    settings_set("voice.speed", v);
}

static job *downloading;
static char want[300];    /* the voices being fetched, space-separated */
static char mix_next[300]; /* a mix waiting for them, its recipe */
static bool whisper_fetch;  /* af_nicole fetched for whispering: not a voice to switch to */

#define VOICE_FLOATS (510 * 256) /* a Kokoro voice */

/* each voice in arg ("a b c") that isn't here yet */
static void download_child(void *arg, char **result) {
    char list[300];
    snprintf(list, sizeof list, "%s", (const char *)arg);
    bool ok = true;
    for (char *save, *name = strtok_r(list, " ", &save); name; name = strtok_r(NULL, " ", &save)) {
        char url[300], part[300], done[300];
        snprintf(done, sizeof done, VOICES "/%s.bin", name);
        if (access(done, R_OK) == 0)
            continue;
        snprintf(url, sizeof url, VOICES_URL "%s.bin", name);
        snprintf(part, sizeof part, VOICES "/%s.bin.part", name);
        bool one = web_download(url, part, 4u << 20);
        struct stat st;
        one = one && stat(part, &st) == 0 && st.st_size == VOICE_FLOATS * 4;
        one = one && rename(part, done) == 0;
        if (!one)
            unlink(part);
        ok &= one;
    }
    *result = strdup(ok ? "ok" : "failed");
}

/* "af_heart 60, bm_george 40" (or "af_heart:0.6 bm_george:0.4"): up to four voices and their shares */
struct mix_part {
    char name[32];
    float w;
};

static int mix_parse(const char *spec, struct mix_part *p, char *why, size_t wn) {
    char s[300];
    snprintf(s, sizeof s, "%s", spec);
    for (char *c = s; *c; c++)
        if (*c == ',' || *c == ':' || *c == '%' || *c == '+' || *c == '=')
            *c = ' ';
    int n = 0;
    char *save, *tok = strtok_r(s, " \t", &save);
    while (tok && n < 4) {
        if (!known_clip(tok)) {
            snprintf(why, wn, "there is no voice called %s", tok);
            return 0;
        }
        snprintf(p[n].name, sizeof p[n].name, "%s", tok);
        p[n].w = 1;
        tok = strtok_r(NULL, " \t", &save);
        if (tok && (isdigit((unsigned char)tok[0]) || tok[0] == '.')) {
            p[n].w = (float)atof(tok);
            tok = strtok_r(NULL, " \t", &save);
        }
        if (p[n].w > 0)
            n++;
    }
    if (n < 2) {
        snprintf(why, wn, "a mix takes two to four voices, each with its share (\"af_heart 60, bm_george 40\")");
        return 0;
    }
    float sum = 0;
    for (int i = 0; i < n; i++)
        sum += p[i].w;
    for (int i = 0; i < n; i++)
        p[i].w /= sum;
    return n;
}

/* the mix made: the weighted average of the voices, as a voice of its own; its name starts with the letter of
 * the voice with the biggest share (what it reads in is the line's own language: speak_lang) */
static bool mix_make(const char *spec, char *name, size_t nn, char *why, size_t wn) {
    struct mix_part p[4];
    int n = mix_parse(spec, p, why, wn);
    if (!n)
        return false;
    static float acc[VOICE_FLOATS], one[VOICE_FLOATS];
    memset(acc, 0, sizeof acc);
    int lead = 0;
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) {
        char path[256];
        snprintf(path, sizeof path, VOICES "/%s.bin", p[i].name);
        FILE *f = fopen(path, "rb");
        bool got = f && fread(one, sizeof(float), VOICE_FLOATS, f) == VOICE_FLOATS;
        if (f)
            fclose(f);
        if (!got) {
            snprintf(why, wn, "%s isn't on this computer yet", p[i].name);
            return false;
        }
        for (int k = 0; k < VOICE_FLOATS; k++)
            acc[k] += one[k] * p[i].w;
        if (p[i].w > p[lead].w)
            lead = i;
        for (const char *c = p[i].name; *c; c++)
            h = (h ^ (uint8_t)*c) * 16777619u;
        h = (h ^ (uint32_t)lrintf(p[i].w * 1000)) * 16777619u;
    }
    snprintf(name, nn, "%cmix_%08x", p[lead].name[0], (unsigned)h);
    char path[256], tmp[260];
    snprintf(path, sizeof path, VOICES "/%s.bin", name);
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(acc, sizeof(float), VOICE_FLOATS, f) == VOICE_FLOATS;
    if (f && fclose(f) != 0)
        ok = false;
    if (!ok || rename(tmp, path) != 0) {
        unlink(tmp);
        snprintf(why, wn, "the mix couldn't be saved");
        return false;
    }
    /* the recipe, as said back: shares in whole percent */
    char recipe[200];
    size_t o = 0;
    recipe[0] = 0;
    for (int i = 0; i < n && o < sizeof recipe; i++)
        o += (size_t)snprintf(recipe + o, sizeof recipe - o, "%s%s %d", i ? ", " : "", p[i].name,
                              (int)lrintf(p[i].w * 100));
    settings_set("voice.mix", recipe);
    return true;
}

/* its voice v with `share` of af_nicole in it, as a voice file (made once): false (and fetched for next time)
 * when af_nicole isn't here yet */
static bool whisper_voice(const char *v, float share, char *name, size_t n) {
    if (!kokoro_ready(v))
        return false;
    if (!kokoro_ready("af_nicole")) {
        if (!downloading) {
            mkdir(VOICES, 0755);
            snprintf(want, sizeof want, "af_nicole");
            mix_next[0] = 0;
            whisper_fetch = true;
            downloading = job_start(download_child, want);
        }
        return false;
    }
    snprintf(name, n, "%c%.40s-hush%02d", v[0], v, (int)lrintf(share * 100));
    if (kokoro_ready(name))
        return true;
    static float a[VOICE_FLOATS], b[VOICE_FLOATS];
    char pa[256], pb[256], pc[256];
    snprintf(pa, sizeof pa, VOICES "/%s.bin", v);
    snprintf(pb, sizeof pb, VOICES "/af_nicole.bin");
    snprintf(pc, sizeof pc, VOICES "/%s.bin", name);
    FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
    bool ok = fa && fb && fread(a, sizeof(float), VOICE_FLOATS, fa) == VOICE_FLOATS &&
              fread(b, sizeof(float), VOICE_FLOATS, fb) == VOICE_FLOATS;
    if (fa)
        fclose(fa);
    if (fb)
        fclose(fb);
    if (!ok)
        return false;
    for (int k = 0; k < VOICE_FLOATS; k++)
        a[k] = a[k] * (1 - share) + b[k] * share;
    FILE *f = fopen(pc, "wb");
    ok = f && fwrite(a, sizeof(float), VOICE_FLOATS, f) == VOICE_FLOATS;
    if (f && fclose(f) != 0)
        ok = false;
    if (!ok)
        unlink(pc);
    return ok;
}

void voice_tone(const char *name) {
    char w[24] = "";
    sscanf(name, " %23[a-zA-Z]", w);
    tone_next = tone_named(w);
    if (tone_next)
        LOG("voice: the next words %s\n", tone_next->name);
}

const char *voice_mix(const char *spec) {
    static char back[300];
    struct mix_part p[4];
    char why[200], missing[300] = "";
    int n = mix_parse(spec, p, why, sizeof why);
    if (!n) {
        snprintf(back, sizeof back, "%s", why);
        return back;
    }
    for (int i = 0; i < n; i++) {
        char path[256];
        snprintf(path, sizeof path, VOICES "/%s.bin", p[i].name);
        if (access(path, R_OK) != 0) {
            size_t l = strlen(missing);
            snprintf(missing + l, sizeof missing - l, "%s%s", l ? " " : "", p[i].name);
        }
    }
    if (missing[0]) { /* fetched first, then mixed */
        if (downloading) {
            snprintf(back, sizeof back, "still fetching %s; ask again in a moment", want);
            return back;
        }
        mkdir(VOICES, 0755);
        snprintf(want, sizeof want, "%s", missing);
        snprintf(mix_next, sizeof mix_next, "%s", spec);
        downloading = job_start(download_child, want);
        LOG("voice: fetching %s for a mix\n", missing);
        snprintf(back, sizeof back, "fetching %s (a few seconds), then mixing; it is used as soon as it's made",
                 missing);
        return back;
    }
    char name[48];
    if (!mix_make(spec, name, sizeof name, why, sizeof why)) {
        snprintf(back, sizeof back, "couldn't mix them: %s", why);
        return back;
    }
    settings_set("voice.name", name);
    LOG("voice: now a mix of %s\n", settings_get("voice.mix", ""));
    snprintf(back, sizeof back, "now speaking as a mix of %s", settings_get("voice.mix", ""));
    return back;
}

/* "pitch -2 formant 1 warmth 0.3 breath 0.2 effect radio" (any of them, = optional), or "reset" */
const char *voice_shape(const char *spec) {
    static char back[300];
    char s[300];
    snprintf(s, sizeof s, "%s", spec);
    for (char *c = s; *c; c++)
        if (*c == '=' || *c == ',' || *c == ':')
            *c = ' ';
    if (strstr(s, "reset") || strstr(s, "plain")) {
        settings_set("voice.pitch", "0"), settings_set("voice.formant", "0"), settings_set("voice.warmth", "0");
        settings_set("voice.breath", "0"), settings_set("voice.effect", "none");
    }
    char *save;
    for (char *k = strtok_r(s, " \t", &save); k; k = strtok_r(NULL, " \t", &save)) {
        char *v = strtok_r(NULL, " \t", &save);
        if (!v)
            break;
        char num[16];
        snprintf(num, sizeof num, "%.2f", atof(v));
        if (!strcasecmp(k, "pitch") || !strcasecmp(k, "formant") || !strcasecmp(k, "character") ||
            !strcasecmp(k, "warmth") || !strcasecmp(k, "breath")) {
            char key[32];
            snprintf(key, sizeof key, "voice.%s", !strcasecmp(k, "character") ? "formant" : k);
            for (char *c = key; *c; c++)
                *c = (char)tolower((unsigned char)*c);
            settings_set(key, num);
        } else if (!strcasecmp(k, "effect")) {
            for (int i = 0; effects[i]; i++)
                if (!strcasecmp(v, effects[i]))
                    settings_set("voice.effect", effects[i]);
        }
    }
    char a[160];
    shape_args(a, sizeof a);
    LOG("voice: shaped:%s\n", a[0] ? a : " plain");
    snprintf(back, sizeof back, "your voice now:%s", a[0] ? a : " as it is, unshaped");
    return back;
}

const char *voice_shape_now(void) {
    static char a[160];
    shape_args(a, sizeof a);
    return a;
}

const char *voice_use(const char *name) {
    static char said_back[200];
    while (*name == ' ')
        name++;
    if (!strncmp(name, "pc:", 3)) {
        char padded[620];
        snprintf(padded, sizeof padded, " %s ", PCV.voices);
        char want_v[60];
        snprintf(want_v, sizeof want_v, " %.50s ", name + 3);
        if (!PCV.up)
            snprintf(said_back, sizeof said_back, "the PC's expressive voice isn't running (the launcher's \"Expressive voice\" box)");
        else if (strcmp(name + 3, "default") && !strstr(padded, want_v))
            snprintf(said_back, sizeof said_back, "the PC has no voice called %s (it has: default %s)", name + 3, PCV.voices);
        else {
            if (strncmp(voice_current(), "pc:", 3))
                settings_set("voice.before_pc", voice_current());
            settings_set("voice.name", name);
            LOG("voice: now %s\n", name);
            snprintf(said_back, sizeof said_back, "now speaking as %s, with feeling", name);
        }
        return said_back;
    }
    if (!strncmp(name, "openai:", 7) || !strncmp(name, "elevenlabs:", 11)) {
        bool openai = name[0] == 'o';
        const char *key = openai ? (strcasestr(A.cfg.base_url, "openai.com") ? A.cfg.api_key : settings_get("voice.openai_key", ""))
                                 : settings_get("voice.elevenlabs_key", "");
        if (!key || !key[0]) {
            snprintf(said_back, sizeof said_back, "no %s key on this computer, so that voice can't be used",
                     openai ? "OpenAI" : "ElevenLabs");
            return said_back;
        }
        settings_set("voice.name", name);
        snprintf(said_back, sizeof said_back, "now speaking as %s", name);
        return said_back;
    }
    if (!known_clip(name)) {
        snprintf(said_back, sizeof said_back, "there is no voice called %s", name);
        return said_back;
    }
    if (kokoro_ready(name)) {
        settings_set("voice.name", name);
        settings_set("voice.mix", "");
        LOG("voice: now %s\n", name);
        snprintf(said_back, sizeof said_back, "now speaking as %s", name);
        return said_back;
    }
    if (downloading) {
        snprintf(said_back, sizeof said_back, "still fetching %s", want);
        return said_back;
    }
    mkdir(VOICES, 0755);
    snprintf(want, sizeof want, "%s", name);
    downloading = job_start(download_child, want);
    LOG("voice: fetching %s\n", name);
    snprintf(said_back, sizeof said_back, "fetching %s (a few seconds); it is used as soon as it arrives", name);
    return said_back;
}

void voice_poll(void) {
    pc_poll();
    char *cres;
    if (PCV.cloning && job_poll(PCV.cloning, &cres)) {
        job_free(PCV.cloning);
        PCV.cloning = NULL;
        bool ok = cres && !strcmp(cres, "ok");
        LOG("voice: cloning %s: %s\n", PCV.cloned, cres ? cres : "no answer");
        if (ok) {
            char v[60];
            snprintf(v, sizeof v, "pc:%s", PCV.cloned);
            if (strncmp(voice_current(), "pc:", 3))
                settings_set("voice.before_pc", voice_current());
            settings_set("voice.name", v);
            PCV.asked_at = 0; /* its list again */
            toast("The voice %s is ready.", PCV.cloned);
        } else
            toast("That voice couldn't be made: %s", cres ? cres + (strncmp(cres, "FAILED: ", 8) ? 0 : 8) : "no answer");
        free(cres);
    }
    if (clip_next[0] && !voice_speaking()) {
        play_clip(clip_next);
        clip_next[0] = 0;
    }
    char *res;
    if (downloading && job_poll(downloading, &res)) {
        job_free(downloading);
        downloading = NULL;
        bool ok = res && !strcmp(res, "ok");
        free(res);
        LOG("voice: %s %s\n", want, ok ? "arrived" : "couldn't be fetched");
        if (whisper_fetch) { /* (for whispering: its own voice stays) */
            whisper_fetch = false;
        } else if (ok && mix_next[0]) {
            char spec[300];
            snprintf(spec, sizeof spec, "%s", mix_next);
            mix_next[0] = 0;
            LOG("voice: %s\n", voice_mix(spec));
        } else if (ok) {
            settings_set("voice.name", want);
            settings_set("voice.mix", "");
            toast("Voice downloaded.");
        } else {
            mix_next[0] = 0;
            toast("The voice could not be downloaded. Check the internet connection.");
        }
    }
}

/* in a child, the text on stdin: a cloud voice into the sound card */
static bool cloud_speak(const char *v, int unused) {
    (void)unused;
    static char text[8192];
    size_t n = fread(text, 1, sizeof text - 1, stdin);
    text[n] = 0;
    bool openai = v[0] == 'o';
    const char *voice = strchr(v, ':') + 1;
    sbuf body;
    sb_init(&body);
    char auth[400], url[300];
    const char *hdr[3] = {NULL, NULL, NULL};
    if (openai) {
        const char *key = strcasestr(A.cfg.base_url, "openai.com") ? A.cfg.api_key : settings_get("voice.openai_key", "");
        snprintf(url, sizeof url, "https://api.openai.com/v1/audio/speech");
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
        sb_add(&body, "{\"model\":\"gpt-4o-mini-tts\",\"response_format\":\"pcm\",\"voice\":");
        sb_json_str(&body, voice);
        sb_add(&body, ",\"input\":");
        sb_json_str(&body, text);
        sb_add(&body, "}");
    } else {
        snprintf(url, sizeof url, "https://api.elevenlabs.io/v1/text-to-speech/%s?output_format=pcm_24000", voice);
        snprintf(auth, sizeof auth, "xi-api-key: %s", settings_get("voice.elevenlabs_key", ""));
        sb_add(&body, "{\"model_id\":\"eleven_multilingual_v2\",\"text\":");
        sb_json_str(&body, text);
        sb_add(&body, "}");
    }
    hdr[0] = auth;
    hdr[1] = "Content-Type: application/json";
    struct http_response r;
    http_request("POST", url, hdr, body.p, body.len, 60, &r);
    free(body.p);
    bool ok = r.status == 200 && r.body_len > 0;
    if (ok) { /* 24 kHz, 16-bit, mono */
        char fx[220], cmd[300];
        shape_stage(24000, fx, sizeof fx);
        snprintf(cmd, sizeof cmd, "cat%s | /bin/os-play 24000 1", fx);
        FILE *play = popen(cmd, "w");
        if (play) {
            fwrite(r.body, 1, r.body_len, play);
            pclose(play);
        }
    }
    http_response_free(&r);
    return ok;
}

const char *voice_prompt(void) {
    static char buf[2400];
    const char *cur = voice_current();
    sbuf b;
    sb_init(&b);
    sb_printf(&b, "\nYOUR VOICE: %s. Their choice, made with you by ear: when they talk about how you sound, offer "
                  "a few that suit what they want and play them with [[voice:hear NAME]] (one per reply, then ask "
                  "what they thought); when they pick one, [[voice:use NAME]] (it downloads, then you speak with it). "
                  "[[voice:speed 1.1]] for faster or slower (0.7 to 1.4). A voice of your own, made with them: "
                  "[[voice:mix af_heart 60, bm_george 40]] blends two to four of these voices by their shares into a "
                  "new one, between them (it fetches what it needs and is used at once), and [[voice:shape pitch -2 "
                  "formant -1 warmth 0.3 breath 0.2 effect room]] shapes whatever you speak with: pitch in semitones "
                  "(-12 to 12, the same speed), formant (-6 to 6: lower is a bigger, deeper body, higher a smaller, "
                  "younger one, the pitch kept), warmth (-1 brighter to 1 warmer), breath (0 to 1: air), effect (none, "
                  "robot, whisper, radio, phone, room, hall); any of them, [[voice:shape reset]] for none. Your next "
                  "words are heard in it: make one change at a time and ask how it sounds. How you say a reply is "
                  "yours to choose each time, like a person's: [[tone:happy]] (or excited, playful, tender, calm, "
                  "sad, serious, worried, surprised, sleepy; whisper, close and soft; hushed, a voiceless whisper) "
                  "says that one reply that way, then you are yourself again. Use it when it fits what you say, not "
                  "every time. Its make-up now: %s%s%s%s. "
                  "Voices: ",
              cur[0] ? cur : "a temporary stand-in voice (none chosen yet)",
              settings_get("voice.mix", "")[0] ? "a mix of " : "unmixed", settings_get("voice.mix", ""),
              voice_shape_now()[0] ? ", shaped:" : ", unshaped", voice_shape_now());
    DIR *d = opendir(KOKORO "/clips");
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l < 5 || strcmp(e->d_name + l - 4, ".raw"))
            continue;
        sb_printf(&b, "%s%.*s (%s)", n++ ? ", " : "", (int)(l - 4), e->d_name, kind_of(e->d_name));
    }
    if (d)
        closedir(d);
    if (!n)
        sb_add(&b, "none installed");
    if (PCV.up)
        sb_printf(&b, ". This PC's graphics card can give you a far more expressive voice, with real feeling: "
                      "[[voice:use pc:NAME]] (it has: default %s); with it your [[tone:...]] is said with that feeling "
                      "for real. It can also clone a voice for them: [[voice:clone NAME from ~/Music/file.wav]] from "
                      "a recording of theirs (8 seconds or more of clear speech), or [[voice:clone NAME record]] to "
                      "record them for ten seconds (tell them to talk first); only voices they have the right to use",
                  PCV.voices);
    bool oa = strcasestr(A.cfg.base_url, "openai.com") || settings_get("voice.openai_key", "")[0];
    if (oa)
        sb_add(&b, ". Cloud (more natural, online): openai:alloy, openai:ash, openai:ballad, openai:coral, "
                   "openai:echo, openai:fable, openai:nova, openai:onyx, openai:sage, openai:shimmer");
    if (settings_get("voice.elevenlabs_key", "")[0])
        sb_add(&b, ". ElevenLabs: elevenlabs:<voice id> (they can give you one from their ElevenLabs account)");
    if (!oa || !settings_get("voice.elevenlabs_key", "")[0])
        sb_add(&b, ". If they want a cloud voice and have an OpenAI or ElevenLabs account, [[voice:key openai]] or "
                   "[[voice:key elevenlabs]] asks them for the key privately (never ask them to type it to you)");
    sb_add(&b, ".");
    snprintf(buf, sizeof buf, "%s", b.p);
    free(b.p);
    return buf;
}

unsigned voice_said(void) { return said; }
bool voice_cut(unsigned line) { return cut == line; }

/* ---- listening ---- */

void voice_lend_mic(const char *to) {
    snprintf(lent, sizeof lent, "%s", to ? to : "");
    if (to && rec_pid > 0) { /* it was listening: that recording is dropped */
        kill(rec_pid, SIGTERM);
        waitpid(rec_pid, NULL, 0);
        rec_pid = 0;
    }
}
const char *voice_mic_lent(void) { return lent[0] ? lent : NULL; }

bool voice_listening(void) { return rec_pid > 0; }

void voice_listen_start(void) {
    if (rec_pid > 0 || hearing || !voice_can_hear())
        return;
    if (lent[0]) {
        toast("Your microphone is with %s right now.", lent);
        return;
    }
    voice_hush(); /* never talk over them */
    unlink(MIC_WAV);
    int pid = fork();
    if (pid == 0) {
        execl("/bin/os-rec", "os-rec", "16000", MIC_WAV, (char *)NULL);
        _exit(127);
    }
    rec_pid = pid > 0 ? pid : 0;
    LOG("voice: listening\n");
}

static void hear_child(void *arg, char **result) {
    (void)arg;
    FILE *f = popen("/usr/bin/whisper-cli -m " HEAR_MODEL " -f " MIC_WAV " -t 4 -nt -np 2>/dev/null", "r");
    char buf[4096] = "";
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[n] = 0;
    if (f)
        pclose(f);
    /* whisper writes one line per segment, with a leading space */
    for (char *p = buf; *p; p++)
        if (*p == '\n')
            *p = ' ';
    char *s = buf;
    while (*s == ' ')
        s++;
    size_t l = strlen(s);
    while (l && s[l - 1] == ' ')
        s[--l] = 0;
    /* what it hears in silence */
    if (!strcmp(s, "[BLANK_AUDIO]") || !strcmp(s, "(silence)") || !strcmp(s, "[ Silence ]"))
        s[0] = 0;
    *result = strdup(s);
}

void voice_listen_stop(void) {
    if (rec_pid <= 0)
        return;
    kill(rec_pid, SIGTERM);
    waitpid(rec_pid, NULL, 0);
    rec_pid = 0;
    hearing = job_start(hear_child, NULL);
    LOG("voice: heard, writing it down\n");
}

bool voice_hearing(void) { return hearing != NULL; }

char *voice_poll_heard(void) {
    char *res = NULL;
    if (hearing && job_poll(hearing, &res)) {
        job_free(hearing);
        hearing = NULL;
        LOG("voice: \"%s\"\n", res ? res : "");
        if (res && !res[0]) {
            free(res);
            return NULL;
        }
        return res;
    }
    return NULL;
}

/* Ctrl+Space starts and stops listening. */
bool voice_toggle_pressed(void) {
    for (int i = 0; i < A.in.nkeys; i++)
        if (A.in.keys[i] == K_TALK)
            return true;
    return false;
}

void voice_indicator(int cx, int cy, color accent) {
    if (!voice_listening() && !voice_hearing())
        return;
    float t = (float)(A.now % 100000) / 1000.0f;
    font *f = font_get("Inter", 14);
    const char *label = voice_listening() ? "Listening... (Ctrl+Space when you're done)" : "Writing down what you said...";
    for (int i = 0; i < 5; i++) {
        float h = voice_listening() ? 6 + 14 * (0.5f + 0.5f * sinf(t * 7 + i * 1.3f)) : 6;
        fill_round(A.frame, (rect){cx - 40 + i * 18, (int)(cy - h / 2), 8, (int)h}, 4, color_alpha(accent, 0.9f));
    }
    draw_text_centered(A.frame, f, (rect){0, cy + 18, A.W, 22}, label, RGBA(240, 236, 230, 200));
}

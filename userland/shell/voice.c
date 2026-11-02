#define _GNU_SOURCE
#include "voice.h"
#include "app.h"
#include "settings.h"
#include "web.h"
#include "../lib/http.h"
#include "../lib/json.h"
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

/* The companion's voice and ears, on this computer: speech comes from Piper
 * into the sound card, and what they say is recorded and written down by
 * whisper.cpp. Nothing leaves the machine for either. Every word spoken is
 * also on screen, so the voice is never the only way in. */

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
static char spoke_with[64];
static bool cloud_speak(const char *v, int unused);
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

bool voice_can_speak(void) {
    if (access("/dev/dsp", W_OK) != 0)
        return false;
    const char *v = voice_current();
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

void voice_say(const char *text) {
    if (!enabled || !text || !*text || !voice_can_speak())
        return;
    voice_hush(); /* the newest words win */
    said++;
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
        char speed[16];
        snprintf(speed, sizeof speed, "%s", settings_get("voice.speed", "1.0"));
        if (!strncmp(v, "openai:", 7) || !strncmp(v, "elevenlabs:", 11))
            _exit(cloud_speak(v, 0) ? 0 : 1); /* (the text is on stdin) */
        if (kokoro_ready(v)) {
            char cmd[600];
            snprintf(cmd, sizeof cmd,
                     KOKORO "/os-speak " KOKORO "/model " VOICES "/%s.bin %s 2>/tmp/os-speak.err | /bin/os-play %d 1", v,
                     speed, kokoro_rate());
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        }
        execl("/bin/sh", "sh", "-c",
              PIPER " --model " SPEAK_MODEL " --output_raw --sentence_silence 0.25 | /bin/os-play 22050 1",
              (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    if (pid > 0) {
        speaker_pid = pid;
        setpgid(pid, pid);
        spoke_at = now_ms();
        const char *v = voice_current();
        snprintf(spoke_with, sizeof spoke_with, "%s", strchr(v, ':') ? v : kokoro_ready(v) ? v : "the stand-in");
        LOG("voice: speaking (%s)\n", spoke_with);
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
static char want[64];

static void download_child(void *arg, char **result) {
    const char *name = arg;
    char url[300], part[300], done[300];
    snprintf(url, sizeof url, VOICES_URL "%s.bin", name);
    snprintf(part, sizeof part, VOICES "/%s.bin.part", name);
    snprintf(done, sizeof done, VOICES "/%s.bin", name);
    bool ok = web_download(url, part, 4u << 20);
    struct stat st;
    ok = ok && stat(part, &st) == 0 && st.st_size == 510 * 256 * 4; /* a voice: 510 x 256 numbers */
    ok = ok && rename(part, done) == 0;
    if (!ok)
        unlink(part);
    *result = strdup(ok ? "ok" : "failed");
}

const char *voice_use(const char *name) {
    static char said_back[200];
    while (*name == ' ')
        name++;
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
        if (ok) {
            settings_set("voice.name", want);
            toast("Voice downloaded.");
        } else {
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
        FILE *play = popen("/bin/os-play 24000 1", "w");
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
                  "[[voice:speed 1.1]] for faster or slower (0.7 to 1.4). Voices: ",
              cur[0] ? cur : "a temporary stand-in voice (none chosen yet)");
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

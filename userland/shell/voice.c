#define _GNU_SOURCE
#include "voice.h"
#include "app.h"

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
#define PIPER "/usr/bin/piper" /* a wrapper that tells it where its libraries are */
#define SPEAK_MODEL VOICE_DIR "/en_US-amy-medium.onnx"
#define HEAR_MODEL VOICE_DIR "/ggml-base.en.bin"
#define MIC_WAV "/tmp/os-mic.wav"

static int speaker_pid, rec_pid;
static job *hearing;
static bool enabled = true;

bool voice_can_speak(void) { return access("/dev/dsp", W_OK) == 0 && access(PIPER, X_OK) == 0 && access(SPEAK_MODEL, R_OK) == 0; }
bool voice_can_hear(void) { return access("/dev/dsp", R_OK) == 0 && access("/usr/bin/whisper-cli", X_OK) == 0 && access(HEAR_MODEL, R_OK) == 0; }
void voice_set_enabled(bool on) {
    enabled = on;
    if (!on)
        voice_hush();
}
bool voice_enabled(void) { return enabled; }

void voice_hush(void) {
    if (speaker_pid > 0) {
        kill(-speaker_pid, SIGTERM);
        waitpid(speaker_pid, NULL, WNOHANG);
        speaker_pid = 0;
    }
}

bool voice_speaking(void) {
    if (speaker_pid > 0 && waitpid(speaker_pid, NULL, WNOHANG) == speaker_pid)
        speaker_pid = 0;
    return speaker_pid > 0;
}

void voice_say(const char *text) {
    if (!enabled || !text || !*text || !voice_can_speak())
        return;
    voice_hush(); /* the newest words win */
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
        execl("/bin/sh", "sh", "-c",
              PIPER " --model " SPEAK_MODEL " --output_raw --sentence_silence 0.25 | /bin/os-play 22050 1",
              (char *)NULL);
        _exit(127);
    }
    close(p[0]);
    if (pid > 0) {
        speaker_pid = pid;
        setpgid(pid, pid);
        /* one line per sentence keeps Piper's pauses natural */
        size_t n = strlen(text);
        if (write(p[1], text, n) != (ssize_t)n) {
            /* a voice cut short is not worth more than a log line */
        }
        if (write(p[1], "\n", 1) < 0) {
        }
    }
    close(p[1]);
}

/* ---- listening ---- */

bool voice_listening(void) { return rec_pid > 0; }

void voice_listen_start(void) {
    if (rec_pid > 0 || hearing || !voice_can_hear())
        return;
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

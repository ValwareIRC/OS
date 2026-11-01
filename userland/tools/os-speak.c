/* os-speak: the companion's voice, Kokoro through sherpa-onnx (Apache-2.0),
 * on this computer. It keeps the model loaded and says each line it reads,
 * writing 16-bit mono PCM to stdout as it goes (into os-play), so a reply is
 * heard while the rest is still being made.
 *
 *   os-speak DIR VOICE [SPEED]                 lines on stdin -> PCM on stdout
 *   os-speak DIR VOICE SPEED --say TEXT OUT    one clip into OUT (raw PCM)
 *   os-speak DIR --rate VOICE                  the sample rate it speaks at
 *
 * DIR holds the model (model.onnx, tokens.txt, espeak-ng-data, lexicons,
 * dict); VOICE is one voice file (510 x 256 floats, as Kokoro's voices are
 * published one by one) or a voices.bin of many with ":N" for which one.
 * The language follows the voice's name: a/b English, p Portuguese, z Chinese.
 * sherpa-onnx 1.13 (2.x has no espeak-ng). Built against glibc (it runs on the base system): ports/build-kokoro.sh. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sherpa-onnx/c-api/c-api.h"

/* sherpa-onnx wants every speaker the model has in its voices file; one voice
 * downloaded alone goes into the first place of a bank that is otherwise empty
 * (sparse, in /tmp, gone as soon as it is read). */
#define VOICE_BYTES (510 * 256 * 4)
#define SPEAKERS 54

static bool bank_of_one(const char *voice, char *out, size_t n) {
    struct stat st;
    if (stat(voice, &st) != 0 || st.st_size != VOICE_BYTES)
        return false;
    static char buf[VOICE_BYTES];
    int in = open(voice, O_RDONLY);
    bool ok = in >= 0 && read(in, buf, sizeof buf) == (ssize_t)sizeof buf;
    if (in >= 0)
        close(in);
    snprintf(out, n, "/tmp/os-speak-%d.bin", (int)getpid());
    int fd = ok ? open(out, O_WRONLY | O_CREAT | O_TRUNC, 0600) : -1;
    ok = fd >= 0 && write(fd, buf, sizeof buf) == (ssize_t)sizeof buf &&
         ftruncate(fd, (off_t)VOICE_BYTES * SPEAKERS) == 0;
    if (fd >= 0)
        close(fd);
    return ok;
}

static void path(char *out, size_t n, const char *dir, const char *name) { snprintf(out, n, "%s/%s", dir, name); }

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: os-speak DIR VOICE [SPEED] [--say TEXT OUT] | os-speak DIR --rate\n");
        return 2;
    }
    const char *dir = argv[1];
    char model[512], tokens[512], data[512], lex[1100], dict[512], fsts[1600];
    path(model, sizeof model, dir, "model.onnx");
    path(tokens, sizeof tokens, dir, "tokens.txt");
    path(data, sizeof data, dir, "espeak-ng-data");
    path(dict, sizeof dict, dir, "dict");
    snprintf(fsts, sizeof fsts, "%s/date-zh.fst,%s/number-zh.fst,%s/phone-zh.fst", dir, dir, dir);

    char voices[512];
    int sid = 0;
    const char *voice = argv[2];
    bool rate_only = !strcmp(voice, "--rate");
    if (!rate_only) {
        snprintf(voices, sizeof voices, "%s", voice);
        char *colon = strrchr(voices, ':');
        if (colon && colon > strrchr(voices, '/')) {
            sid = atoi(colon + 1);
            *colon = 0;
        }
    } else {
        snprintf(voices, sizeof voices, "%s", argc > 3 ? argv[3] : "voices.bin"); /* any voice will do */
    }
    /* the language, from the voice's file name (af_heart.bin, pf_dora.bin, zf_xiaoxiao.bin) */
    const char *base = strrchr(voices, '/');
    base = base ? base + 1 : voices;
    const char *lang = base[0] == 'p' ? "pt-br" : base[0] == 'z' ? "zh" : base[0] == 'b' ? "en" : "en-us";
    if (getenv("OS_SPEAK_LANG")) /* a voices.bin of many says nothing of the language: told instead */
        lang = getenv("OS_SPEAK_LANG");
    /* words in the lexicon are said as listed; the rest through espeak-ng in the voice's language */
    if (!strcmp(lang, "zh"))
        snprintf(lex, sizeof lex, "%s/lexicon-zh.txt,%s/lexicon-us-en.txt", dir, dir);
    else if (!strcmp(lang, "en"))
        snprintf(lex, sizeof lex, "%s/lexicon-gb-en.txt", dir);
    else if (!strcmp(lang, "en-us"))
        snprintf(lex, sizeof lex, "%s/lexicon-us-en.txt", dir);
    else
        lex[0] = 0;

    char bank[64] = "";
    if (bank_of_one(voices, bank, sizeof bank))
        snprintf(voices, sizeof voices, "%s", bank), sid = 0;

    SherpaOnnxOfflineTtsConfig cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.model.kokoro.model = model;
    cfg.model.kokoro.voices = voices;
    cfg.model.kokoro.tokens = tokens;
    cfg.model.kokoro.data_dir = data;
    cfg.model.kokoro.lexicon = lex;
    cfg.model.kokoro.dict_dir = dict;
    cfg.model.kokoro.lang = lang;
    cfg.model.kokoro.length_scale = 1.0f;
    cfg.model.num_threads = 4;
    cfg.model.provider = "cpu";
    cfg.rule_fsts = !strcmp(lang, "zh") ? fsts : "";
    cfg.max_num_sentences = 1;
    const SherpaOnnxOfflineTts *tts = SherpaOnnxCreateOfflineTts(&cfg);
    if (bank[0])
        unlink(bank);
    if (!tts) {
        fprintf(stderr, "os-speak: the voice couldn't start (model in %s, voice %s)\n", dir, voices);
        return 1;
    }
    int rate = SherpaOnnxOfflineTtsSampleRate(tts);
    if (rate_only) {
        printf("%d\n", rate);
        return 0;
    }
    float speed = argc > 3 && argv[3][0] != '-' ? strtof(argv[3], NULL) : 1.0f;
    if (speed < 0.5f || speed > 2.0f)
        speed = 1.0f;

    const char *say = NULL, *out_path = NULL;
    for (int i = 3; i + 2 < argc + 1; i++)
        if (!strcmp(argv[i], "--say") && i + 2 < argc)
            say = argv[i + 1], out_path = argv[i + 2];

    FILE *out = say ? fopen(out_path, "wb") : stdout;
    if (!out)
        return 1;
    char line[8192];
    for (;;) {
        const char *text;
        if (say) {
            text = say;
        } else {
            if (!fgets(line, sizeof line, stdin))
                break;
            line[strcspn(line, "\r\n")] = 0;
            if (!line[0])
                continue;
            text = line;
        }
        const SherpaOnnxGeneratedAudio *a = SherpaOnnxOfflineTtsGenerate(tts, text, sid, speed);
        if (a) {
            for (int i = 0; i < a->n; i++) {
                float s = a->samples[i];
                s = s > 1 ? 1 : s < -1 ? -1 : s;
                short v = (short)(s * 32767);
                fwrite(&v, 2, 1, out);
            }
            /* a breath between sentences */
            short zero = 0;
            for (int i = 0; i < rate / 5; i++)
                fwrite(&zero, 2, 1, out);
            fflush(out);
            SherpaOnnxDestroyOfflineTtsGeneratedAudio(a);
        }
        if (say)
            break;
    }
    if (say)
        fclose(out);
    SherpaOnnxDestroyOfflineTts(tts);
    return 0;
}

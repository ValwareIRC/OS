/* os-play and os-rec: raw PCM to and from the sound card (/dev/dsp).
 *
 *   os-play RATE [CHANNELS]      plays signed 16-bit little-endian PCM from stdin,
 *                                   and measures it as it goes (/run/os-lip: how
 *                                   loud each 10 ms is and how much of it is in
 *                                   two vowel bands, stamped with when it's
 *                                   heard) so the companion's mouth can follow
 *   os-rec RATE FILE.wav         records 16-bit mono until SIGTERM/SIGINT, then
 *                                   finishes the WAV header (for speech recognition)
 *
 * One binary; it looks at the name it was started as. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define SNDCTL_DSP_SPEED 0xC0045002
#define SNDCTL_DSP_SETFMT 0xC0045005
#define SNDCTL_DSP_CHANNELS 0xC0045006
#define SNDCTL_DSP_SYNC 0x00005001
#define AFMT_S16_LE 0x10

static volatile sig_atomic_t stop;
static void on_stop(int s) { (void)s, stop = 1; }

static int open_dsp(int mode, int rate, int channels) {
    int fd = open("/dev/dsp", mode);
    if (fd < 0) {
        int e = errno;
        if (e != EBUSY) /* busy: the caller has somewhere else to go */
            fprintf(stderr, "os-audio: /dev/dsp: %s\n", strerror(e));
        errno = e;
        return -1;
    }
    int fmt = AFMT_S16_LE;
    ioctl(fd, SNDCTL_DSP_SETFMT, &fmt);
    ioctl(fd, SNDCTL_DSP_CHANNELS, &channels);
    ioctl(fd, SNDCTL_DSP_SPEED, &rate);
    return fd;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

static void wav_header(uint8_t h[44], int rate, uint32_t bytes) {
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);
    put16(h + 22, 1);
    put32(h + 24, (uint32_t)rate);
    put32(h + 28, (uint32_t)rate * 2);
    put16(h + 32, 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, bytes);
}

/* ---- the lip-sync meter ---- */

#define LIP_PATH "/run/os-lip"
#define LIP_N 512
struct lip_entry {
    double t;
    float level, f1, f2;
};
struct lip_file {
    uint32_t magic, next;
    struct lip_entry e[LIP_N];
};

static struct lip_file *lip;

typedef struct {
    float b0, b1, b2, a1, a2, x1, x2, y1, y2;
} biquad;

static void bandpass(biquad *f, float f0, float q, float rate) {
    float w = 2 * 3.14159265f * f0 / rate, al = sinf(w) / (2 * q), a0 = 1 + al;
    *f = (biquad){al / a0, 0, -al / a0, -2 * cosf(w) / a0, (1 - al) / a0, 0, 0, 0, 0};
}

static inline float run(biquad *f, float x) {
    float y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1, f->x1 = x, f->y2 = f->y1, f->y1 = y;
    return y;
}

static double mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void lip_open(void) {
    int fd = open(LIP_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (ftruncate(fd, sizeof(struct lip_file)) == 0) {
        void *m = mmap(NULL, sizeof(struct lip_file), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m != MAP_FAILED) {
            lip = m;
            lip->magic = 0x4c495031u; /* "LIP1" */
        }
    }
    close(fd);
}

static struct {
    biquad lo, hi;
    double s, s1, s2;
    int n, block, rate, channels;
    double start;   /* when the first sample was handed to the card */
    long played;    /* frames handed over so far */
} M;

/* every 10 ms of what is played: how loud, and how much in each band */
static void measure(const int16_t *pcm, size_t frames) {
    for (size_t i = 0; i < frames; i++) {
        float x = 0;
        for (int c = 0; c < M.channels; c++)
            x += pcm[i * M.channels + c];
        x /= M.channels;
        float a = run(&M.lo, x), b = run(&M.hi, x);
        M.s += (double)x * x, M.s1 += (double)a * a, M.s2 += (double)b * b;
        if (++M.n < M.block)
            continue;
        double rms = sqrt(M.s / M.n), r1 = sqrt(M.s1 / M.n), r2 = sqrt(M.s2 / M.n);
        float db = rms > 1 ? 20 * (float)log10(rms / 32768.0) : -90;
        float level = (db + 50) / 40, f1 = rms > 1 ? (float)(r1 / rms) * 1.2f : 0, f2 = rms > 1 ? (float)(r2 / rms) * 1.6f : 0;
        if (lip) {
            /* heard when the card gets to it: a tenth of a second after it's handed over, about */
            struct lip_entry *e = &lip->e[lip->next % LIP_N];
            e->t = M.start + (double)(M.played + (long)i) / M.rate + 0.1;
            e->level = level < 0 ? 0 : level > 1 ? 1 : level;
            e->f1 = f1 > 1 ? 1 : f1;
            e->f2 = f2 > 1 ? 1 : f2;
            __atomic_store_n(&lip->next, lip->next + 1, __ATOMIC_RELEASE);
        }
        M.s = M.s1 = M.s2 = 0, M.n = 0;
    }
}

static int play(int rate, int channels) {
    if (channels < 1 || channels > 8)
        channels = 1;
    M.rate = rate, M.channels = channels, M.block = rate / 100 > 0 ? rate / 100 : 1;
    bandpass(&M.lo, 550, 0.7f, (float)rate);
    bandpass(&M.hi, 1900, 0.9f, (float)rate);
    lip_open();
    int fd = open_dsp(O_WRONLY, rate, channels);
    pid_t pa = 0;
    if (fd < 0 && errno == EBUSY && access("/usr/bin/pacat", X_OK) == 0) {
        /* apps' sound server has the card: play through it, mixed with theirs */
        int p[2];
        if (pipe(p) == 0 && (pa = fork()) == 0) {
            dup2(p[0], 0);
            close(p[0]), close(p[1]);
            char r[16], c[16];
            snprintf(r, sizeof r, "--rate=%d", rate);
            snprintf(c, sizeof c, "--channels=%d", channels);
            setenv("PULSE_SERVER", "unix:/run/user/1000/pulse/native", 0);
            execl("/usr/bin/pacat", "pacat", "--playback", "--raw", "--format=s16le", r, c, (char *)NULL);
            _exit(127);
        }
        if (pa > 0) {
            close(p[0]);
            fd = p[1];
        }
    }
    if (fd < 0)
        return 1;
    static char buf[16384];
    size_t have = 0; /* a sample split between reads waits for its other half */
    ssize_t n;
    while ((n = read(0, buf + have, sizeof buf - have)) > 0) {
        size_t total = have + (size_t)n, whole = total - total % (2 * (size_t)channels);
        if (!M.start)
            M.start = mono();
        measure((const int16_t *)buf, whole / (2 * channels));
        for (size_t off = 0; off < whole;) {
            ssize_t w = write(fd, buf + off, whole - off);
            if (w <= 0)
                return 1;
            off += (size_t)w;
        }
        M.played += (long)(whole / (2 * channels));
        /* the card takes it at its own pace: keep the clock honest */
        double ahead = M.start + (double)M.played / rate - mono();
        if (ahead < -0.25) /* we fell behind (a gap in the input): start counting again */
            M.start = mono() - (double)M.played / rate;
        have = total - whole;
        memmove(buf, buf + whole, have);
    }
    if (pa > 0) {
        close(fd);
        waitpid(pa, NULL, 0);
        return 0;
    }
    ioctl(fd, SNDCTL_DSP_SYNC, 0);
    close(fd);
    return 0;
}

static int rec(int rate, const char *path) {
    int fd = open_dsp(O_RDONLY, rate, 1);
    if (fd < 0)
        return 1;
    FILE *f = fopen(path, "wb");
    if (!f)
        return 1;
    uint8_t h[44];
    wav_header(h, rate, 0);
    fwrite(h, 1, sizeof h, f);
    signal(SIGTERM, on_stop);
    signal(SIGINT, on_stop);
    uint32_t total = 0;
    static char buf[8192];
    while (!stop) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        fwrite(buf, 1, (size_t)n, f);
        total += (uint32_t)n;
    }
    wav_header(h, rate, total);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
    fclose(f);
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    const char *name = strrchr(argv[0], '/') ? strrchr(argv[0], '/') + 1 : argv[0];
    if (!strcmp(name, "os-rec")) {
        if (argc < 3)
            return 2;
        return rec(atoi(argv[1]), argv[2]);
    }
    return play(argc > 1 ? atoi(argv[1]) : 22050, argc > 2 ? atoi(argv[2]) : 1);
}

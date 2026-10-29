/* os-play and os-rec: raw PCM to and from the sound card (/dev/dsp).
 *
 *   os-play RATE [CHANNELS]      plays signed 16-bit little-endian PCM from stdin
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
#include <sys/ioctl.h>
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

static int play(int rate, int channels) {
    int fd = open_dsp(O_WRONLY, rate, channels);
    if (fd < 0 && errno == EBUSY && access("/usr/bin/pacat", X_OK) == 0) {
        /* apps' sound server has the card: play through it, mixed with theirs */
        char r[16], c[16];
        snprintf(r, sizeof r, "--rate=%d", rate);
        snprintf(c, sizeof c, "--channels=%d", channels);
        setenv("PULSE_SERVER", "unix:/run/user/1000/pulse/native", 0);
        execl("/usr/bin/pacat", "pacat", "--playback", "--raw", "--format=s16le", r, c, (char *)NULL);
    }
    if (fd < 0)
        return 1;
    static char buf[16384];
    ssize_t n;
    while ((n = read(0, buf, sizeof buf)) > 0) {
        for (ssize_t off = 0; off < n;) {
            ssize_t w = write(fd, buf + off, (size_t)(n - off));
            if (w <= 0)
                return 1;
            off += w;
        }
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

/* Plays the shell's part with os-figure, natively: shared memory for the
 * frames, commands down a pipe, frames announced back. Saves a strip of
 * frames, checks the sandbox is on, swaps the look part way, and reports
 * the frame rate.
 *   build/host/figure_ipc build/userland/os-figure OUT.jpg */
#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define NSLOT 3
#define W 256
#define H 256

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int to_fig;
static void send(const char *s) {
    if (write(to_fig, s, strlen(s)) < 0)
        perror("write");
}

int main(int argc, char **argv) {
    const char *exe = argc > 1 ? argv[1] : "build/userland/os-figure";
    const char *out = argc > 2 ? argv[2] : "figure_ipc.jpg";
    size_t slot = (size_t)W * H * 4;
    int mfd = memfd_create("figure-frames", 0);
    if (mfd < 0 || ftruncate(mfd, (off_t)(slot * NSLOT)) < 0)
        return 1;
    uint8_t *ring = mmap(NULL, slot * NSLOT, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    int a[2], b[2];
    if (pipe(a) || pipe(b))
        return 1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(a[0], 0);
        dup2(b[1], 1);
        dup2(mfd, 3);
        int vfd = getenv("FIG_VRM") ? open(getenv("FIG_VRM"), O_RDONLY) : -1;
        if (vfd >= 0 && vfd != 4)
            dup2(vfd, 4), close(vfd);
        for (int fd = vfd >= 0 ? 5 : 4; fd < 64; fd++)
            close(fd);
        char sb[32], ns[8];
        snprintf(sb, sizeof sb, "%zu", slot);
        snprintf(ns, sizeof ns, "%d", NSLOT);
        execl(exe, "os-figure", sb, ns, (char *)NULL);
        _exit(127);
    }
    close(a[0]), close(b[1]);
    to_fig = a[1];
    FILE *in = fdopen(b[0], "r");
    char line[512];
    double t0 = now();
    while (fgets(line, sizeof line, in) && strncmp(line, "ready", 5))
        printf("figure says: %s", line);
    if (strncmp(line, "ready", 5)) {
        fprintf(stderr, "no ready: %s\n", line);
        return 1;
    }
    printf("ready after %.0f ms\n", (now() - t0) * 1000);
    /* the sandbox */
    char path[64], st[4096];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    FILE *sf = fopen(path, "r");
    if (sf) {
        while (fgets(st, sizeof st, sf))
            if (!strncmp(st, "Seccomp:", 8) || !strncmp(st, "NoNewPrivs:", 11))
                printf("%s", st);
        fclose(sf);
    }
    send("light fff6ee c4c8e6 8f9fe8\nview 256 256 bust\n");
    /* a strip: 8 frames, one every ~0.45 s, while things happen */
    enum { STRIP = 8 };
    uint32_t *strip = calloc((size_t)W * H * STRIP, 4);
    int frames = 0, saved = 0, shown = -1;
    double start = now(), next_save = start + 0.2;
    bool waved = false, happy = false, spoke = false, looked = false, swapped = false;
    while (now() - start < 4.4 && saved < STRIP) {
        double t = now() - start;
        if (!waved && t > 0.3)
            send("act wave\npointer 0.6 0.3\n"), waved = true;
        if (!happy && t > 1.6)
            send("mood happy\n"), happy = true;
        if (!spoke && t > 2.4)
            send("speaking 1\nspeak 0.7 0.8 0.3\n"), spoke = true;
        if (!looked && t > 3.0)
            send("speak 0 0 0\nspeaking 0\npointer -0.7 -0.2\nact nod\n"), looked = true;
        if (!swapped && t > 3.5) {
            send("look-begin\n<figure name=\"test\"><hair style=\"long\" length=\"0.2\" color=\"#c9b8f0 #8fd0ff\"/>"
                 "<outfit top=\"tee\" bottom=\"shorts\" colors=\"#2c2a4a #6a8ad8 #f4f2f8 #e85a8a #ffd95a\" "
                 "patterns=\"stars none\"/><eyes iris=\"#7a2a8c #e878d0\" pupil=\"star\"/></figure>\nlook-end\n");
            swapped = true;
        }
        struct pollfd p = {.fd = b[0], .events = POLLIN};
        if (poll(&p, 1, 100) <= 0)
            continue;
        if (!fgets(line, sizeof line, in))
            break;
        int s, w, h;
        float hx, hy, hr;
        if (sscanf(line, "frame %d %d %d %f %f %f", &s, &w, &h, &hx, &hy, &hr) == 6) {
            frames++;
            if (now() >= next_save && saved < STRIP) {
                memcpy(strip + (size_t)saved * W * H, ring + (size_t)s * slot, (size_t)W * H * 4);
                /* mark the head the figure reported */
                for (int k = -3; k <= 3; k++) {
                    int x = (int)hx + k, y = (int)hy;
                    if (x >= 0 && x < W && y >= 0 && y < H)
                        strip[(size_t)saved * W * H + y * W + x] = 0xffff2050;
                }
                saved++;
                next_save = now() + 0.5;
            }
            /* show this one, give back the one before */
            if (shown >= 0) {
                char ack[32];
                snprintf(ack, sizeof ack, "ack %d\n", shown);
                send(ack);
            }
            shown = s;
        } else
            printf("figure says: %s", line);
    }
    double el = now() - start;
    printf("%d frames in %.2f s: %.1f fps\n", frames, el, frames / el);
    send("quit\n");
    int status = 0;
    waitpid(pid, &status, 0);
    printf("exit %d\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    /* the strip over a soft background */
    unsigned char *rgbb = malloc((size_t)W * STRIP * H * 3);
    for (int y = 0; y < H; y++)
        for (int i = 0; i < STRIP; i++)
            for (int x = 0; x < W; x++) {
                uint32_t s = strip[(size_t)i * W * H + y * W + x];
                float al = ((s >> 24) & 255) / 255.0f;
                int bg[3] = {0xe8, 0xe4, 0xf0};
                unsigned char *d = &rgbb[((size_t)y * W * STRIP + i * W + x) * 3];
                for (int c = 0; c < 3; c++) {
                    int v = (int)(((s >> (16 - 8 * c)) & 255) + bg[c] * (1 - al));
                    d[c] = (unsigned char)(v > 255 ? 255 : v);
                }
            }
    stbi_write_jpg(out, W * STRIP, H, 3, rgbb, 92);
    printf("wrote %s\n", out);
    return 0;
}

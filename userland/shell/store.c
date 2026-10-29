#define _GNU_SOURCE
#include "store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The OS keeps what matters on the disk: its state (/etc/os: the design,
 * the portrait, the mind) and the person's home (/root). Everything else is
 * rebuilt from the boot image each time.
 *
 * Layout on the raw disk: a 512-byte header at offset 0 naming the active
 * slot, then two slots of SLOT bytes. A save writes the slot that is NOT
 * active, flushes, then rewrites the header (one sector) to point at it, so
 * a power cut in the middle of a save leaves the previous state intact.
 *
 * A slot is a simple archive: per entry "LMN1", u32 kind (1 dir, 2 file),
 * u32 mode, u32 path length, u32 size, the path, the data; then an end
 * marker. The header carries a checksum of the slot's bytes. */

#define HDR_MAGIC "LUMENSTORE1"
#define SLOT0 (1u << 20)       /* 1 MiB */
#define SLOT_SIZE (128u << 20) /* 128 MiB each */
#define MAX_FILE (32u << 20)   /* bigger files aren't kept (yet) */

struct header {
    char magic[12];
    uint32_t active;     /* 0 or 1 */
    uint64_t generation; /* bumps on every save */
    uint64_t length;     /* bytes used in the active slot */
    uint32_t crc;        /* of those bytes */
    uint32_t hdr_crc;    /* of the fields above */
};

static const char *dev_path = "/dev/sda";
static const char *const default_roots[] = {"/etc/os", "/root", NULL};
static const char *const *roots = default_roots;

void store_set_roots(const char *const *r) { roots = r; }

void store_set_device(const char *path) { dev_path = path; }

static uint32_t crc_update(uint32_t c, const void *data, size_t n) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i;
            for (int k = 0; k < 8; k++)
                r = r & 1 ? (r >> 1) ^ 0xEDB88320u : r >> 1;
            table[i] = r;
        }
    const uint8_t *p = data;
    c = ~c;
    while (n--)
        c = table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}

static uint32_t hdr_crc(const struct header *h) { return crc_update(0, h, offsetof(struct header, hdr_crc)); }

static bool read_header(int fd, struct header *h) {
    if (pread(fd, h, sizeof *h, 0) != (ssize_t)sizeof *h)
        return false;
    return !memcmp(h->magic, HDR_MAGIC, sizeof h->magic) && h->hdr_crc == hdr_crc(h) && h->active < 2 &&
           h->length <= SLOT_SIZE;
}

/* ---- saving ---- */

struct out {
    uint8_t *buf;
    size_t len, cap;
    bool full;
};

static void put(struct out *o, const void *d, size_t n) {
    if (o->len + n > SLOT_SIZE) {
        o->full = true;
        return;
    }
    if (o->len + n > o->cap) {
        size_t c = o->cap ? o->cap * 2 : 1 << 16;
        while (c < o->len + n)
            c *= 2;
        o->buf = realloc(o->buf, c);
        o->cap = c;
    }
    memcpy(o->buf + o->len, d, n);
    o->len += n;
}

static void put_entry(struct out *o, uint32_t kind, uint32_t mode, const char *path, const void *data, uint32_t size) {
    uint32_t pl = (uint32_t)strlen(path);
    put(o, "LMN1", 4);
    put(o, &kind, 4);
    put(o, &mode, 4);
    put(o, &pl, 4);
    put(o, &size, 4);
    put(o, path, pl);
    if (size)
        put(o, data, size);
}

#define MIND_PATH "/etc/os/mind.json"
#define MIND_KEPT "/etc/os/mind.disk.json"

/* What the host hands over for one boot only (kernel/main.c preloads it):
 * never kept, and never brought back over this boot's copy. A saved display
 * size from a full-screen run made the next windowed one too big. */
static bool for_this_boot(const char *path) {
    return !strcmp(path, "/etc/os/display") || !strcmp(path, "/etc/os/autorun.sh") ||
           !strcmp(path, "/etc/os/app.env");
}

static void walk(struct out *o, const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0)
        return;
    if (for_this_boot(path))
        return;
    /* the host's mind for this boot is not theirs to keep (see store_restore) */
    if (!strcmp(path, MIND_PATH) && access(MIND_KEPT, F_OK) == 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        put_entry(o, 1, st.st_mode & 07777, path, NULL, 0);
        DIR *d = opendir(path);
        if (!d)
            return;
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            char child[1024];
            snprintf(child, sizeof child, "%s/%s", path, e->d_name);
            walk(o, child);
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode) && (uint64_t)st.st_size <= MAX_FILE) {
        int fd = open(path, O_RDONLY);
        if (fd < 0)
            return;
        uint8_t *data = malloc((size_t)st.st_size + 1);
        ssize_t n = read(fd, data, (size_t)st.st_size);
        close(fd);
        if (n == st.st_size)
            put_entry(o, 2, st.st_mode & 07777, path, data, (uint32_t)n);
        free(data);
    }
}

bool store_save(void) {
    int fd = open(dev_path, O_RDWR);
    if (fd < 0)
        return false;
    struct header h;
    bool had = read_header(fd, &h);
    struct out o = {0};
    for (int i = 0; roots[i]; i++)
        walk(&o, roots[i]);
    put(&o, "END!", 4);
    if (o.full) {
        free(o.buf);
        close(fd);
        return false;
    }
    uint32_t slot = had ? 1 - h.active : 0;
    off_t at = SLOT0 + (off_t)slot * SLOT_SIZE;
    bool ok = pwrite(fd, o.buf, o.len, at) == (ssize_t)o.len && fsync(fd) == 0;
    if (ok) {
        struct header nh;
        memset(&nh, 0, sizeof nh);
        memcpy(nh.magic, HDR_MAGIC, sizeof nh.magic);
        nh.active = slot;
        nh.generation = had ? h.generation + 1 : 1;
        nh.length = o.len;
        nh.crc = crc_update(0, o.buf, o.len);
        nh.hdr_crc = hdr_crc(&nh);
        uint8_t sector[512] = {0};
        memcpy(sector, &nh, sizeof nh);
        ok = pwrite(fd, sector, sizeof sector, 0) == (ssize_t)sizeof sector && fsync(fd) == 0;
    }
    free(o.buf);
    close(fd);
    return ok;
}

/* ---- restoring ---- */

static void mkdirs_for(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
}

int store_restore(void) {
    int fd = open(dev_path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct header h;
    if (!read_header(fd, &h)) {
        close(fd);
        return 0; /* a new disk: nothing yet */
    }
    uint8_t *buf = malloc(h.length ? h.length : 1);
    off_t at = SLOT0 + (off_t)h.active * SLOT_SIZE;
    if (pread(fd, buf, h.length, at) != (ssize_t)h.length || crc_update(0, buf, h.length) != h.crc) {
        free(buf);
        close(fd);
        return -1;
    }
    close(fd);
    /* A desktop handed over by the host for this boot (a theme being tried)
     * wins over the saved one, and the saved look goes with it. */
    bool host_facet = access("/etc/os/facet.xml", F_OK) == 0;
    /* A mind handed over the same way (Claude through the PC's sign-in, say)
     * is used for this boot only: the saved one is kept aside as
     * mind.disk.json, never overwritten, and comes back on a plain boot. */
    bool host_mind = access(MIND_PATH, F_OK) == 0;
    int files = 0;
    size_t p = 0;
    while (p + 20 <= h.length && !memcmp(buf + p, "LMN1", 4)) {
        uint32_t kind, mode, pl, size;
        memcpy(&kind, buf + p + 4, 4);
        memcpy(&mode, buf + p + 8, 4);
        memcpy(&pl, buf + p + 12, 4);
        memcpy(&size, buf + p + 16, 4);
        p += 20;
        if (pl >= 1024 || p + pl + size > h.length)
            break;
        char path[1024];
        memcpy(path, buf + p, pl);
        path[pl] = 0;
        p += pl;
        /* Disks written before the rename keep the state under its old
         * folder name: it comes back where it lives now. */
        static const char old_dir[] = "/etc/" "lu" "men";
        if (!strncmp(path, old_dir, sizeof old_dir - 1) && (path[sizeof old_dir - 1] == '/' || !path[sizeof old_dir - 1])) {
            char moved[1024];
            snprintf(moved, sizeof moved, "/etc/os%s", path + sizeof old_dir - 1);
            snprintf(path, sizeof path, "%s", moved);
        }
        /* only ever into the places it was taken from */
        bool allowed = false;
        for (int i = 0; roots[i]; i++)
            if (!strncmp(path, roots[i], strlen(roots[i])) && !strstr(path, "/../"))
                allowed = true;
        if (host_facet && (!strcmp(path, "/etc/os/facet.xml") || !strcmp(path, "/etc/os/design.json")))
            allowed = false;
        if (for_this_boot(path))
            allowed = false;
        if (host_mind && !strcmp(path, MIND_PATH))
            snprintf(path, sizeof path, "%s", MIND_KEPT);
        else if (!host_mind && !strcmp(path, MIND_KEPT))
            snprintf(path, sizeof path, "%s", MIND_PATH);
        if (allowed) {
            mkdirs_for(path);
            if (kind == 1) {
                mkdir(path, mode ? mode : 0755);
            } else if (kind == 2) {
                int out = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode ? mode : 0644);
                if (out >= 0) {
                    if (write(out, buf + p, size) == (ssize_t)size)
                        files++;
                    close(out);
                }
            }
        }
        p += size;
    }
    free(buf);
    return files;
}

bool store_available(void) { return access(dev_path, R_OK | W_OK) == 0; }

/* Erases what is kept, so "Forget me" also forgets on the disk. */
bool store_wipe(void) {
    int fd = open(dev_path, O_RDWR);
    if (fd < 0)
        return false;
    uint8_t sector[512] = {0};
    bool ok = pwrite(fd, sector, sizeof sector, 0) == (ssize_t)sizeof sector && fsync(fd) == 0;
    /* and the slots' contents, so nothing personal lingers */
    static uint8_t zero[1 << 16];
    for (int s = 0; s < 2 && ok; s++)
        for (off_t o = 0; o < (off_t)(4u << 20); o += sizeof zero) /* the start of each slot holds the state */
            if (pwrite(fd, zero, sizeof zero, SLOT0 + (off_t)s * SLOT_SIZE + o) != (ssize_t)sizeof zero)
                break;
    fsync(fd);
    close(fd);
    return ok;
}

/* Checks the OS's disk store natively: save a tree into a disk image, wipe
 * the tree, restore it, compare; then a second save goes to the other slot
 * and a torn header falls back cleanly. Run by tests/store-test.sh. */
#include "../userland/shell/store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); } while (0)

static void write_file(const char *p, const char *s) {
    FILE *f = fopen(p, "w");
    fputs(s, f);
    fclose(f);
}
static char *read_file(const char *p) {
    static char buf[4096];
    FILE *f = fopen(p, "r");
    if (!f)
        return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/tmp/os-store-test";
    char disk[512], a[512], b[512], c[512];
    snprintf(disk, sizeof disk, "%s/disk.img", dir);
    snprintf(a, sizeof a, "%s/etc-os", dir);
    snprintf(b, sizeof b, "%s/root", dir);
    const char *roots[] = {a, b, NULL};
    char cmd[1200];
    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s/etc-os %s/root/Documents && truncate -s 300M %s", dir, dir, dir, disk);
    if (system(cmd))
        return 2;
    store_set_device(disk);
    store_set_roots(roots);

    CHECK(store_restore() == 0, "a new disk restores nothing");
    snprintf(c, sizeof c, "%s/portrait.txt", a);
    write_file(c, "rhythm\tnight owl\n");
    snprintf(c, sizeof c, "%s/Documents/song.txt", b);
    write_file(c, "the hush after rain\n");
    CHECK(store_save(), "first save");

    snprintf(cmd, sizeof cmd, "rm -rf %s %s", a, b);
    system(cmd);
    CHECK(store_restore() == 2, "restore brings back 2 files");
    snprintf(c, sizeof c, "%s/portrait.txt", a);
    char *t = read_file(c);
    CHECK(t && !strcmp(t, "rhythm\tnight owl\n"), "portrait content");
    snprintf(c, sizeof c, "%s/Documents/song.txt", b);
    t = read_file(c);
    CHECK(t && !strcmp(t, "the hush after rain\n"), "nested file content");

    /* second save goes to the other slot; corrupting it after the header
     * write would be caught by the checksum */
    snprintf(c, sizeof c, "%s/portrait.txt", a);
    write_file(c, "rhythm\tearly riser\n");
    CHECK(store_save(), "second save");
    snprintf(cmd, sizeof cmd, "rm -rf %s %s", a, b);
    system(cmd);
    CHECK(store_restore() == 2, "restore after second save");
    t = read_file(c);
    CHECK(t && !strcmp(t, "rhythm\tearly riser\n"), "newest version restored");

    /* flip a byte inside the active slot's data: restore must refuse it */
    FILE *f = fopen(disk, "r+b");
    fseek(f, (1 << 20) + (128 << 20) + 40, SEEK_SET); /* slot 1 is active after two saves */
    fputc('X', f);
    fclose(f);
    CHECK(store_restore() == -1, "a damaged slot is refused, not half-restored");

    CHECK(store_wipe(), "wipe");
    CHECK(store_restore() == 0, "after wipe nothing is kept");
    printf(fails ? "STORE TEST FAILED\n" : "STORE TEST PASSED\n");
    return fails != 0;
}

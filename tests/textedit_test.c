/* Host test of the shared line editor (userland/shell/textedit.c) with a fixed-width stand-in font:
 *   gcc -I userland/shell tests/textedit_test.c userland/shell/textedit.c -o build/textedit_test && build/textedit_test */
#include "textedit.h"
#include <stdio.h>
#include <string.h>

/* stand-ins for the shell's font, canvas and clipboard: every character 10px wide */
int text_width_n(font *f, const char *s, int n) {
    (void)f;
    int w = 0;
    for (int i = 0; i < n && s[i]; i++)
        if ((s[i] & 0xc0) != 0x80)
            w += 10;
    return w;
}
int text_width(font *f, const char *s) { return text_width_n(f, s, (int)strlen(s)); }
int font_line_height(font *f) { (void)f; return 20; }
int draw_text(canvas *c, font *f, int x, int y, const char *s, color col) { (void)c, (void)f, (void)x, (void)y, (void)s, (void)col; return 0; }
void fill_rect(canvas *c, rect r, color col) { (void)c, (void)r, (void)col; }
bool pt_in(rect r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }
static char clip[256] = "pasted";
const char *clipboard_text(void) { return clip; }
void clipboard_set(const char *t) { snprintf(clip, sizeof clip, "%s", t); }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static text_edit e;
static char buf[256];
static uint64_t now = 10000;
static void key(int k, bool ctrl, bool shift) { now += 50; te_key(&e, buf, sizeof buf, k, ctrl, shift, false, now, NULL); }
static void type(const char *s) { for (; *s; s++) key(*s, false, false); }

int main(void) {
    font *f = (font *)1;
    canvas cv = {0};
    cv.cx1 = cv.cy1 = 1000;

    type("hello world");
    CHECK(!strcmp(buf, "hello world"), "typed: %s", buf);
    for (int i = 0; i < 6; i++) key(K_LEFT, false, false);
    key(K_RIGHT, false, false), key(K_LEFT, false, false);
    type(" big");
    CHECK(!strcmp(buf, "hello big world"), "insert mid: %s", buf);
    key('a', true, false);
    CHECK(e.anchor == 0 && e.caret == (int)strlen(buf), "ctrl+a selects all");
    key(K_BACKSPACE, false, false);
    CHECK(!buf[0], "delete selection: '%s'", buf);
    key('z', true, false);
    CHECK(!strcmp(buf, "hello big world"), "undo: %s", buf);
    key('Z', true, true);
    CHECK(!buf[0], "redo: '%s'", buf);
    key('z', true, false);
    key(K_HOME, false, false);
    key(K_RIGHT, true, true); /* select a word */
    CHECK(e.anchor == 0 && e.caret == 5, "ctrl+shift+right: %d..%d", e.anchor, e.caret);
    key('c', true, false);
    CHECK(!strcmp(clip, "hello"), "copy: %s", clip);
    key(K_END, false, false);
    key(K_BACKSPACE, true, false);
    CHECK(!strcmp(buf, "hello big "), "ctrl+backspace: '%s'", buf);
    key(K_PASTE, false, false);
    CHECK(!strcmp(buf, "hello big hello"), "paste: %s", buf);
    key(K_LEFT, false, true), key(K_LEFT, false, true);
    type("XY");
    CHECK(!strcmp(buf, "hello big helXY"), "type over selection: %s", buf);
    /* the caller clears the field: the caret follows */
    buf[0] = 0;
    type("ok");
    CHECK(!strcmp(buf, "ok") && e.caret == 2, "after clear: %s %d", buf, e.caret);
    /* unicode: é is two bytes, one step */
    key(0xe9, false, false);
    key(K_LEFT, false, false);
    CHECK(e.caret == 2, "left over é: %d", e.caret);
    key(K_DELETE, false, false);
    CHECK(!strcmp(buf, "ok"), "delete é: %s", buf);
    /* the pointer: click at 10px per character, double-click a word */
    snprintf(buf, sizeof buf, "one two three");
    input_state in = {0};
    rect box = {0, 0, 400, 30};
    in.mx = 52, in.my = 10, in.pressed = in.down = true;
    now += 1000;
    te_mouse(&e, buf, f, box, 0, &in, false, now);
    CHECK(e.caret == 5 && e.anchor == 5, "click places caret: %d", e.caret);
    in.pressed = false, in.mx = 92;
    te_mouse(&e, buf, f, box, 0, &in, false, now + 20);
    CHECK(e.anchor == 5 && e.caret == 9, "drag selects: %d..%d", e.anchor, e.caret);
    in.down = false;
    te_mouse(&e, buf, f, box, 0, &in, false, now + 40);
    in.pressed = in.down = true, in.mx = 52;
    te_mouse(&e, buf, f, box, 0, &in, false, now + 100);
    CHECK(e.anchor == 4 && e.caret == 7, "double-click word: %d..%d", e.anchor, e.caret);

    /* several lines: wrap at 100px (10 characters), Up/Down keep the column */
    text_edit m = {0};
    m.multiline = true;
    snprintf(buf, sizeof buf, "first line\nsecond one here wraps");
    te_reset(&m, buf);
    te_draw(&cv, &m, buf, f, (rect){0, 0, 104, 200}, 0, 0, 0, 0, true, false, now);
    CHECK(m.nlines >= 3, "wrapped lines: %d", m.nlines);
    now += 100;
    te_key(&m, buf, sizeof buf, K_HOME, true, false, false, now, NULL);
    te_key(&m, buf, sizeof buf, K_RIGHT, false, false, false, now, NULL);
    te_key(&m, buf, sizeof buf, K_RIGHT, false, false, false, now, NULL);
    te_key(&m, buf, sizeof buf, K_DOWN, false, false, false, now, NULL);
    CHECK(m.caret == 13, "down keeps column: %d", m.caret);
    te_key(&m, buf, sizeof buf, K_ENTER, false, false, false, now, NULL);
    CHECK(buf[13] == '\n', "enter makes a line: %s", buf);
    te_key(&m, buf, sizeof buf, K_END, false, false, false, now, NULL);
    te_key(&m, buf, sizeof buf, K_UP, false, false, false, now, NULL);
    CHECK(m.caret < 14, "up goes back a line: %d", m.caret);

    printf(fails ? "%d failed\n" : "all passed\n", fails);
    return fails != 0;
}

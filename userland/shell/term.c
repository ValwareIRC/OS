#define _GNU_SOURCE
#include "term.h"
#include "app.h"
#include "theme.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* A terminal: a pty pair from the kernel, a shell on the slave end, and an
 * xterm-compatible screen emulator drawing into a window. Enough of VT100 /
 * xterm for shells, editors (vi), pagers (less) and full-screen tools (top):
 * cursor movement, erase, scroll regions, insert/delete, SGR colours (16,
 * 256, truecolour), the alternate screen, bracketed paste, app cursor keys. */

#define DEF 0xffffffffu  /* default colour */
#define RGBF 0x01000000u /* truecolour flag */
#define SB_MAX 2000      /* scrollback lines */
#define SB_W 240         /* columns kept per scrollback line */

enum { AT_BOLD = 1, AT_DIM = 2, AT_ITALIC = 4, AT_UNDER = 8, AT_REVERSE = 16 };

typedef struct {
    uint32_t ch, fg, bg;
    uint8_t attr;
} cell;

enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC, ST_CHARSET };

static struct {
    bool started, exited;
    int fd, pid;
    int cols, rows;
    cell *main, *alt, *grid; /* grid points at main or alt */
    int cx, cy, scx, scy;
    bool wrap_pending;
    int top, bot; /* scroll region, inclusive */
    cell pen;
    bool cursor_on, app_cursor, bracketed, autowrap;
    cell *sb; /* SB_MAX x SB_W ring */
    int sb_count, sb_head, view;
    int state, np, params[16];
    char priv;
    char osc[256];
    int osc_len;
    uint32_t u_cp;
    int u_left;
    char title[128];
    int exit_status;
    uint64_t last_output;
} T;

static cell blank(void) { return (cell){' ', T.pen.fg, T.pen.bg, 0}; }
static cell *at(int x, int y) { return &T.grid[y * T.cols + x]; }

static void clear_cells(int y, int x0, int x1) {
    for (int x = x0; x < x1 && x < T.cols; x++)
        *at(x, y) = blank();
}

static void sb_push(const cell *line) {
    int slot = (T.sb_head + T.sb_count) % SB_MAX;
    if (T.sb_count == SB_MAX) {
        T.sb_head = (T.sb_head + 1) % SB_MAX;
        slot = (T.sb_head + T.sb_count - 1) % SB_MAX;
    } else {
        T.sb_count++;
    }
    cell *dst = &T.sb[slot * SB_W];
    for (int x = 0; x < SB_W; x++)
        dst[x] = x < T.cols ? line[x] : (cell){' ', DEF, DEF, 0};
}

static void scroll_up(int top, int bot, int n) {
    for (int k = 0; k < n; k++) {
        if (top == 0 && T.grid == T.main)
            sb_push(at(0, 0));
        memmove(at(0, top), at(0, top + 1), sizeof(cell) * (size_t)(T.cols * (bot - top)));
        clear_cells(bot, 0, T.cols);
    }
}

static void scroll_down(int top, int bot, int n) {
    for (int k = 0; k < n; k++) {
        memmove(at(0, top + 1), at(0, top), sizeof(cell) * (size_t)(T.cols * (bot - top)));
        clear_cells(top, 0, T.cols);
    }
}

static void newline(void) {
    if (T.cy == T.bot)
        scroll_up(T.top, T.bot, 1);
    else if (T.cy < T.rows - 1)
        T.cy++;
}

static void put_char(uint32_t cp) {
    if (T.wrap_pending) {
        T.wrap_pending = false;
        if (T.autowrap) {
            T.cx = 0;
            newline();
        }
    }
    cell c = T.pen;
    c.ch = cp;
    *at(T.cx, T.cy) = c;
    if (T.cx == T.cols - 1)
        T.wrap_pending = true;
    else
        T.cx++;
}

static void clamp_cursor(void) {
    if (T.cx < 0) T.cx = 0;
    if (T.cx >= T.cols) T.cx = T.cols - 1;
    if (T.cy < 0) T.cy = 0;
    if (T.cy >= T.rows) T.cy = T.rows - 1;
    T.wrap_pending = false;
}

static void reply(const char *s) {
    if (T.fd >= 0 && write(T.fd, s, strlen(s)) < 0) {
        /* the shell went away; nothing to answer */
    }
}

static int param(int i, int def) { return i < T.np && T.params[i] > 0 ? T.params[i] : def; }

static uint32_t sgr_color(int *i) {
    /* 38;5;n or 38;2;r;g;b */
    if (*i + 1 < T.np && T.params[*i + 1] == 5 && *i + 2 < T.np) {
        uint32_t c = (uint32_t)T.params[*i + 2] & 255;
        *i += 2;
        return c;
    }
    if (*i + 1 < T.np && T.params[*i + 1] == 2 && *i + 4 < T.np) {
        uint32_t c = RGBF | ((uint32_t)(T.params[*i + 2] & 255) << 16) | ((uint32_t)(T.params[*i + 3] & 255) << 8) |
                     (uint32_t)(T.params[*i + 4] & 255);
        *i += 4;
        return c;
    }
    return DEF;
}

static void sgr(void) {
    if (T.np == 0) {
        T.pen = (cell){' ', DEF, DEF, 0};
        return;
    }
    for (int i = 0; i < T.np; i++) {
        int p = T.params[i];
        if (p == 0) T.pen.attr = 0, T.pen.fg = T.pen.bg = DEF;
        else if (p == 1) T.pen.attr |= AT_BOLD;
        else if (p == 2) T.pen.attr |= AT_DIM;
        else if (p == 3) T.pen.attr |= AT_ITALIC;
        else if (p == 4) T.pen.attr |= AT_UNDER;
        else if (p == 7) T.pen.attr |= AT_REVERSE;
        else if (p == 22) T.pen.attr &= (uint8_t)~(AT_BOLD | AT_DIM);
        else if (p == 23) T.pen.attr &= (uint8_t)~AT_ITALIC;
        else if (p == 24) T.pen.attr &= (uint8_t)~AT_UNDER;
        else if (p == 27) T.pen.attr &= (uint8_t)~AT_REVERSE;
        else if (p >= 30 && p <= 37) T.pen.fg = (uint32_t)(p - 30);
        else if (p == 38) T.pen.fg = sgr_color(&i);
        else if (p == 39) T.pen.fg = DEF;
        else if (p >= 40 && p <= 47) T.pen.bg = (uint32_t)(p - 40);
        else if (p == 48) T.pen.bg = sgr_color(&i);
        else if (p == 49) T.pen.bg = DEF;
        else if (p >= 90 && p <= 97) T.pen.fg = (uint32_t)(p - 90 + 8);
        else if (p >= 100 && p <= 107) T.pen.bg = (uint32_t)(p - 100 + 8);
    }
}

static void set_alt(bool on) {
    if (on == (T.grid == T.alt))
        return;
    if (on) {
        T.scx = T.cx, T.scy = T.cy;
        T.grid = T.alt;
        for (int y = 0; y < T.rows; y++)
            clear_cells(y, 0, T.cols);
    } else {
        T.grid = T.main;
        T.cx = T.scx, T.cy = T.scy;
    }
    T.wrap_pending = false;
}

static void mode(bool set) {
    for (int i = 0; i < T.np; i++) {
        int p = T.params[i];
        if (T.priv == '?') {
            if (p == 1) T.app_cursor = set;
            else if (p == 7) T.autowrap = set;
            else if (p == 25) T.cursor_on = set;
            else if (p == 47 || p == 1047 || p == 1049) set_alt(set);
            else if (p == 2004) T.bracketed = set;
        }
    }
}

static void csi(char f) {
    int n = param(0, 1);
    switch (f) {
    case 'A': T.cy -= n; if (T.cy < T.top && T.cy + n >= T.top) T.cy = T.top; clamp_cursor(); break;
    case 'B': T.cy += n; if (T.cy > T.bot && T.cy - n <= T.bot) T.cy = T.bot; clamp_cursor(); break;
    case 'C': T.cx += n; clamp_cursor(); break;
    case 'D': T.cx -= n; clamp_cursor(); break;
    case 'E': T.cx = 0; T.cy += n; clamp_cursor(); break;
    case 'F': T.cx = 0; T.cy -= n; clamp_cursor(); break;
    case 'G': case '`': T.cx = n - 1; clamp_cursor(); break;
    case 'd': T.cy = n - 1; clamp_cursor(); break;
    case 'H': case 'f': T.cy = param(0, 1) - 1; T.cx = param(1, 1) - 1; clamp_cursor(); break;
    case 'J': {
        int m = T.np ? T.params[0] : 0;
        if (m == 0) {
            clear_cells(T.cy, T.cx, T.cols);
            for (int y = T.cy + 1; y < T.rows; y++) clear_cells(y, 0, T.cols);
        } else if (m == 1) {
            for (int y = 0; y < T.cy; y++) clear_cells(y, 0, T.cols);
            clear_cells(T.cy, 0, T.cx + 1);
        } else {
            for (int y = 0; y < T.rows; y++) clear_cells(y, 0, T.cols);
            if (m == 3) T.sb_count = 0;
        }
        break;
    }
    case 'K': {
        int m = T.np ? T.params[0] : 0;
        if (m == 0) clear_cells(T.cy, T.cx, T.cols);
        else if (m == 1) clear_cells(T.cy, 0, T.cx + 1);
        else clear_cells(T.cy, 0, T.cols);
        break;
    }
    case 'L': if (T.cy >= T.top && T.cy <= T.bot) scroll_down(T.cy, T.bot, n); break;
    case 'M': if (T.cy >= T.top && T.cy <= T.bot) {
            for (int k = 0; k < n; k++) {
                memmove(at(0, T.cy), at(0, T.cy + 1), sizeof(cell) * (size_t)(T.cols * (T.bot - T.cy)));
                clear_cells(T.bot, 0, T.cols);
            }
        }
        break;
    case '@': {
        if (n > T.cols - T.cx) n = T.cols - T.cx;
        memmove(at(T.cx + n, T.cy), at(T.cx, T.cy), sizeof(cell) * (size_t)(T.cols - T.cx - n));
        clear_cells(T.cy, T.cx, T.cx + n);
        break;
    }
    case 'P': {
        if (n > T.cols - T.cx) n = T.cols - T.cx;
        memmove(at(T.cx, T.cy), at(T.cx + n, T.cy), sizeof(cell) * (size_t)(T.cols - T.cx - n));
        clear_cells(T.cy, T.cols - n, T.cols);
        break;
    }
    case 'X': clear_cells(T.cy, T.cx, T.cx + n); break;
    case 'S': scroll_up(T.top, T.bot, n); break;
    case 'T': scroll_down(T.top, T.bot, n); break;
    case 'm': sgr(); break;
    case 'r': {
        int t = param(0, 1) - 1, b = param(1, T.rows) - 1;
        if (t < 0) t = 0;
        if (b >= T.rows) b = T.rows - 1;
        if (t < b) T.top = t, T.bot = b;
        T.cx = 0, T.cy = 0;
        T.wrap_pending = false;
        break;
    }
    case 's': T.scx = T.cx, T.scy = T.cy; break;
    case 'u': T.cx = T.scx, T.cy = T.scy; clamp_cursor(); break;
    case 'h': mode(true); break;
    case 'l': mode(false); break;
    case 'n':
        if (param(0, 0) == 6) {
            char b[32];
            snprintf(b, sizeof b, "\033[%d;%dR", T.cy + 1, T.cx + 1);
            reply(b);
        } else if (param(0, 0) == 5) {
            reply("\033[0n");
        }
        break;
    case 'c':
        if (!T.priv) reply("\033[?62;22c");
        break;
    case 't': break; /* window manipulation: ignored */
    }
}

static void esc(char c) {
    switch (c) {
    case '7': T.scx = T.cx, T.scy = T.cy; break;
    case '8': T.cx = T.scx, T.cy = T.scy; clamp_cursor(); break;
    case 'D': newline(); break;
    case 'E': T.cx = 0; newline(); break;
    case 'M':
        if (T.cy == T.top) scroll_down(T.top, T.bot, 1);
        else if (T.cy > 0) T.cy--;
        break;
    case 'c': /* full reset */
        T.pen = (cell){' ', DEF, DEF, 0};
        T.top = 0, T.bot = T.rows - 1;
        T.cx = T.cy = 0;
        for (int y = 0; y < T.rows; y++) clear_cells(y, 0, T.cols);
        break;
    }
}

static void feed(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        switch (T.state) {
        case ST_GROUND:
            if (T.u_left) {
                if ((c & 0xc0) == 0x80) {
                    T.u_cp = (T.u_cp << 6) | (c & 0x3f);
                    if (--T.u_left == 0)
                        put_char(T.u_cp);
                    continue;
                }
                T.u_left = 0;
                put_char(0xfffd);
            }
            if (c == 0x1b) T.state = ST_ESC;
            else if (c == '\r') T.cx = 0, T.wrap_pending = false;
            else if (c == '\n' || c == 0x0b || c == 0x0c) newline(), T.wrap_pending = false;
            else if (c == '\b') { if (T.cx > 0) T.cx--; T.wrap_pending = false; }
            else if (c == '\t') { T.cx = (T.cx / 8 + 1) * 8; if (T.cx >= T.cols) T.cx = T.cols - 1; }
            else if (c == 0x07 || c < 0x20 || c == 0x7f) { /* bell and other controls */ }
            else if (c < 0x80) put_char(c);
            else if ((c & 0xe0) == 0xc0) T.u_cp = c & 0x1f, T.u_left = 1;
            else if ((c & 0xf0) == 0xe0) T.u_cp = c & 0x0f, T.u_left = 2;
            else if ((c & 0xf8) == 0xf0) T.u_cp = c & 0x07, T.u_left = 3;
            else put_char(0xfffd);
            break;
        case ST_ESC:
            if (c == '[') {
                T.state = ST_CSI;
                T.np = 0;
                T.priv = 0;
                memset(T.params, 0, sizeof T.params);
            } else if (c == ']') {
                T.state = ST_OSC;
                T.osc_len = 0;
            } else if (c == '(' || c == ')' || c == '*' || c == '+') {
                T.state = ST_CHARSET;
            } else {
                esc((char)c);
                T.state = ST_GROUND;
            }
            break;
        case ST_CHARSET:
            T.state = ST_GROUND;
            break;
        case ST_CSI:
            if (c >= '0' && c <= '9') {
                if (T.np == 0) T.np = 1;
                if (T.params[T.np - 1] < 100000) T.params[T.np - 1] = T.params[T.np - 1] * 10 + (c - '0');
            } else if (c == ';' || c == ':') {
                if (T.np == 0) T.np = 1;
                if (T.np < 16) T.np++;
            } else if (c == '?' || c == '>' || c == '=' || c == '!') {
                T.priv = (char)c;
            } else if (c >= 0x40 && c <= 0x7e) {
                csi((char)c);
                T.state = ST_GROUND;
            } else if (c == 0x1b) {
                T.state = ST_ESC;
            }
            break;
        case ST_OSC:
            if (c == 0x07) {
                T.state = ST_GROUND;
            } else if (c == 0x1b) {
                T.state = ST_OSC_ESC;
                continue;
            } else if (T.osc_len < (int)sizeof T.osc - 1) {
                T.osc[T.osc_len++] = (char)c;
                continue;
            } else {
                continue;
            }
            /* fallthrough: finished */
            T.osc[T.osc_len] = 0;
            if ((T.osc[0] == '0' || T.osc[0] == '2') && T.osc[1] == ';')
                snprintf(T.title, sizeof T.title, "%s", T.osc + 2);
            break;
        case ST_OSC_ESC:
            T.osc[T.osc_len] = 0;
            if ((T.osc[0] == '0' || T.osc[0] == '2') && T.osc[1] == ';')
                snprintf(T.title, sizeof T.title, "%s", T.osc + 2);
            T.state = ST_GROUND;
            break;
        }
    }
}

/* ---- the shell on the other end ---- */

static void resize(int cols, int rows) {
    if (cols < 10) cols = 10;
    if (rows < 3) rows = 3;
    if (cols == T.cols && rows == T.rows)
        return;
    cell *nm = calloc((size_t)(cols * rows), sizeof(cell)), *na = calloc((size_t)(cols * rows), sizeof(cell));
    cell b = {' ', DEF, DEF, 0};
    for (int i = 0; i < cols * rows; i++)
        nm[i] = na[i] = b;
    /* Keep the bottom of the old screen (where the prompt is). */
    int shift = T.main && T.cy >= rows ? T.cy - rows + 1 : 0;
    for (int y = 0; T.main && y < rows && y + shift < T.rows; y++)
        for (int x = 0; x < cols && x < T.cols; x++) {
            nm[y * cols + x] = T.main[(y + shift) * T.cols + x];
            na[y * cols + x] = T.alt[(y + shift) * T.cols + x];
        }
    bool on_alt = T.grid && T.grid == T.alt;
    free(T.main);
    free(T.alt);
    T.main = nm, T.alt = na;
    T.grid = on_alt ? T.alt : T.main;
    T.cols = cols, T.rows = rows;
    T.cy -= shift;
    T.top = 0, T.bot = rows - 1;
    clamp_cursor();
    if (T.fd >= 0) {
        struct winsize ws = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)cols};
        ioctl(T.fd, TIOCSWINSZ, &ws);
    }
}

static bool spawn(void) {
    int fd = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (fd < 0) {
        LOG("terminal: /dev/ptmx: %s\n", strerror(errno));
        return false;
    }
    int unlock = 0, n = -1;
    ioctl(fd, TIOCSPTLCK, &unlock);
    ioctl(fd, TIOCGPTN, &n);
    char slave[32];
    snprintf(slave, sizeof slave, "/dev/pts/%d", n);
    struct winsize ws = {.ws_row = (unsigned short)T.rows, .ws_col = (unsigned short)T.cols};
    ioctl(fd, TIOCSWINSZ, &ws);
    int pid = fork();
    if (pid < 0) {
        close(fd);
        return false;
    }
    if (pid == 0) {
        setsid();
        int s = open(slave, O_RDWR); /* becomes the controlling terminal */
        if (s < 0)
            _exit(127);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if (s > 2)
            close(s);
        for (int i = 3; i < 64; i++)
            close(i);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        chdir("/root");
        setenv("TERM", "xterm-256color", 1);
        setenv("HOME", "/root", 1);
        setenv("PS1", "\\[\\e[38;5;117m\\]\\w\\[\\e[0m\\] \\$ ", 1);
        setenv("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);
        /* bash when it is in the image (a shell with history and completion,
         * and no banner of its own); busybox's ash otherwise */
        if (access("/usr/bin/bash", X_OK) == 0)
            execl("/usr/bin/bash", "-bash", (char *)NULL);
        execl("/bin/sh", "-sh", (char *)NULL);
        _exit(127);
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    T.fd = fd;
    T.pid = pid;
    LOG("terminal: shell %d on %s\n", pid, slave);
    return true;
}

static void start(int cols, int rows) {
    free(T.sb);
    free(T.main);
    free(T.alt);
    memset(&T, 0, sizeof T);
    T.fd = -1;
    T.pen = (cell){' ', DEF, DEF, 0};
    T.cursor_on = T.autowrap = true;
    T.sb = calloc(SB_MAX * SB_W, sizeof(cell));
    resize(cols, rows);
    T.started = true;
    if (!spawn()) {
        const char *m = "Could not open a terminal.\r\n";
        feed((const unsigned char *)m, strlen(m));
        T.exited = true;
    }
}

static void pump(void) {
    if (T.fd < 0)
        return;
    unsigned char buf[16384];
    for (int rounds = 0; rounds < 16; rounds++) {
        ssize_t n = read(T.fd, buf, sizeof buf);
        if (n > 0) {
            feed(buf, (size_t)n);
            T.view = 0;
            T.last_output = now_ms();
            continue;
        }
        if (n < 0 && errno == EAGAIN)
            break;
        /* EIO or 0: the shell and everything it started have gone */
        close(T.fd);
        T.fd = -1;
        break;
    }
    if (T.pid > 0) {
        int st;
        if (waitpid(T.pid, &st, WNOHANG) == T.pid) {
            T.pid = 0;
            T.exit_status = st;
        }
    }
    if (T.fd < 0 && !T.exited) {
        T.exited = true;
        const char *m = "\r\n\033[2m[the shell has ended; press Enter for a new one]\033[0m";
        feed((const unsigned char *)m, strlen(m));
    }
}

static void send(const char *s, size_t n) {
    if (T.fd >= 0 && write(T.fd, s, n) < 0) {
        /* full or gone: the next pump notices */
    }
}

static void type_keys(void) {
    for (int i = 0; i < A.in.nkeys; i++) {
        int k = A.in.keys[i];
        if (T.exited) {
            if (k == K_ENTER)
                start(T.cols, T.rows);
            continue;
        }
        const char *app = T.app_cursor ? "\033O" : "\033[";
        char b[16];
        switch (k) {
        case K_ENTER: send("\r", 1); break;
        case K_BACKSPACE: send("\x7f", 1); break;
        case K_ESC: send("\033", 1); break;
        case K_TAB: send("\t", 1); break;
        case K_UP: snprintf(b, sizeof b, "%sA", app); send(b, strlen(b)); break;
        case K_DOWN: snprintf(b, sizeof b, "%sB", app); send(b, strlen(b)); break;
        case K_RIGHT: snprintf(b, sizeof b, "%sC", app); send(b, strlen(b)); break;
        case K_LEFT: snprintf(b, sizeof b, "%sD", app); send(b, strlen(b)); break;
        case K_HOME: snprintf(b, sizeof b, "%sH", app); send(b, strlen(b)); break;
        case K_END: snprintf(b, sizeof b, "%sF", app); send(b, strlen(b)); break;
        case K_DELETE: send("\033[3~", 4); break;
        case K_PGUP:
            if (A.in.shift) T.view += T.rows / 2;
            else send("\033[5~", 4);
            break;
        case K_PGDN:
            if (A.in.shift) T.view -= T.rows / 2;
            else send("\033[6~", 4);
            break;
        case K_META: break;
        case K_TALK: break; /* the shell listens instead */
        case K_PASTE: {
            const char *clip = clipboard_text();
            if (T.bracketed) send("\033[200~", 6);
            send(clip, strlen(clip));
            if (T.bracketed) send("\033[201~", 6);
            break;
        }
        default:
            if (k >= 0x80 && k < 0x100) { /* é and friends from other layouts: UTF-8, like the rest */
                char u[2] = {(char)(0xc0 | (k >> 6)), (char)(0x80 | (k & 0x3f))};
                send(u, 2);
            } else if (k > 0 && k < 0x100) {
                char c = (char)k;
                if (A.in.ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
                    c = (char)(c & 0x1f);
                else if (A.in.ctrl && c == '[')
                    c = 0x1b;
                send(&c, 1);
            } else if (k >= 0x100 && k < 0x110000 && k > K_PASTE) {
                char u[4];
                int n = 0;
                if (k < 0x800) u[n++] = (char)(0xc0 | (k >> 6)), u[n++] = (char)(0x80 | (k & 0x3f));
                else if (k < 0x10000) u[n++] = (char)(0xe0 | (k >> 12)), u[n++] = (char)(0x80 | ((k >> 6) & 0x3f)), u[n++] = (char)(0x80 | (k & 0x3f));
                else u[n++] = (char)(0xf0 | (k >> 18)), u[n++] = (char)(0x80 | ((k >> 12) & 0x3f)), u[n++] = (char)(0x80 | ((k >> 6) & 0x3f)), u[n++] = (char)(0x80 | (k & 0x3f));
                send(u, (size_t)n);
            }
        }
    }
}

/* ---- drawing ---- */

/* The colours come from the design (theme.c): its own terminal colours if
 * it wrote them, else ones derived from its surface with good contrast. */
static struct term_colors TC;

static color ansi(int i, bool dark) {
    (void)dark;
    return TC.ansi[i & 15];
}

static color resolve(uint32_t c, color def, bool dark) {
    if (c == DEF)
        return def;
    if (c & RGBF)
        return RGBA((c >> 16) & 255, (c >> 8) & 255, c & 255, 255);
    if (c < 16)
        return ansi((int)c, dark);
    if (c < 232) {
        int v = (int)c - 16, r = v / 36, g = v / 6 % 6, b = v % 6;
        static const int lv[6] = {0, 95, 135, 175, 215, 255};
        return RGBA(lv[r], lv[g], lv[b], 255);
    }
    int gr = 8 + ((int)c - 232) * 10;
    return RGBA(gr, gr, gr, 255);
}

static int utf8(uint32_t cp, char *o) {
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xc0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xe0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[2] = (char)(0x80 | (cp & 0x3f)); return 3; }
    o[0] = (char)(0xf0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3f)); o[2] = (char)(0x80 | ((cp >> 6) & 0x3f)); o[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

const char *term_title(void) { return T.title[0] ? T.title : NULL; }

void term_draw(rect r, bool focused, color surface, color text, float px) {
    theme_term_colors(&TC, surface, text);
    font *f = font_get(TC.font[0] ? TC.font : "JetBrainsMono", px);
    int cw = text_width(f, "M"), ch = font_line_height(f);
    if (cw < 4) cw = 8;
    if (ch < 8) ch = 16;
    /* A console fills its whole slot, edge to edge, like a real one. */
    rect slot = theme_slot_box(r);
    bool edge = slot.w != r.w || slot.h != r.h;
    r = slot;
    if (edge)
        canvas_clip(A.frame, r);
    int pad = edge ? 8 : 12;
    int cols = (r.w - pad * 2) / cw, rows = (r.h - pad * 2) / ch;
    if (!T.started)
        start(cols, rows);
    else
        resize(cols, rows);
    pump();
    if (focused)
        type_keys();
    if (pt_in(r, A.in.mx, A.in.my) && A.in.wheel)
        T.view += A.in.wheel * 3;
    if (T.view > T.sb_count) T.view = T.sb_count;
    if (T.view < 0) T.view = 0;

    bool dark = color_luma(TC.bg) < 0.45f;
    color bgdef = TC.bg;
    color fgdef = TC.fg;
    if (edge)
        theme_fill_slot(r, bgdef);
    else
        fill_rect(A.frame, r, bgdef);
    int ox = r.x + pad, oy = r.y + pad;
    for (int y = 0; y < T.rows; y++) {
        /* rows above the screen come from the scrollback when scrolled */
        int src = y - T.view;
        const cell *line;
        if (src < 0) {
            int sbi = T.sb_count + src;
            if (sbi < 0)
                continue;
            line = &T.sb[((T.sb_head + sbi) % SB_MAX) * SB_W];
        } else {
            line = &T.grid[src * T.cols];
        }
        int x = 0;
        while (x < T.cols) {
            const cell *c0 = &line[x];
            /* a run of cells with the same look */
            int x1 = x + 1;
            while (x1 < T.cols && line[x1].fg == c0->fg && line[x1].bg == c0->bg && line[x1].attr == c0->attr)
                x1++;
            color fg = resolve(c0->fg, fgdef, dark), bg = resolve(c0->bg, bgdef, dark);
            if (c0->fg < 8 && (c0->attr & AT_BOLD))
                fg = ansi((int)c0->fg + 8, dark);
            /* 256-colour and true-colour text picked for some other ground
             * still has to read on this one */
            if (c0->fg != DEF && c0->fg >= 16 && c0->bg == DEF)
                fg = theme_fit(fg, bgdef, 3.0f);
            if (c0->attr & AT_REVERSE) {
                color t = fg;
                fg = bg;
                bg = t;
            }
            if (c0->attr & AT_DIM)
                fg = color_mix(fg, bg, 0.45f);
            if (bg != bgdef)
                fill_rect(A.frame, (rect){ox + x * cw, oy + y * ch, (x1 - x) * cw, ch}, bg);
            for (int k = x; k < x1; k++) {
                if (line[k].ch == ' ' || line[k].ch == 0)
                    continue;
                char u[5];
                int n = utf8(line[k].ch, u);
                u[n] = 0;
                draw_text(A.frame, f, ox + k * cw, oy + y * ch, u, fg);
                if (c0->attr & AT_BOLD) /* no bold face: overstrike */
                    draw_text(A.frame, f, ox + k * cw + 1, oy + y * ch, u, color_alpha(fg, 0.5f));
            }
            if (c0->attr & AT_UNDER)
                fill_rect(A.frame, (rect){ox + x * cw, oy + y * ch + ch - 2, (x1 - x) * cw, 1}, fg);
            x = x1;
        }
    }
    /* the cursor: the design's shape and colour, blinking when idle */
    if (T.cursor_on && T.view == 0 && !T.exited) {
        rect cr = {ox + T.cx * cw, oy + (T.cy + T.view) * ch, cw, ch};
        float idle = (float)(A.now - T.last_output);
        bool on = idle < 500 || (A.now / 530) % 2 == 0;
        if (focused && on && TC.cursor_shape == 1)
            fill_rect(A.frame, (rect){cr.x, cr.y + 1, 2, ch - 2}, TC.cursor);
        else if (focused && on && TC.cursor_shape == 2)
            fill_rect(A.frame, (rect){cr.x, cr.y + ch - 3, cw, 3}, TC.cursor);
        else if (focused && on)
            fill_round(A.frame, cr, 2, color_alpha(TC.cursor, 0.85f));
        else if (!focused)
            stroke_round(A.frame, cr, 2, 1, color_alpha(TC.cursor, 0.6f));
        if (focused && on && TC.cursor_shape == 0 && T.cx < T.cols) {
            const cell *c = at(T.cx, T.cy);
            if (c->ch != ' ') {
                char u[5];
                int n = utf8(c->ch, u);
                u[n] = 0;
                draw_text(A.frame, f, cr.x, cr.y, u, readable_on(TC.cursor));
            }
        }
    }
    if (T.view > 0) {
        char m[48];
        snprintf(m, sizeof m, "%d lines up", T.view);
        font *sf = font_get("Inter", 12);
        int w = text_width(sf, m) + 16;
        fill_round(A.frame, (rect){r.x + r.w - w - 10, r.y + 8, w, 22}, 11, color_alpha(fgdef, 0.15f));
        draw_text(A.frame, sf, r.x + r.w - w - 2, r.y + 11, m, fgdef);
    }
}

void term_close(void) {
    if (T.pid > 0)
        kill(-T.pid, SIGHUP);
    if (T.fd >= 0)
        close(T.fd);
    T.fd = -1;
    T.started = false;
}

/* For tests: the visible text, one line per row. */
void term_dump(char *out, size_t len) {
    size_t o = 0;
    for (int y = 0; y < T.rows && o + 4 < len; y++) {
        int last = T.cols - 1;
        while (last >= 0 && T.grid[y * T.cols + last].ch == ' ')
            last--;
        for (int x = 0; x <= last && o + 5 < len; x++)
            o += (size_t)utf8(T.grid[y * T.cols + x].ch, out + o);
        out[o++] = '\n';
    }
    out[o] = 0;
}

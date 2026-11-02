#include <bootfont.h>
#include <kernel/boot.h>
#include <kernel/drivers.h>
#include <kernel/lib.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>

/* The boot screen, from the kernel's first moment until a program takes the screen (os-boot, which goes on in
 * the person's own design, then the desktop). By default a calm splash: a mark, the name, a thin bar and one
 * line saying what is happening; Esc shows the steps themselves, each with its state, details and time. The
 * kernel's messages never appear: they are kept (/proc/bootlog, and the serial port), and shown only if the
 * system fails to start (bootscreen_failed).
 *
 * The steps are declared here, all of them up to the desktop, so the list reads whole from the start; programs
 * mark theirs through /proc/bootsteps, which is also where os-boot reads them all. Text is blended against the
 * known background colour, never read back from the screen (reading video memory is slow). */

static struct limine_framebuffer *fb;
static uint32_t W, H;
static spinlock_t draw_lock = SPINLOCK_INIT;
static bool suspended, failed, log_view;

/* neutral until the person's design is known (os-boot) */
static const uint32_t C_BG = 0x101114, C_TEXT = 0xe8e6e3, C_MUTED = 0x85837f, C_ACCENT = 0xd9cbbd, C_TRACK = 0x26272b,
                      C_FAIL = 0xe0866e;

/* ---- the steps ---- */

enum { PENDING, ACTIVE, DONE, FAILED_STEP };
struct step {
    const char *id, *label, *doing;
    int weight; /* share of the bar; 0: in the background, not waited for */
    int state;
    char detail[56];
    uint64_t done, total, t0, t1;
};
static struct step steps[] = {
    {.id = "cpu", .label = "Processor", .doing = "waking the processors", .weight = 2},
    {.id = "memory", .label = "Memory", .doing = "counting memory", .weight = 1},
    {.id = "disks", .label = "Disks", .doing = "finding the disks", .weight = 2},
    {.id = "base", .label = "Base system", .doing = "reading the base system", .weight = 40},
    {.id = "model", .label = "Built-in mind", .doing = "reading the built-in mind", .weight = 0},
    {.id = "state", .label = "Your things", .doing = "bringing back your things", .weight = 4},
    {.id = "apps", .label = "Apps area", .doing = "checking the apps area", .weight = 6},
    {.id = "services", .label = "Services", .doing = "starting services", .weight = 5},
    {.id = "desktop", .label = "Desktop", .doing = "opening your desktop", .weight = 40},
};
#define NSTEPS ((int)ARRAY_LEN(steps))

uint64_t time_monotonic_ns(void);

static struct step *find(const char *id) {
    for (int i = 0; i < NSTEPS; i++)
        if (!strcmp(steps[i].id, id))
            return &steps[i];
    return NULL;
}

/* ---- drawing ---- */

static inline uint32_t pack(uint32_t rgb) {
    return ((rgb >> 16 & 0xff) << fb->red_mask_shift) | ((rgb >> 8 & 0xff) << fb->green_mask_shift) |
           ((rgb & 0xff) << fb->blue_mask_shift);
}

static inline uint32_t mix(uint32_t a, uint32_t b, uint32_t t) { /* t of 255 towards b */
    uint32_t r = ((a >> 16 & 0xff) * (255 - t) + (b >> 16 & 0xff) * t) / 255;
    uint32_t g = ((a >> 8 & 0xff) * (255 - t) + (b >> 8 & 0xff) * t) / 255;
    uint32_t bl = ((a & 0xff) * (255 - t) + (b & 0xff) * t) / 255;
    return r << 16 | g << 8 | bl;
}

static inline void put(int x, int y, uint32_t rgb) {
    if (x < 0 || y < 0 || (uint32_t)x >= W || (uint32_t)y >= H)
        return;
    ((uint32_t *)((uint8_t *)fb->address + (uint64_t)y * fb->pitch))[x] = pack(rgb);
}

static void fill(int x, int y, int w, int h, uint32_t rgb) {
    uint32_t p = pack(rgb);
    for (int j = y < 0 ? 0 : y; j < y + h && (uint32_t)j < H; j++) {
        uint32_t *row = (uint32_t *)((uint8_t *)fb->address + (uint64_t)j * fb->pitch);
        for (int i = x < 0 ? 0 : x; i < x + w && (uint32_t)i < W; i++)
            row[i] = p;
    }
}

static int text_width(const struct bootfont *f, const char *s) {
    int w = 0;
    for (; *s; s++)
        if (*s >= 32 && *s < 127)
            w += f->g[*s - 32].adv;
    return (w + 32) / 64;
}

/* `s` with its baseline at y, from x; on `bg` (the colour beneath, solid) */
static int text(const struct bootfont *f, int x, int y, const char *s, uint32_t color, uint32_t bg) {
    int pen = x * 64;
    for (; *s; s++) {
        if (*s < 32 || *s >= 127)
            continue;
        const struct bootglyph *g = &f->g[*s - 32];
        int gx = pen / 64 + g->x, gy = y + g->y;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                uint8_t a = f->bits[g->off + (uint32_t)(j * g->w + i)];
                if (a)
                    put(gx + i, gy + j, mix(bg, color, a));
            }
        pen += g->adv;
    }
    return pen / 64;
}

/* coverage of a pixel at distance d (in 1/16 px) from an edge of a shape of half-width hw (1/16 px) */
static uint32_t cover(int d, int hw) {
    int c = hw - d + 8; /* half a pixel of soft edge each side */
    return c <= 0 ? 0 : c >= 16 ? 255 : (uint32_t)(c * 255 / 16);
}

static int isqrt(int v) {
    int r = 0;
    for (int b = 1 << 14; b; b >>= 1)
        if ((r + b) * (r + b) <= v)
            r += b;
    return r;
}

/* a ring (radius r, stroke sw px) or a filled dot (sw 0), centre (cx, cy) */
static void ring(int cx, int cy, int r, int sw, uint32_t color, uint32_t bg) {
    int R = r + sw + 2;
    for (int y = -R; y <= R; y++)
        for (int x = -R; x <= R; x++) {
            int d = isqrt((x * x + y * y) * 256); /* distance from the centre, in 1/16 px */
            uint32_t a = sw ? cover(d > r * 16 ? d - r * 16 : r * 16 - d, sw * 8) : cover(d, r * 16);
            if (a)
                put(cx + x, cy + y, mix(bg, color, a));
        }
}

/* a line from (x0,y0) to (x1,y1), width w px, round ends */
static void line(int x0, int y0, int x1, int y1, int w, uint32_t color, uint32_t bg) {
    int minx = (x0 < x1 ? x0 : x1) - w, maxx = (x0 > x1 ? x0 : x1) + w;
    int miny = (y0 < y1 ? y0 : y1) - w, maxy = (y0 > y1 ? y0 : y1) + w;
    int dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++) {
            int px = x - x0, py = y - y0;
            int t = len2 ? (px * dx + py * dy) * 256 / len2 : 0;
            t = t < 0 ? 0 : t > 256 ? 256 : t;
            int qx = px * 16 - dx * t / 16, qy = py * 16 - dy * t / 16; /* 1/16 px */
            uint32_t a = cover(isqrt(qx * qx + qy * qy), w * 8);
            if (a)
                put(x, y, mix(bg, color, a));
        }
}

static void check(int x, int y, uint32_t color) { /* a tick, 12 px */
    line(x, y + 6, x + 4, y + 10, 2, color, C_BG);
    line(x + 4, y + 10, x + 12, y + 1, 2, color, C_BG);
}

/* eight dots round a small circle, one bright, the rest fading behind it: turns with `frame` */
static const int8_t spin_x[8] = {0, 5, 7, 5, 0, -5, -7, -5}, spin_y[8] = {-7, -5, 0, 5, 7, 5, 0, -5};
static void spinner(int cx, int cy, int frame) {
    fill(cx - 10, cy - 10, 21, 21, C_BG);
    for (int i = 0; i < 8; i++) {
        int age = (frame - i) & 7;
        ring(cx + spin_x[i], cy + spin_y[i], 1, 0, mix(C_BG, C_ACCENT, (uint32_t)(255 - age * 30)), C_BG);
    }
}

static uint64_t ms_of(struct step *s) { return (s->t1 ? s->t1 : time_monotonic_ns()) / 1000000 - s->t0 / 1000000; }

/* the share of the start done, of 1000 */
static int overall(void) {
    int all = 0, got = 0;
    for (int i = 0; i < NSTEPS; i++) {
        struct step *s = &steps[i];
        all += s->weight;
        if (s->state == DONE)
            got += s->weight * 1000;
        else if (s->state == ACTIVE && s->total)
            got += (int)((uint64_t)s->weight * 1000 * (s->done < s->total ? s->done : s->total) / s->total);
    }
    return all ? got / all : 0;
}

static struct step *current(void) {
    for (int i = 0; i < NSTEPS; i++)
        if (steps[i].state == ACTIVE && steps[i].weight)
            return &steps[i];
    return NULL;
}

/* ---- the splash ---- */

static void splash_status(void) {
    int y = (int)H / 2;
    fill(0, y + 14, (int)W, 60, C_BG);
    int bw = 280, bx = ((int)W - bw) / 2;
    fill(bx, y + 22, bw, 3, C_TRACK);
    fill(bx, y + 22, bw * overall() / 1000, 3, C_ACCENT);
    struct step *s = current();
    const char *what = s ? s->doing : "";
    text(&bf_small, ((int)W - text_width(&bf_small, what)) / 2, y + 56, what, C_MUTED, C_BG);
}

static void splash(void) {
    fill(0, 0, (int)W, (int)H, C_BG);
    int y = (int)H / 2;
    ring((int)W / 2, y - 58, 13, 2, C_ACCENT, C_BG);
    const char *name = "Operating System";
    text(&bf_title, ((int)W - text_width(&bf_title, name)) / 2, y - 6, name, C_TEXT, C_BG);
    splash_status();
    const char *hint = "esc   details";
    text(&bf_hint, (int)W - 40 - text_width(&bf_hint, hint), (int)H - 32, hint, mix(C_BG, C_MUTED, 170), C_BG);
}

/* ---- the steps, one line each ---- */

static int list_x(void) { return (int)W > 900 ? ((int)W - 760) / 2 : 40; }
static int row_y(int i) { return 150 + i * 34; }

static void step_row(int i, int frame) {
    struct step *s = &steps[i];
    int x = list_x(), y = row_y(i);
    fill(x - 4, y - 22, 780, 32, C_BG);
    if (s->state == DONE)
        check(x, y - 12, C_ACCENT);
    else if (s->state == ACTIVE)
        spinner(x + 6, y - 6, frame);
    else if (s->state == FAILED_STEP)
        line(x + 1, y - 11, x + 11, y - 1, 2, C_FAIL, C_BG), line(x + 11, y - 11, x + 1, y - 1, 2, C_FAIL, C_BG);
    else
        ring(x + 6, y - 6, 2, 0, mix(C_BG, C_MUTED, 120), C_BG);
    uint32_t ink = s->state == PENDING ? mix(C_BG, C_MUTED, 160) : C_TEXT;
    text(&bf_small, x + 30, y, s->label, ink, C_BG);
    int dx = x + 190;
    if (s->state == ACTIVE && s->total) {
        int bw = 220;
        fill(dx, y - 7, bw, 3, C_TRACK);
        fill(dx, y - 7, (int)((uint64_t)bw * (s->done < s->total ? s->done : s->total) / s->total), 3, C_ACCENT);
        char amount[40];
        ksnprintf(amount, sizeof amount, "%lu / %lu MB", (unsigned long)(s->done >> 20), (unsigned long)(s->total >> 20));
        text(&bf_small, dx + bw + 14, y, amount, C_MUTED, C_BG);
    } else if (s->detail[0]) {
        text(&bf_small, dx, y, s->detail, C_MUTED, C_BG);
    }
    if ((s->state == DONE && s->t1 > 1000000) || (s->state == ACTIVE && s->t0 > 1)) {
        char t[24];
        uint64_t ms = ms_of(s);
        ksnprintf(t, sizeof t, ms < 1000 ? "%lu ms" : "%lu.%lu s", (unsigned long)(ms < 1000 ? ms : ms / 1000),
                  (unsigned long)(ms / 100 % 10));
        text(&bf_small, x + 760 - text_width(&bf_small, t), y, t, mix(C_BG, C_MUTED, 190), C_BG);
    }
}

static void steps_view(int frame) {
    fill(0, 0, (int)W, (int)H, C_BG);
    int x = list_x();
    ring(x + 6, 82, 7, 2, C_ACCENT, C_BG);
    text(&bf_title, x + 30, 92, "Operating System", C_TEXT, C_BG);
    for (int i = 0; i < NSTEPS; i++)
        step_row(i, frame);
    const char *hint = "esc   back";
    text(&bf_hint, (int)W - 40 - text_width(&bf_hint, hint), (int)H - 32, hint, mix(C_BG, C_MUTED, 170), C_BG);
}

/* ---- what the rest of the kernel calls ---- */

static int frame_no;

static void redraw_locked(bool whole, int changed) {
    if (!fb || suspended || failed)
        return;
    if (log_view) {
        if (whole)
            steps_view(frame_no);
        else if (changed >= 0)
            step_row(changed, frame_no);
    } else if (whole) {
        splash();
    } else {
        splash_status();
    }
}

static void redraw(bool whole, int changed) {
    uint64_t f = spin_lock_irqsave(&draw_lock);
    redraw_locked(whole, changed);
    spin_unlock_irqrestore(&draw_lock, f);
}

void bootscreen_init(void) {
    struct limine_framebuffer_response *r = boot_fb_request.response;
    if (!r || r->framebuffer_count == 0)
        return;
    struct limine_framebuffer *f = r->framebuffers[0];
    if (f->bpp != 32 || f->memory_model != LIMINE_FRAMEBUFFER_RGB)
        return;
    fb = f, W = (uint32_t)f->width, H = (uint32_t)f->height;
    redraw(true, -1);
}

bool bootscreen_active(void) { return fb && !suspended; }

void bootscreen_suspend(void) { suspended = true; }

void boot_step(const char *id, int state, const char *detail) {
    struct step *s = find(id);
    if (!s)
        return;
    uint64_t now = time_monotonic_ns();
    if (state == ACTIVE && s->state != ACTIVE)
        s->t0 = now ? now : 1, s->t1 = 0, s->done = s->total = 0;
    if ((state == DONE || state == FAILED_STEP) && !s->t0)
        s->t0 = now;
    if (state == DONE || state == FAILED_STEP)
        s->t1 = now ? now : 1; /* (1: before the clock ran: no time shown) */
    s->state = state;
    if (detail)
        ksnprintf(s->detail, sizeof s->detail, "%s", detail);
    redraw(false, (int)(s - steps));
}

void boot_step_start(const char *id) { boot_step(id, ACTIVE, NULL); }
void boot_step_failed(const char *id, const char *detail) { boot_step(id, FAILED_STEP, detail); }
void boot_step_done(const char *id, const char *detail) { boot_step(id, DONE, detail); }

void boot_step_progress(const char *id, uint64_t done, uint64_t total) {
    struct step *s = find(id);
    if (!s)
        return;
    uint64_t was = s->total ? s->done * 100 / s->total : 0;
    s->done = done, s->total = total;
    if (!total || done * 100 / total != was) /* a redraw per percent */
        redraw(false, (int)(s - steps));
}

void bootscreen_key_esc(void) {
    if (!fb || suspended || failed)
        return;
    uint64_t f = spin_lock_irqsave(&draw_lock);
    log_view = !log_view;
    redraw_locked(true, -1);
    spin_unlock_irqrestore(&draw_lock, f);
}

/* turns the spinners while the steps are shown */
static void animate(void *arg) {
    (void)arg;
    while (!suspended && !failed) {
        thread_sleep_ms(90);
        if (!log_view)
            continue;
        uint64_t f = spin_lock_irqsave(&draw_lock);
        frame_no++;
        for (int i = 0; i < NSTEPS && log_view && !suspended; i++)
            if (steps[i].state == ACTIVE)
                step_row(i, frame_no);
        spin_unlock_irqrestore(&draw_lock, f);
    }
}

void bootscreen_start_animation(void) {
    if (fb)
        thread_create("bootscreen", animate, NULL);
}

/* /proc/bootsteps: one line a step, "id state done total ms label|detail" */
int bootscreen_steps_text(char *buf, unsigned long size) {
    static const char *const names[] = {"pending", "active", "done", "failed"};
    /* first, which view is up: os-boot goes on with the same one */
    size_t n = (size_t)ksnprintf(buf, size, "view %s\n", log_view ? "steps" : "splash");
    for (int i = 0; i < NSTEPS && n < size; i++) {
        struct step *s = &steps[i];
        n += (size_t)ksnprintf(buf + n, size - n, "%s %s %lu %lu %lu %d %s|%s|%s\n", s->id, names[s->state],
                               (unsigned long)s->done, (unsigned long)s->total,
                               (unsigned long)(s->t0 > 1 && s->t1 != 1 ? ms_of(s) : 0), s->weight, s->label, s->doing,
                               s->detail);
    }
    return (int)(n < size ? n : size);
}

/* a program marks its step: "id active|done|failed [detail]" */
int bootscreen_steps_write(const char *buf, unsigned long len) {
    char line[128];
    size_t n = len < sizeof line - 1 ? len : sizeof line - 1;
    memcpy(line, buf, n);
    line[n] = 0;
    while (n && (line[n - 1] == '\n' || line[n - 1] == ' '))
        line[--n] = 0;
    char *sp = line;
    while (*sp && *sp != ' ')
        sp++;
    if (!*sp)
        return -1;
    *sp++ = 0;
    char *detail = sp;
    while (*detail && *detail != ' ')
        detail++;
    if (*detail)
        *detail++ = 0;
    int state = !strcmp(sp, "active") ? ACTIVE : !strcmp(sp, "done") ? DONE : !strcmp(sp, "failed") ? FAILED_STEP : -1;
    if (state < 0 || !find(line))
        return -1;
    boot_step(line, state, *detail ? detail : NULL);
    return 0;
}

/* The system could not start: whatever owns the screen, the end of the log, the failure itself marked. */
void bootscreen_failed(const char *log, unsigned long len, const char *why) {
    if (!fb)
        return;
    failed = true;
    fill(0, 0, (int)W, (int)H, C_BG);
    int x = 48;
    ring(x + 6, 52, 7, 2, C_FAIL, C_BG);
    text(&bf_title, x + 30, 62, "The system couldn't start", C_TEXT, C_BG);
    text(&bf_small, x + 30, 92, why, C_FAIL, C_BG);
    /* the last lines of the log that fit */
    int lines = ((int)H - 160) / 20;
    size_t start = len;
    for (int k = 0; start > 0 && k <= lines; start--)
        if (log[start - 1] == '\n' && ++k > lines)
            break;
    int y = 140;
    char l[200];
    while (start < len && y < (int)H - 20) {
        size_t e = start;
        while (e < len && log[e] != '\n')
            e++;
        size_t m = e - start < sizeof l - 1 ? e - start : sizeof l - 1;
        memcpy(l, log + start, m);
        l[m] = 0;
        bool bad = strncmp(l, "*** KERNEL PANIC", 16) == 0;
        text(&bf_hint, x + 30, y, l, bad ? C_FAIL : C_MUTED, C_BG);
        y += 20;
        start = e + 1;
    }
}

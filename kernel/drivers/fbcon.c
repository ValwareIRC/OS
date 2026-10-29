#include <font8x8_basic.h>
#include <kernel/boot.h>
#include <kernel/drivers.h>
#include <kernel/lib.h>

/* Boot/emergency text console. Glyphs are the public-domain 8x8 font scaled
 * 2x. Deliberately plain: the user's real desktop is designed by their AI. */

#define SCALE  2
#define CELL_W (8 * SCALE)
#define CELL_H (8 * SCALE + 4)
#define MARGIN 24

static struct limine_framebuffer *fb;
static uint32_t cols, rows, cx, cy;
static uint32_t fg, bg;

static uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << fb->red_mask_shift) | ((uint32_t)g << fb->green_mask_shift) |
           ((uint32_t)b << fb->blue_mask_shift);
}

static inline uint32_t *pixel_row(uint32_t y) {
    return (uint32_t *)((uint8_t *)fb->address + (uint64_t)y * fb->pitch);
}

static void fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    for (uint32_t j = 0; j < h; j++) {
        uint32_t *row = pixel_row(y + j);
        for (uint32_t i = 0; i < w; i++)
            row[x + i] = color;
    }
}

void fbcon_init(void) {
    struct limine_framebuffer_response *r = boot_fb_request.response;
    if (!r || r->framebuffer_count == 0)
        return;
    struct limine_framebuffer *f = r->framebuffers[0];
    if (f->bpp != 32 || f->memory_model != LIMINE_FRAMEBUFFER_RGB)
        return;
    fb = f;
    cols = (fb->width - 2 * MARGIN) / CELL_W;
    rows = (fb->height - 2 * MARGIN) / CELL_H;
    bg = rgb(0x14, 0x10, 0x10);
    fg = rgb(0xf2, 0xd7, 0xc9);
    fill_rect(0, 0, fb->width, fb->height, bg);
}

static void draw_glyph(uint32_t col, uint32_t row, char c) {
    const unsigned char *g = font8x8_basic[(unsigned char)c & 0x7f];
    uint32_t px = MARGIN + col * CELL_W, py = MARGIN + row * CELL_H;
    for (uint32_t y = 0; y < 8 * SCALE; y++) {
        uint32_t *line = pixel_row(py + y) + px;
        uint8_t bits = g[y / SCALE];
        for (uint32_t x = 0; x < 8 * SCALE; x++)
            line[x] = (bits >> (x / SCALE)) & 1 ? fg : bg;
    }
}

static void scroll(void) {
    uint32_t top = MARGIN, line_bytes = CELL_H * fb->pitch;
    uint8_t *base = (uint8_t *)fb->address + (uint64_t)top * fb->pitch;
    memmove(base, base + line_bytes, (uint64_t)(rows - 1) * line_bytes);
    fill_rect(0, MARGIN + (rows - 1) * CELL_H, fb->width, CELL_H, bg);
}

static void newline(void) {
    cx = 0;
    if (++cy >= rows) {
        scroll();
        cy = rows - 1;
    }
}

static bool suspended;

void fbcon_suspend(bool on) { suspended = on; }

void fbcon_write(const char *s, uint64_t len) {
    if (!fb || suspended)
        return;
    for (uint64_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\n') {
            newline();
        } else if (c == '\r') {
            cx = 0;
        } else {
            draw_glyph(cx, cy, c);
            if (++cx >= cols)
                newline();
        }
    }
}

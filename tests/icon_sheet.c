/* Draws every icon subject in every look onto one sheet (a PPM on stdout),
 * on light and dark ground, so the icon art can be checked without booting.
 *   tests/icon_sheet.sh OUT.png */
#include "../userland/shell/design.h"
#include "../userland/shell/gfx.h"
#include "../userland/shell/iconart.h"
#include <stdio.h>

int main(void) {
    const int subj[] = {APP_FILES, APP_COMPANION, APP_NOTES, APP_SETTINGS, APP_ABOUT, APP_TERMINAL, APP_REDESIGN,
                        APP_POWER, ICON_DOCUMENT, ICON_IMAGE, ICON_AUDIO, ICON_VIDEO, ICON_ARCHIVE, ICON_CODE,
                        ICON_EXECUTABLE};
    const int ns = (int)(sizeof subj / sizeof subj[0]);
    const struct {
        enum icon_look look;
        int tile;
        color tint;
    } rows[] = {{LOOK_COLOR, 0, 0},          {LOOK_COLOR, 2, 0},          {LOOK_COLOR, 1, 0},
                {LOOK_MONO, 0, RGBA(20, 20, 30, 255)}, {LOOK_MONO, 2, RGBA(40, 90, 200, 255)},
                {LOOK_CLASSIC, 0, 0},        {LOOK_CLASSIC, 3, 0}};
    const int nr = (int)(sizeof rows / sizeof rows[0]);
    const int sz = 64, cell = 80, W = ns * cell * 2 + 20, H = nr * cell + 3 * 140 + 20;
    canvas *c = canvas_new(W, H);
    fill_rect(c, (rect){0, 0, W / 2, H}, RGBA(240, 240, 236, 255));
    fill_rect(c, (rect){W / 2, 0, W - W / 2, H}, RGBA(30, 34, 44, 255));
    for (int half = 0; half < 2; half++)
        for (int r = 0; r < nr; r++)
            for (int s = 0; s < ns; s++) {
                color tint = rows[r].tint;
                if (half && rows[r].look == LOOK_MONO)
                    tint = RGBA(235, 235, 240, 255);
                int x = half * (W / 2) + 10 + s * cell, y = 10 + r * cell;
                icon_art(c, (rect){x, y, sz, sz}, subj[s], rows[r].look, rows[r].tile, tint, 1, false);
            }
    /* sizes: small, as in a list, and big, as on a launcher */
    int y = 10 + nr * cell;
    for (int s = 0; s < ns; s++) {
        icon_art(c, (rect){10 + s * 40, y, 24, 24}, subj[s], LOOK_COLOR, 0, 0, 1, false);
        icon_art(c, (rect){10 + s * 40, y + 30, 32, 32}, subj[s], LOOK_CLASSIC, 0, 0, 1, false);
    }
    for (int s = 0; s < 8; s++)
        icon_art(c, (rect){10 + s * 136, y + 80, 128, 128}, subj[s], LOOK_COLOR, s & 1 ? 2 : 0, 0, 1, s == 0);
    for (int s = 8; s < ns; s++)
        icon_art(c, (rect){W / 2 + 10 + (s - 8) * 136, y + 80, 128, 128}, subj[s], LOOK_COLOR, 0, 0, 1, false);
    printf("P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint32_t p = c->px[i];
        putchar((int)C_R(p));
        putchar((int)C_G(p));
        putchar((int)C_B(p));
    }
    return 0;
}

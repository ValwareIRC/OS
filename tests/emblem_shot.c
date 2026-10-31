/* Every emblem the companion can be, in each of its states, drawn by the real
 * engine on the build machine: one sheet to look at.
 *   tests/emblem_shot.sh OUT.png */
#define _GNU_SOURCE
#include "../userland/shell/app.h"
#include "../userland/shell/emblem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

app A;
/* the voice isn't here: a still picture */
bool voice_listening(void) { return false; }
bool voice_speaking(void) { return false; }
uint64_t now_ms(void) { return A.now; }

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "emblems.png";
    fonts_init(argc > 2 ? argv[2] : "build/ports/fonts");
    static const int states[] = {ES_IDLE, ES_THINKING, ES_SPEAKING, ES_WORKING};
    static const char *const sname[] = {"idle", "thinking", "speaking", "working"};
    int cols = EM_COUNT, rows = 4, cell = 220;
    A.W = cols * cell, A.H = rows * cell + 40;
    A.frame = canvas_new(A.W, A.H);
    fill_vgradient(A.frame, (rect){0, 0, A.W, A.H}, RGBA(246, 245, 255, 255), RGBA(236, 240, 252, 255));
    font *f = font_get("Inter", 14);
    color a = RGBA(124, 108, 255, 255), b = RGBA(255, 143, 184, 255);
    for (int s = 0; s < rows; s++)
        for (int k = 0; k < cols; k++) {
            A.now = 4000 + (uint64_t)k * 333 + (uint64_t)s * 977; /* a different moment each */
            /* turn it a little first, as if it had been running */
            for (int i = 0; i < 20; i++) {
                A.now += 50;
                canvas *scratch = canvas_new(8, 8);
                emblem_draw_kind(scratch, k, 4, 4, 2, a, b, states[s], 0.6f);
                canvas_free(scratch);
            }
            float cx = k * cell + cell / 2.0f, cy = 40 + s * cell + cell / 2.0f - 10;
            emblem_draw_kind(A.frame, k, cx, cy, 54, a, b, states[s], 0.7f);
            char lab[64];
            snprintf(lab, sizeof lab, "%s, %s", emblem_name(k), sname[s]);
            draw_text_centered(A.frame, f, (rect){k * cell, 40 + s * cell + cell - 34, cell, 20}, lab, RGBA(80, 84, 120, 255));
        }
    unsigned char *rgb = malloc((size_t)A.W * A.H * 3);
    for (int i = 0; i < A.W * A.H; i++) {
        uint32_t p = A.frame->px[i];
        rgb[i * 3] = C_R(p), rgb[i * 3 + 1] = C_G(p), rgb[i * 3 + 2] = C_B(p);
    }
    stbi_write_png(out, A.W, A.H, 3, rgb, A.W * 3);
    printf("wrote %s\n", out);
    return 0;
}

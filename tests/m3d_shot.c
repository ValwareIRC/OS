/* Renders a .glb the way the interface will (userland/shell/m3d), to PNG, on the host:
 *   gcc -O2 -I build/ports/include -I userland/shell tests/m3d_shot.c userland/shell/m3d/*.c userland/lib/json.c \
 *       -lm -lpthread -o build/m3d_shot
 *   build/m3d_shot MODEL.glb OUT.png [W H] [yaw pitch] [--anim NAME T] [--hover NODE] [--camera]
 * Prints what it found in the model (parts and what they do, animations) and how long drawing took. */
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include "m3d/m3d.h"

unsigned char *m3_image_decode(const unsigned char *data, int len, int *w, int *h) {
    int c;
    return stbi_load_from_memory(data, len, w, h, &c, 4);
}
void m3_image_free(void *px) { stbi_image_free(px); }
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: m3d_shot MODEL.glb OUT.png [W H] [yaw pitch] [--anim NAME T] [--hover NODE] [--camera]\n");
        return 2;
    }
    char why[256];
    m3_model *m = m3_load_file(argv[1], why, sizeof why);
    if (!m) {
        fprintf(stderr, "can't load: %s\n", why);
        return 1;
    }
    int w = 640, h = 480, pos = 0;
    m3_view v;
    m3_view_default(&v);
    int anim = -1;
    float t = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--anim") && i + 2 < argc) {
            anim = m3_anim_find(m, argv[i + 1]), t = (float)atof(argv[i + 2]), i += 2;
        } else if (!strcmp(argv[i], "--hover") && i + 1 < argc) {
            for (int k = 0; k < m->nnodes; k++)
                if (!strcmp(m->nodes[k].name, argv[i + 1]) && m->nodes[k].part >= 0)
                    m->nodes[m->nodes[k].part].glow = 1, m->nodes[m->nodes[k].part].lift[1] = 0.05f;
            i++;
        } else if (!strcmp(argv[i], "--camera")) {
            v.use_camera = true;
        } else if (!strcmp(argv[i], "--ss") && i + 1 < argc) {
            v.supersample = atoi(argv[++i]);
        } else {
            float x = (float)atof(argv[i]);
            if (pos == 0) w = (int)x;
            else if (pos == 1) h = (int)x;
            else if (pos == 2) v.yaw = x;
            else if (pos == 3) v.pitch = x;
            pos++;
        }
    }
    printf("%d nodes, %d meshes, %d materials, %d textures, %d animations, %d parts\n", m->nnodes, m->nmeshes, m->nmats,
           m->ntexs, m->nanims, m->nparts);
    for (int i = 0; i < m->nnodes; i++)
        if (m->nodes[i].part == i)
            printf("  part %s: %s%s%s\n", m->nodes[i].name, m->nodes[i].action, m->nodes[i].hover[0] ? " hover " : "",
                   m->nodes[i].hover);
    for (int i = 0; i < m->nanims; i++)
        printf("  animation %s: %.2fs, %d channels\n", m->anims[i].name, m->anims[i].duration, m->anims[i].nch);
    int which[2] = {anim, -1};
    float times[1] = {t};
    m3_pose(m, which, times);
    uint32_t *px = malloc(sizeof *px * (size_t)w * h);
    uint16_t *pick = malloc(sizeof *pick * (size_t)w * h);
    m3_render(m, &v, px, pick, w, h); /* the first draw prepares the studio's maps */
    double ms = m3_render(m, &v, px, pick, w, h);
    printf("drawn %dx%d in %.1f ms\n", w, h, ms);
    /* over a checker, so transparency shows; premultiplied ARGB to RGBA */
    unsigned char *rgb = malloc((size_t)w * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t p = px[y * w + x];
            int a = p >> 24, bg = ((x / 16 + y / 16) & 1) ? 236 : 214;
            for (int c = 0; c < 3; c++) {
                int s = (p >> (16 - 8 * c)) & 255;
                rgb[(y * w + x) * 3 + c] = (unsigned char)(s + bg * (255 - a) / 255);
            }
        }
    stbi_write_png(argv[2], w, h, 3, rgb, w * 3);
    /* which parts are where, for checking clicks */
    int counts[64] = {0};
    for (int i = 0; i < w * h; i++) {
        int part = m3_part_at(m, pick, w, h, i % w, i / w);
        if (part >= 0 && part < 64)
            counts[part]++;
    }
    for (int i = 0; i < 64 && i < m->nnodes; i++)
        if (counts[i])
            printf("  %s covers %d px\n", m->nodes[i].name, counts[i]);
    free(px), free(pick), free(rgb);
    m3_free(m);
    return 0;
}

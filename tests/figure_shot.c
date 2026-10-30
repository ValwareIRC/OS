/* Renders the companion's generated figure to a picture, without the OS:
 *   build/host/figure_shot OUT.jpg [sheet|body|portrait] [style]
 * sheet: the face's expressions, head and shoulders; body: the whole figure
 * from four sides, relaxed and in its T-pose; portrait: one face-cam frame.
 * Prints how long things took. FIG_DEBUG=1 draws the light that reaches
 * each point; FIG_SHADOWS=1 adds the live shadow map. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "../userland/figure/anim.h"
#include "../userland/figure/figure.h"
#include "../userland/figure/face.h"
#include "../userland/figure/fig.h"
#include "../userland/figure/hair.h"
#include "../userland/figure/head.h"
#include "../userland/figure/outfit.h"
#include "../userland/figure/prim.h"

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static v3 key_light(void) { return vnorm(getenv("FIG_LIGHT_TOP") ? V3(0, 1, 0.35f) : V3(-0.5f, 0.65f, 0.75f)); }

/* the test looks: 0 the default, 1 casual with long hair, 2 a dark dress */
static void build(figure *f, int style) {
    look lk;
    look_default(&lk);
    if (style == 1) {
        lk.hair.style = HAIR_LONG, lk.hair.length = 0.22f, lk.hair.bangs = BANGS_FULL, lk.hair.color = 0xc9b8f0,
        lk.hair.tip = 0x8fd0ff, lk.hair.curl = 0.2f;
        outfit_params *op = &lk.outfit;
        op->top = TOP_TEE, op->bottom = BOTTOM_SHORTS, op->legs = LEGS_KNEESOCKS, op->shoes = SHOES_SNEAKERS,
        op->top_col = 0x2c2a4a, op->top_pattern = PAT_STARS, op->top_col2 = 0xffd95a, op->bottom_col = 0x6a8ad8,
        op->bottom_pattern = PAT_NONE, op->legs_col = 0xf4f2f8, op->shoes_col = 0xe85a8a, op->sole_col = 0xf4f2f8;
    } else if (style == 2) {
        lk.hair.style = HAIR_HIME, lk.hair.length = 0.3f, lk.hair.bangs = BANGS_BLUNT, lk.hair.color = 0x23202e,
        lk.hair.tip = 0x3a3352, lk.hair.curl = 0;
        outfit_params *op = &lk.outfit;
        op->top = TOP_DRESS, op->bottom = BOTTOM_NONE, op->legs = LEGS_TIGHTS, op->shoes = SHOES_BOOTS, op->top_col = 0x1e1a2a,
        op->neckline = NECK_HIGH, op->legs_col = 0x14121c, op->shoes_col = 0x2a2030, op->skirt_len = 1.2f, op->flare = 0.8f;
    }
    double t0 = now_ms();
    if (getenv("FIG_VRM")) { /* someone's own model instead */
        FILE *vf = fopen(getenv("FIG_VRM"), "rb");
        if (!vf) {
            printf("no file %s\n", getenv("FIG_VRM"));
            exit(1);
        }
        fseek(vf, 0, SEEK_END);
        long n = ftell(vf);
        fseek(vf, 0, SEEK_SET);
        unsigned char *d = malloc((size_t)n);
        if (fread(d, 1, (size_t)n, vf) != (size_t)n)
            exit(1);
        fclose(vf);
        char why[200];
        if (!figure_load_vrm(f, d, (size_t)n, why, sizeof why)) {
            printf("VRM refused: %s\n", why);
            exit(1);
        }
        free(d);
        fpose rest;
        fig_pose_rest(&f->m, &rest);
        body_pose_relaxed(&f->body, &rest);
        fig_pose_update(&f->m, &rest);
        fig_bake_sun(&f->m, &rest, key_light(), 8);
        printf("VRM %s by %s (%s): %d bones, %d meshes\n", f->vrm_title, f->vrm_author, f->vrm_licence, f->m.nbones, f->m.nmeshes);
    } else
        figure_build(f, &lk, getenv("FIG_CELL") ? (float)atof(getenv("FIG_CELL")) : 0);
    printf("figure built in %.0f ms\n", now_ms() - t0);
}

/* ---- drawing a view ---- */

typedef struct {
    float yaw, pitch;   /* camera round the target (radians) */
    float dist, fov;
    float tilt;         /* head turn, for liveliness */
    face_state fs;
    int full;           /* 1: the whole figure, 2: the whole figure in T-pose, 3: waist up */
    const fpose *posed; /* a pose made elsewhere (the life layer), or NULL */
} view;

static void render_view(figure *f, const view *v, uint32_t *dst, int stride, int W, int H, double *ms) {
    fmodel *m = &f->m;
    body_info *bi = &f->body;
    fpose pose;
    if (v->posed)
        pose = *v->posed;
    else {
        fig_pose_rest(m, &pose);
        if (v->full != 2)
            body_pose_relaxed(bi, &pose);
        pose.local[bi->bone[BN_HEAD]] = qeuler(0.05f, v->tilt, v->tilt * -0.25f);
        fig_pose_update(m, &pose);
    }
    double t0 = now_ms();
    figure_face(f, &v->fs);
    if (f->vrm)
        figure_vrm_face(f, &v->fs, &pose);
    double t1 = now_ms();
    v3 target = v->full == 3 ? V3(0, bi->chest_y + 0.02f * bi->s, 0)
                : v->full ? V3(0, bi->H * 0.5f, 0)
                          : vadd(f->head.center, V3(0, -0.045f * bi->s, 0));
    v3 eye = vadd(target, V3(sinf(v->yaw) * cosf(v->pitch) * v->dist, sinf(v->pitch) * v->dist,
                             cosf(v->yaw) * cosf(v->pitch) * v->dist));
    fcam cam = {.view = m4lookat(eye, target, V3(0, 1, 0)), .fovy = v->fov, .znear = 0.05f, .zfar = 30, .w = W, .h = H, .ss = 2};
    flight L = {.dir = key_light(), .color = 0xfff6ee, .ambient = 0xc4c8e6, .rim_col = 0x8f9fe8, .rim_power = 3,
                .shadows = getenv("FIG_SHADOWS") != NULL, .shadow_size = 320};
    fbuf b;
    if (!fig_buf_init(&b, W * 2, H * 2))
        return;
    uint32_t *out = malloc(sizeof *out * W * H);
    double t2 = now_ms();
    fig_render(&b, &cam, &L, m, &pose);
    fig_resolve(&b, &cam, m, out, NULL, v->full ? 0.7f : 0.9f);
    double t3 = now_ms();
    /* over a soft background */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float g = (float)y / H;
            rgb bg = rgb_mix(0xeceaf4, 0xc9c5dc, g);
            uint32_t s = out[y * W + x];
            float a = ((s >> 24) & 255) / 255.0f;
            int r = (int)(((s >> 16) & 255) + ((bg >> 16) & 255) * (1 - a));
            int gg = (int)(((s >> 8) & 255) + ((bg >> 8) & 255) * (1 - a));
            int bl = (int)((s & 255) + (bg & 255) * (1 - a));
            dst[y * stride + x] = (uint32_t)((r > 255 ? 255 : r) << 16 | (gg > 255 ? 255 : gg) << 8 | (bl > 255 ? 255 : bl));
        }
    if (ms)
        ms[0] += t1 - t0, ms[1] += t3 - t2;
    free(out);
    fig_buf_free(&b);
}

static int save(const char *path, const uint32_t *px, int W, int H) {
    unsigned char *rgbb = malloc((size_t)W * H * 3);
    for (int i = 0; i < W * H; i++)
        rgbb[i * 3] = (px[i] >> 16) & 255, rgbb[i * 3 + 1] = (px[i] >> 8) & 255, rgbb[i * 3 + 2] = px[i] & 255;
    int ok = strstr(path, ".png") ? stbi_write_png(path, W, H, 3, rgbb, W * 3) : stbi_write_jpg(path, W, H, 3, rgbb, 93);
    free(rgbb);
    return ok;
}

static void sheet(figure *f, const view *views, int n, int cols, int cw, int ch, const char *outp) {
    int rows = (n + cols - 1) / cols, W = cw * cols, H = ch * rows;
    uint32_t *img = calloc((size_t)W * H, 4);
    double ms[2] = {0};
    for (int i = 0; i < n; i++)
        render_view(f, &views[i], img + (i / cols) * ch * W + (i % cols) * cw, W, cw, ch, ms);
    printf("per view: face %.2f ms, render+resolve %.2f ms at %dx%d (2x2 supersampled)\n", ms[0] / n, ms[1] / n, cw, ch);
    save(outp, img, W, H);
    free(img);
}

int main(int argc, char **argv) {
    const char *outp = argc > 1 ? argv[1] : "figure.jpg";
    const char *mode = argc > 2 ? argv[2] : "sheet";
    int style = argc > 3 ? atoi(argv[3]) : 0;
    if (getenv("FIG_DEBUG"))
        fig_debug = atoi(getenv("FIG_DEBUG"));
    static figure f;
    double tb = now_ms();
    build(&f, style);
    double te = now_ms();
    int tris = 0;
    for (int i = 0; i < f.m.nmeshes; i++)
        tris += f.m.mesh[i].nt;
    printf("built in %.1f ms: %d bones, %d meshes, %d triangles, %d hair locks\n", te - tb, f.m.nbones, f.m.nmeshes, tris,
           f.hair.nlocks);
    float fov = 0.36f, d = 1.05f;
    if (!strcmp(mode, "body")) {
        float bf = 0.5f, bd = 4.2f;
        view views[] = {
            {0, 0.05f, bd, bf, 0, {FE_OPEN, FB_NEUTRAL, FM_SMILE, 0, 0, 0, 0}, 1},
            {-0.7f, 0.08f, bd, bf, 0, {FE_OPEN, FB_NEUTRAL, FM_SMILE, 0, 0.4f, 0, 0}, 1},
            {-1.57f, 0.05f, bd, bf, 0, {FE_OPEN, FB_NEUTRAL, FM_CLOSED, 0, 0, 0, 0}, 1},
            {3.14f, 0.05f, bd, bf, 0, {FE_OPEN, FB_NEUTRAL, FM_CLOSED, 0, 0, 0, 0}, 1},
            {0, 0.05f, bd, bf, 0, {FE_OPEN, FB_NEUTRAL, FM_SMILE, 0, 0, 0, 0}, 2},
            {0.6f, 0.3f, bd, bf, 0, {FE_HAPPY, FB_RAISED, FM_OPEN_SMILE, 0, 0, 0, 0}, 2},
        };
        sheet(&f, views, 6, 3, 384, 512, outp);
    } else if (!strcmp(mode, "clip") && argc > 4) {
        /* a clip file played on her whole body: 12 moments of it */
        FILE *cf = fopen(argv[4], "rb");
        if (!cf) {
            printf("no clip %s\n", argv[4]);
            return 1;
        }
        fseek(cf, 0, SEEK_END);
        long sz = ftell(cf);
        fseek(cf, 0, SEEK_SET);
        char *txt = malloc((size_t)sz + 1);
        size_t got = fread(txt, 1, (size_t)sz, cf);
        txt[got] = 0;
        fclose(cf);
        static clip c;
        if (!clip_parse(&c, txt)) {
            printf("clip not understood\n");
            return 1;
        }
        clip_bind(&c, &f.m);
        int known = 0;
        for (int i = 0; i < c.nb; i++)
            known += c.map[i] >= 0;
        printf("clip %s: %d frames at %g fps, %d of %d bones known, %d contacts\n", c.name, c.frames, c.fps, known, c.nb,
               c.ncontact);
        static fpose poses[12];
        view views[12];
        float len = clip_length(&c);
        for (int i = 0; i < 12; i++) {
            float t = len * i / 11;
            fig_pose_rest(&f.m, &poses[i]);
            clip_apply(&c, t, &poses[i], 1);
            poses[i].root = clip_root(&c, t);
            fig_pose_update(&f.m, &poses[i]);
            views[i] = (view){0.5f, 0.1f, 4.6f, 0.5f, 0, {FE_OPEN, FB_NEUTRAL, FM_SMILE, 0, 0, 0, 0}, 1, &poses[i]};
        }
        sheet(&f, views, 12, 6, 256, 384, outp);
    } else if (!strcmp(mode, "acts")) {
        /* each gesture, part way through, as the life layer plays it */
        static fpose poses[16];
        view views[16];
        int n = 0;
        for (int i = 0; clip_builtin_names[i] && n < 16; i++) {
            static life L;
            life_init(&L, &f, 7);
            L.mood = MOOD_HAPPY;
            life_act(&L, clip_builtin_names[i]);
            float peak = clip_length(&L.clips[L.act]) * (i == 1 || i == 2 ? 0.22f : 0.5f);
            face_state fs = {0};
            for (double t = 0.0; t <= peak + 1e-6; t += 1.0 / 30)
                life_frame(&L, &f, 10.0 + t, &poses[n], &fs);
            views[n] = (view){0.25f, 0.08f, 2.1f, 0.5f, 0, fs, 3, &poses[n]};
            printf("act %s at %.2f s\n", clip_builtin_names[i], peak);
            life_free(&L);
            n++;
        }
        sheet(&f, views, n, 5, 300, 360, outp);
    } else if (!strcmp(mode, "portrait")) {
        view v = {0, 0.05f, d, fov, 0, {FE_OPEN, FB_NEUTRAL, FM_SMILE, 0, 0, 0, 0}, 0};
        uint32_t *img = calloc(512 * 512, 4);
        render_view(&f, &v, img, 512, 512, 512, NULL);
        save(outp, img, 512, 512);
        free(img);
    } else {
        view views[] = {
            {0, 0.05f, d, fov, 0, {FE_OPEN, FB_NEUTRAL, FM_SMILE, 0, 0, 0, 0}, 0},
            {-0.6f, 0.08f, d, fov, 0.25f, {FE_OPEN, FB_NEUTRAL, FM_OPEN_SMILE, 0, 0.5f, 0.1f, 0}, 0},
            {-1.45f, 0.05f, d, fov, 0, {FE_OPEN, FB_NEUTRAL, FM_CLOSED, 0, 0, 0, 0}, 0},
            {0.35f, 0.15f, d, fov, -0.2f, {FE_HAPPY, FB_RAISED, FM_OPEN_SMILE, 0, 0, 0, 0.4f}, 0},
            {0, 0.05f, d, fov, 0, {FE_WINK_L, FB_RAISED, FM_SMILE, 0, 0, 0, 0}, 0},
            {0.2f, 0.0f, d, fov, 0.1f, {FE_WIDE, FB_RAISED, FM_O, 0.8f, 0, 0, 0}, 0},
            {0, 0.05f, d, fov, 0, {FE_OPEN, FB_NEUTRAL, FM_A, 0.9f, -0.4f, 0.3f, 0}, 0},
            {-0.3f, 0.1f, d, fov, -0.1f, {FE_SAD, FB_WORRY, FM_FROWN, 0, 0.2f, -0.4f, 0}, 0},
            {0.25f, 0.05f, d, fov, 0, {FE_ANGRY, FB_FURROW, FM_GRIT, 0, 0, 0, 0}, 0},
            {0, 0.05f, d, fov, 0, {FE_SPARKLE, FB_RAISED, FM_CAT, 0, 0, 0.1f, 0.3f}, 0},
            {3.0f, 0.1f, d, fov, 0, {FE_OPEN, FB_NEUTRAL, FM_CLOSED, 0, 0, 0, 0}, 0},
            {0.7f, -0.12f, d, fov, 0.15f, {FE_HALF, FB_NEUTRAL, FM_SMALL_O, 0, 0, 0, 0}, 0},
        };
        sheet(&f, views, 12, 4, 384, 384, outp);
    }
    /* the budget: a 256 px portrait, many times */
    {
        view v = {0, 0.05f, d, fov, 0, {FE_OPEN, FB_NEUTRAL, FM_SMILE, 0, 0, 0, 0}, 0};
        int W = 256, H = 256;
        uint32_t *img = calloc((size_t)W * H, 4);
        double ms[2] = {0};
        memset(&fig_stats, 0, sizeof fig_stats);
        for (int i = 0; i < 20; i++)
            render_view(&f, &v, img, W, W, H, ms);
        printf("portrait 256 px: face %.2f ms, render+resolve %.2f ms\n", ms[0] / 20, ms[1] / 20);
        printf("  skin %.2f, shadow %.2f, project %.2f, raster+shade %.2f, resolve %.2f ms\n", fig_stats.skin / 20,
               fig_stats.shadow / 20, fig_stats.project / 20, fig_stats.raster / 20, fig_stats.resolve / 20);
        printf("  cpu time in the raster stage: triangles %.2f, shading %.2f ms\n", fig_stats.rast_cpu / 20,
               fig_stats.shade_cpu / 20);
        free(img);
    }
    printf("wrote %s\n", outp);
    return 0;
}

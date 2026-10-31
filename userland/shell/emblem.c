/* The companion's own look, wherever the OS draws it (the <ai> element of a
 * design, the captions, the companion's window, Settings). Nothing is built
 * in: until she has a look, nothing is drawn, and every design stands on its
 * own without her. Her look is made in conversation, fitted to the design
 * and to what the person wants: a drawing she writes of herself (a small
 * canvas script that shows what she's doing its own way), a picture, a 3D
 * model, or a face (presence.c). She also chooses where it sits: the
 * design's own spot for her, or anywhere on the screen, at any size. */
#include "emblem.h"
#include "app.h"
#include "voice.h"
#include "image.h"
#include "fm/fm.h"
#include "presence.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define EMBLEM_FILE STATE_DIR "/emblem"
#define PLACE_FILE STATE_DIR "/emblem-place"
#define PI 3.14159265f

static struct {
    bool loaded;
    int kind;
    int last_state;
    uint64_t done_at, changed_at;
    uint64_t act_at; /* a gesture (a recorded clip, or [[act:]]) */
} E = {.kind = EM_NONE, .last_state = ES_IDLE};

const char *emblem_name(int kind) {
    if (kind == EM_PICTURE)
        return "picture";
    if (kind == EM_DRAWN)
        return "drawing";
    if (kind == EM_MODEL)
        return "model";
    return "none";
}

int emblem_from_name(const char *s) {
    if (!strcasecmp(s, "picture"))
        return EM_PICTURE;
    if (!strcasecmp(s, "drawing"))
        return EM_DRAWN;
    if (!strcasecmp(s, "model"))
        return EM_MODEL;
    if (!strcasecmp(s, "none"))
        return EM_NONE;
    return -1;
}

int emblem_kind(void) {
    if (!E.loaded) {
        E.loaded = true;
        FILE *f = fopen(EMBLEM_FILE, "r");
        char w[32] = "";
        if (f) {
            if (fscanf(f, "%31s", w) != 1)
                w[0] = 0;
            fclose(f);
        }
        int k = w[0] ? emblem_from_name(w) : -1; /* a look from before (a gem, the Earth...) is gone */
        E.kind = k >= 0 ? k : EM_NONE;
    }
    return E.kind;
}

static void remember(int kind) {
    E.loaded = true;
    E.kind = kind;
    E.changed_at = A.now;
    FILE *f = fopen(EMBLEM_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", emblem_name(kind));
        fclose(f);
    }
    LOG("emblem: %s\n", emblem_name(kind));
}

void emblem_clear(void) { remember(EM_NONE); }

/* ---- where it sits ---- */

static struct {
    bool loaded, free;
    float x, y, size; /* the centre as fractions of the screen; the size in pixels */
} PL;

static void place_load(void) {
    if (PL.loaded)
        return;
    PL.loaded = true;
    FILE *f = fopen(PLACE_FILE, "r");
    if (f) {
        PL.free = fscanf(f, "%f %f %f", &PL.x, &PL.y, &PL.size) == 3;
        fclose(f);
    }
}

bool emblem_set_place(const char *how) {
    while (*how == ' ')
        how++;
    float x, y, s;
    PL.loaded = true;
    if (sscanf(how, "%f %f %f", &x, &y, &s) == 3 && x >= 0 && x <= 1 && y >= 0 && y <= 1 && s >= 8 && s <= 900) {
        PL.free = true, PL.x = x, PL.y = y, PL.size = s;
    } else if (!strncasecmp(how, "slot", 4) || !strncasecmp(how, "design", 6)) {
        PL.free = false;
    } else {
        LOG("emblem: no place like %s\n", how);
        return false;
    }
    FILE *f = fopen(PLACE_FILE, "w");
    if (f) {
        if (PL.free)
            fprintf(f, "%.4f %.4f %.0f\n", PL.x, PL.y, PL.size);
        else
            fputs("slot\n", f);
        fclose(f);
    }
    LOG("emblem: placed %s\n", PL.free ? "freely" : "in the design's spot");
    return true;
}

bool emblem_placed(rect *r) {
    place_load();
    if (!PL.free)
        return false;
    int s = (int)PL.size;
    *r = (rect){(int)(PL.x * A.W) - s / 2, (int)(PL.y * A.H) - s / 2, s, s};
    return true;
}

bool emblem_free_place(rect *r) {
    place_load();
    if (!PL.free || emblem_kind() == EM_NONE)
        return false;
    int s = (int)PL.size;
    *r = (rect){(int)(PL.x * A.W) - s / 2, (int)(PL.y * A.H) - s / 2, s, s};
    return true;
}

bool emblem_draw_free(canvas *c, color a, color b, bool active) {
    rect r;
    if (!emblem_free_place(&r))
        return false;
    return emblem_draw(c, r.x + r.w / 2.0f, r.y + r.h / 2.0f, r.w * 0.42f, a, b, active);
}

/* ---- their own picture ---- */

#define PICTURE_FILE STATE_DIR "/emblem-picture"
static struct {
    image *src;        /* the picture, at most 640 a side */
    image *fit;        /* cut to a square and sized for the last diameter */
    int fit_d;
    bool tried;
} PIC;

static void picture_forget(void) {
    image_free(PIC.src);
    image_free(PIC.fit);
    PIC.src = PIC.fit = NULL;
    PIC.fit_d = 0;
    PIC.tried = false;
}

bool emblem_set_picture(const char *path) {
    image *im = image_load(path, 640);
    if (!im) {
        LOG("emblem: %s isn't a picture this can show\n", path);
        return false;
    }
    image_free(im);
    if (strcmp(path, PICTURE_FILE)) { /* kept: a copy of its own */
        FILE *in = fopen(path, "rb"), *out = fopen(PICTURE_FILE ".new", "wb");
        static char buf[65536];
        size_t n;
        bool ok = in && out;
        while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0)
            ok = fwrite(buf, 1, n, out) == n;
        if (in)
            fclose(in);
        if (out)
            fclose(out);
        if (!ok || rename(PICTURE_FILE ".new", PICTURE_FILE) < 0) {
            unlink(PICTURE_FILE ".new");
            return false;
        }
    }
    picture_forget();
    remember(EM_PICTURE);
    return true;
}

/* The picture as a d x d square, cut from its middle. */
static image *picture_fit(int d) {
    if (!PIC.src && !PIC.tried) {
        PIC.tried = true;
        PIC.src = image_load(PICTURE_FILE, 640);
    }
    if (!PIC.src || d < 2)
        return NULL;
    if (PIC.fit && PIC.fit_d == d)
        return PIC.fit;
    int side = PIC.src->w < PIC.src->h ? PIC.src->w : PIC.src->h;
    image sq = {side, side, malloc(sizeof(uint32_t) * (size_t)side * side), side, side};
    if (!sq.px)
        return NULL;
    int ox = (PIC.src->w - side) / 2, oy = (PIC.src->h - side) / 2;
    for (int y = 0; y < side; y++)
        memcpy(sq.px + (size_t)y * side, PIC.src->px + (size_t)(y + oy) * PIC.src->w + ox, sizeof(uint32_t) * side);
    image_free(PIC.fit);
    PIC.fit = image_resized(&sq, d, d);
    PIC.fit_d = d;
    free(sq.px);
    return PIC.fit;
}

/* ---- what it is doing ---- */

static float lip_level(void) {
    static const struct {
        uint32_t magic, next;
        struct {
            double t;
            float level, f1, f2;
        } e[512];
    } *lip;
    if (!lip) {
        int fd = open("/run/os-lip", O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            return 0.5f;
        void *m = mmap(NULL, sizeof *lip, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED)
            return 0.5f;
        lip = m;
    }
    if (lip->magic != 0x4c495031u)
        return 0.5f;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec / 1e9;
    for (uint32_t k = 1; k <= 64 && k <= lip->next; k++) {
        const typeof(lip->e[0]) *e = &lip->e[(lip->next - k) % 512];
        if (e->t <= now && now - e->t < 0.05)
            return e->level;
    }
    return 0;
}

bool emblem_busy_hint; /* set by others for a moment: working on something (actions, web, designing) */

int emblem_state(bool active, float *level) {
    *level = 0;
    int s = ES_IDLE;
    if (A.companion.error[0] || A.onboard.error[0])
        s = ES_ERROR;
    if (voice_listening())
        s = ES_LISTENING;
    else if (voice_speaking()) {
        s = ES_SPEAKING;
        *level = lip_level();
    } else if (A.companion.pending || A.onboard.pending || active)
        s = ES_THINKING;
    else if (emblem_busy_hint || A.design_job)
        s = ES_WORKING;
    /* a bright moment as thinking or working ends */
    if ((E.last_state == ES_THINKING || E.last_state == ES_WORKING) && s == ES_IDLE)
        E.done_at = A.now;
    E.last_state = s;
    if (s == ES_IDLE && E.done_at && A.now - E.done_at < 700)
        s = ES_DONE;
    return s;
}


/* ---- a drawing the AI wrote: a small canvas script ---- */

#define DRAWING_FILE STATE_DIR "/emblem-drawing.lua"
static struct {
    char *src;
    bool loaded, told;
} DRW;

bool emblem_set_drawing(const char *script) {
    FILE *f = fopen(DRAWING_FILE, "w");
    if (!f)
        return false;
    fputs(script, f);
    fclose(f);
    free(DRW.src);
    DRW.src = strdup(script);
    DRW.loaded = true;
    DRW.told = false;
    remember(EM_DRAWN);
    return true;
}

bool emblem_draw_drawing(canvas *cv, float cx, float cy, float r, color a, color b, int state, float level, float t) {
    (void)t;
    if (!DRW.loaded) {
        DRW.loaded = true;
        FILE *f = fopen(DRAWING_FILE, "r");
        if (f) {
            static char buf[32768];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            DRW.src = strdup(buf);
        }
    }
    if (!DRW.src)
        return false;
    static const char *const sname[] = {"idle", "thinking", "listening", "speaking", "working", "done", "error"};
    float side = r * 2.4f;
    rect box = {(int)(cx - side / 2), (int)(cy - side / 2), (int)side, (int)side};
    char err[200] = "";
    if (fm_canvas_emblem(DRW.src, cv, box, sname[state >= 0 && state <= ES_ERROR ? state : 0], level, a, b, err,
                         sizeof err))
        return true;
    if (!DRW.told && err[0]) { /* once: the companion hears why, and can write it again */
        DRW.told = true;
        LOG("emblem: the drawing won't run: %s\n", err);
        char note[400];
        snprintf(note, sizeof note,
                 "(The drawing you made of yourself stopped with an error: %.200s. Until it's fixed nothing of you "
                 "shows. If they still want it, write it again with [[emblem:draw <canvas>...</canvas>]].)", err);
        companion_note(note, false);
    }
    return false;
}

/* ---- a 3D model of theirs (the figure process draws it, turning) ---- */

#define MODEL_FILE STATE_DIR "/emblem-model.glb"

bool emblem_set_model(const char *path) {
    FILE *in = fopen(path, "rb");
    char magic[4] = {0};
    if (!in || fread(magic, 1, 4, in) != 4 || memcmp(magic, "glTF", 4)) {
        if (in)
            fclose(in);
        LOG("emblem: %s isn't a .glb model\n", path);
        return false;
    }
    rewind(in);
    FILE *out = fopen(MODEL_FILE ".new", "wb");
    static char buf[65536];
    size_t n;
    bool ok = out != NULL;
    while (ok && (n = fread(buf, 1, sizeof buf, in)) > 0)
        ok = fwrite(buf, 1, n, out) == n;
    fclose(in);
    if (out)
        fclose(out);
    if (!ok || rename(MODEL_FILE ".new", MODEL_FILE) < 0) {
        unlink(MODEL_FILE ".new");
        return false;
    }
    remember(EM_MODEL);
    presence_model_changed();
    return true;
}

bool emblem_draw_model(canvas *cv, float cx, float cy, float r, int state, float level) {
    (void)state, (void)level; /* the figure process hears these itself */
    float side = r * 2.6f;
    return presence_draw_model(cv, (rect){(int)(cx - side / 2), (int)(cy - side / 2), (int)side, (int)side});
}

/* ---- their own picture, in a round frame ---- */

struct pic_shade {
    const image *im;
    int x0, y0;
};

static color shade_picture(void *ctx, int x, int y) {
    struct pic_shade *s = ctx;
    int u = x - s->x0, v = y - s->y0;
    if (u < 0 || v < 0 || u >= s->im->w || v >= s->im->h)
        return 0;
    return s->im->px[(size_t)v * s->im->w + u] | 0xff000000u;
}

static bool draw_picture(canvas *cv, float cx, float cy, float r, color a, color b, int state, float level, float t) {
    if (state == ES_SPEAKING)
        r *= 1.0f + 0.05f * level;
    int d = (int)(r * 2);
    image *im = picture_fit(d);
    if (!im)
        return false;
    int x0 = (int)(cx - r), y0 = (int)(cy - r);
    struct pic_shade sh = {im, x0, y0};
    fill_round_shaded(cv, (rect){x0, y0, d, d}, r, shade_picture, &sh);
    /* a gloss across the top, and its rim in the design's colour */
    glow_circle(cv, cx - r * 0.35f, cy - r * 0.45f, r * 0.5f, RGBA(255, 255, 255, 60));
    float rimw = fmaxf(2, r * 0.07f);
    color rim = color_mix(a, RGBA(255, 255, 255, 255), 0.25f);
    if (state == ES_SPEAKING)
        rim = color_mix(rim, RGBA(255, 255, 255, 255), 0.5f * level);
    draw_arc(cv, cx, cy, r - rimw * 0.5f, 0, 2 * PI, rimw, rim);
    if (state == ES_THINKING) /* a light running round the rim */
        draw_arc(cv, cx, cy, r - rimw * 0.5f, t * 4, t * 4 + 1.3f, rimw * 1.4f, color_mix(b, RGBA(255, 255, 255, 255), 0.5f));
    return true;
}

/* ---- the whole look, in its state ---- */

static float act_phase(void);

bool emblem_draw_kind(canvas *cv, int kind, float cx, float cy, float r, color a, color b, int state, float level) {
    float t = (float)(A.now % 1000000) / 1000.0f;
    bool still = A.prof.reduce_motion;
    if (still)
        t = 2.0f, level = 0;
    /* a drawing shows what she's doing its own way */
    if (kind == EM_DRAWN)
        return emblem_draw_drawing(cv, cx, cy, r, a, b, state, level, t);
    if (kind != EM_PICTURE && kind != EM_MODEL)
        return false;
    /* a picture or a model can't show it themselves: a hop for a gesture, a breath of the design's light */
    float ap = act_phase();
    if (ap >= 0)
        cy -= r * 0.28f * sinf(PI * ap);
    if (state != ES_IDLE && !still) {
        float breath = 0.5f + 0.5f * sinf(t * 3.5f);
        glow_circle(cv, cx, cy, r * (1.6f + 0.2f * breath + 0.3f * level), color_alpha(a, 0.10f + 0.06f * breath));
    }
    if (kind == EM_PICTURE)
        return draw_picture(cv, cx, cy, r, a, b, state, level, t);
    return emblem_draw_model(cv, cx, cy, r, state, level);
}

bool emblem_draw(canvas *cv, float cx, float cy, float r, color a, color b, bool active) {
    float level;
    int state = emblem_state(active, &level);
    return emblem_draw_kind(cv, emblem_kind(), cx, cy, r, a, b, state, level);
}

void emblem_act(void) { E.act_at = A.now ? A.now : 1; }

/* How far through a gesture it is (0..1), or -1 when there is none. */
static float act_phase(void) {
    if (!E.act_at || A.prof.reduce_motion)
        return -1;
    float p = (float)(A.now - E.act_at) / 900.0f;
    return p >= 1 ? -1 : p;
}

#pragma once
/* The figure: the companion's body, generated from a description (not a
 * stored model), posed and drawn in software. These are the shapes every
 * part of it shares: meshes skinned to a skeleton, toon materials, a few
 * textures, and the buffer they are drawn into. */
#include <stdbool.h>
#include <stdint.h>

#include "vm.h"

#define FIG_MAX_BONES 192
#define FIG_MAX_MESHES 64
#define FIG_MAX_MATS 40
#define FIG_MAX_TEX 6
#define FIG_MAX_MORPH 8

typedef uint32_t rgb; /* 0xRRGGBB */

typedef struct {
    int w, h;
    uint32_t *px; /* 0xAARRGGBB, opaque */
} ftex;

enum mat_kind { MAT_SKIN, MAT_FACE, MAT_HAIR, MAT_CLOTH, MAT_SHINY, MAT_GLOW, MAT_ROOM, MAT_PLAIN };
enum pattern { PAT_NONE, PAT_STRIPES, PAT_PLAID, PAT_DOTS, PAT_STARS, PAT_GRADIENT, PAT_CHECK };

/* A toon material after MToon (the VRM shader): a lit and a shade colour, a
 * soft or sharp step between them, rim light, and a line around it. */
typedef struct {
    int kind;
    rgb base, shade, line_col;
    float shift;   /* where the shade starts, -1..1 of N.L */
    float toony;   /* width of the step: 0 sharp, 1 smooth */
    float rim;     /* rim light strength */
    float emit;    /* 0..1: lit by itself */
    float spec;    /* a highlight band (hair's ring, shiny things) */
    int tex;       /* -1: none */
    int pattern;
    rgb pat_col, pat_col2;
    float pat_scale;
    bool line;     /* outlined */
    bool two_sided;
    bool cutout;   /* the texture's alpha cuts holes (below one half) */
    bool no_shadow; /* casts no shadow */
    bool line_outer; /* a line only at its outline against nothing (models made elsewhere) */
} fmat;

typedef struct {
    int nv, nt;
    v3 *pos, *nrm;      /* bind pose, model space */
    float *uv;          /* 2 per vertex, or NULL */
    float *aux;         /* 1 per vertex, or NULL: hair, how far along the lock; cloth, a pattern coordinate */
    float *sun;         /* 1 per vertex, or NULL: baked, how much of the key light reaches it */
    uint8_t (*bi)[4];   /* skin: bone indices */
    float (*bw)[4];     /*        and weights (sum 1) */
    int *tri;           /* 3 per triangle */
    uint8_t *tpart;     /* per triangle, or NULL: which part (a lock of hair...) it is, for lines between parts */
    int mat;
    bool hidden;
    /* shapes it can morph towards (the jaw opening...): offsets per vertex, and how far */
    int nmorph;
    v3 *morph[FIG_MAX_MORPH];
    float mw[FIG_MAX_MORPH];
} fmesh;

typedef struct {
    char name[24];
    int parent;
    v3 head;   /* bind position, model space; bind rotation is identity (VRM normalized) */
    v3 tail;   /* where it points to at bind */
} fbone;

typedef struct fmodel {
    fbone bone[FIG_MAX_BONES];
    int nbones;
    fmesh mesh[FIG_MAX_MESHES];
    int nmeshes;
    fmat mat[FIG_MAX_MATS];
    int nmats;
    ftex tex[FIG_MAX_TEX];
    int ntex;
    float height; /* metres, feet to top of head */
} fmodel;

/* A pose: each bone's rotation relative to its parent, and where the root is. */
typedef struct {
    quat local[FIG_MAX_BONES];
    v3 root;          /* offset of the hips from their bind position */
    m4 world[FIG_MAX_BONES]; /* computed: bone to model space */
    m4 skin[FIG_MAX_BONES];  /* computed: bind to posed */
} fpose;

typedef struct {
    v3 dir;        /* towards the light, world space */
    rgb color, ambient, rim_col;
    float rim_power;
    bool shadows;  /* the figure shadows itself */
    int shadow_size; /* texels across the shadow map */
} flight;

typedef struct {
    m4 view;
    float fovy;     /* radians; 0 for orthographic */
    float ortho_h;  /* world height of the view when orthographic */
    float znear, zfar;
    int w, h;       /* output pixels */
    int ss;         /* supersampling factor (1, 2) */
    /* the portal: fragments nearer the camera than this plane (view-space
     * depth) are "in front of the frame" and go to the front layer */
    bool portal;
    v3 portal_p, portal_n; /* a point on the plane and its normal, world space */
} fcam;

/* Drawn at w*ss x h*ss, resolved down to w x h. */
typedef struct {
    int w, h;            /* internal size */
    float *z;            /* view depth, +inf empty */
    uint32_t *col;       /* premultiplied 0xAARRGGBB */
    uint8_t *mat;        /* material + 1; 0 empty */
    uint16_t *id;        /* which part of which mesh, +1; 0 empty (lines go where parts overlap) */
    uint8_t *front;      /* 1: in front of the portal plane */
    float *iz;           /* 1 / depth: the depth test without a division */
    int32_t *vis;        /* the nearest triangle, +1; 0 empty */
    float *aux;          /* its aux value, for the lines between locks */
} fbuf;

/* ---- model building (model.c) ---- */
void fig_model_init(fmodel *m);
void fig_model_free(fmodel *m);
int fig_add_bone(fmodel *m, const char *name, int parent, v3 head, v3 tail);
int fig_find_bone(const fmodel *m, const char *name);
int fig_add_mat(fmodel *m, fmat mt);
fmesh *fig_add_mesh(fmodel *m, int nv, int nt, int mat, bool uv, bool aux);
void fig_mesh_normals(fmesh *me); /* smooth normals from the triangles */
/* every vertex fully on one bone */
void fig_mesh_rigid(fmesh *me, int bone);
int fig_add_tex(fmodel *m, int w, int h);
fmat fig_mat(int kind, rgb base);

/* ---- posing (pose.c) ---- */
void fig_pose_rest(const fmodel *m, fpose *p);
void fig_pose_update(const fmodel *m, fpose *p);

/* ---- drawing (raster.c) ---- */
typedef struct {
    double skin, shadow, project, raster, resolve; /* milliseconds, summed */
    double rast_cpu, shade_cpu; /* the raster stage's parts, summed over threads */
    long tested, written;       /* samples tested and written (fig_debug 2) */
    int frames;
} fig_stats_t;
extern fig_stats_t fig_stats;
extern int fig_debug; /* 1: draw how much light reaches each point, instead of colour */
extern int fig_threads; /* how many threads draw (4) */
/* starts the drawing threads now (they are otherwise started on first use) */
void fig_pool_start(void);
bool fig_buf_init(fbuf *b, int w, int h);
void fig_buf_free(fbuf *b);
void fig_buf_clear(fbuf *b);
void fig_render(fbuf *b, const fcam *cam, const flight *L, const fmodel *m, const fpose *p);
/* outlines from depth and material edges, then down to w x h: premultiplied
 * ARGB into `out` (the back layer, or everything when there's no portal) and,
 * if not NULL, `front` (what is in front of the portal plane) */
void fig_resolve(const fbuf *b, const fcam *cam, const fmodel *m, uint32_t *out, uint32_t *front, float line_px);
/* bakes the key light's shadows into the vertices (see raster.c) */
void fig_bake_sun(fmodel *m, const fpose *p, v3 dir, int samples);

static inline rgb rgb_mix(rgb a, rgb b, float t) {
    t = clampf(t, 0, 1);
    int r = (int)(((a >> 16) & 255) + ((((int)(b >> 16) & 255) - (int)((a >> 16) & 255)) * t));
    int g = (int)(((a >> 8) & 255) + ((((int)(b >> 8) & 255) - (int)((a >> 8) & 255)) * t));
    int bl = (int)((a & 255) + ((((int)b & 255) - (int)(a & 255)) * t));
    return (rgb)(r << 16 | g << 8 | bl);
}
static inline rgb rgb_scale(rgb a, float s) {
    int r = (int)(((a >> 16) & 255) * s), g = (int)(((a >> 8) & 255) * s), b = (int)((a & 255) * s);
    r = r > 255 ? 255 : r, g = g > 255 ? 255 : g, b = b > 255 ? 255 : b;
    return (rgb)(r << 16 | g << 8 | b);
}

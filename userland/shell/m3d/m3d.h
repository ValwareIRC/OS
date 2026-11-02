#pragma once
/* 3D models as part of the interface: a glTF 2.0 model (.glb, as Blender exports it) loaded whole (its node
 * tree, meshes, physically based materials with their textures, animations, the names and custom properties
 * Blender gives objects), posed at any moment of its animations, and drawn in software with physically based
 * shading: metal and roughness, normal, occlusion and emissive maps, a studio environment reflected in it,
 * the desktop's own light and the shadows it casts. Every pixel also says which part of the model it shows, so
 * parts can be hovered and clicked.
 *
 * Nothing here knows about the shell: m3d is used by Facet's <model> (fm/) and by host tests alike. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    float x, y, z;
} m3v;

enum { M3_OPAQUE, M3_MASK, M3_BLEND };

typedef struct m3_tex {
    int w, h, levels;
    uint32_t *mip[12]; /* RGBA8 (R in the low byte), each level half the last */
} m3_tex;

typedef struct m3_mat {
    char name[64];
    float base[4];      /* linear RGBA */
    int base_tex;       /* -1: none (the texture's texcoord set is always 0) */
    float metallic, roughness;
    int mr_tex;         /* G roughness, B metallic */
    int normal_tex;
    float normal_scale;
    int occ_tex;
    float occ_strength;
    float emissive[3];  /* linear, strength included */
    int emissive_tex;
    int alpha_mode;
    float alpha_cutoff;
    bool double_sided, unlit;
    float transmission, ior; /* KHR_materials_transmission / ior: glass */
    float clearcoat, clearcoat_roughness; /* KHR_materials_clearcoat: lacquer, car paint */
} m3_mat;

typedef struct m3_prim {
    int mat;            /* -1: the default material */
    int nverts, nidx;
    float *pos, *nrm, *uv, *tan; /* 3, 3, 2, 4 floats a vertex; nrm/uv/tan may be NULL */
    float *col;         /* COLOR_0 as RGBA, or NULL */
    uint32_t *idx;
    m3v lo, hi;         /* bounds, in the mesh's own space */
} m3_prim;

typedef struct m3_mesh {
    char name[64];
    m3_prim *prims;
    int nprims;
} m3_mesh;

typedef struct m3_node {
    char name[64];
    int parent, mesh, camera, light;
    float t[3], r[4], s[3];       /* as loaded (r: x y z w) */
    float pt[3], pr[4], ps[3];    /* posed: animation applied */
    float world[16];              /* column-major, posed */
    float lift[3];                /* the interface's own nudge (a hovered part rising), in parent space */
    float glow;                   /* 0..1: the interface's own highlight */
    /* what Blender's custom properties (glTF "extras") or the name ("app:files") make of it */
    char action[160], hover[48], cursor[24], title[96];
    int part;                     /* the clickable part it belongs to (itself or an ancestor), -1 for none */
} m3_node;

enum { M3_T, M3_R, M3_S, M3_W };

typedef struct m3_channel {
    int node, path;
    int n, comps;      /* keyframes; floats a value */
    int interp;        /* 0 linear, 1 step, 2 cubic spline */
    float *in, *out;
} m3_channel;

typedef struct m3_anim {
    char name[64];
    m3_channel *ch;
    int nch;
    float duration;
} m3_anim;

typedef struct m3_camera {
    bool ortho;
    float yfov, xmag, ymag, znear, zfar;
    float aspect; /* the picture it was framed for (width / height), 0 if not said */
} m3_camera;

typedef struct m3_light {
    int type;          /* 0 directional, 1 point, 2 spot */
    float color[3], intensity, range, inner, outer;
} m3_light;

typedef struct m3_model {
    m3_node *nodes;
    int nnodes;
    int *roots, nroots;
    m3_mesh *meshes;
    int nmeshes;
    m3_mat *mats;
    int nmats;
    m3_tex *texs;
    int ntexs;
    m3_anim *anims;
    int nanims;
    m3_camera *cams;
    int ncams;
    m3_light *lights;
    int nlights;
    m3v lo, hi;        /* bounds of the whole posed model (world space) */
    int nparts;        /* nodes that are clickable parts */
    char title[96];    /* the scene's own name */
} m3_model;

/* Pictures inside a model (PNG, JPEG) are decoded by whoever uses m3d (the shell's image.c, a test's stb_image):
 * RGBA, 4 bytes a pixel, freed with m3_image_free; NULL if it can't be read. */
unsigned char *m3_image_decode(const unsigned char *data, int len, int *w, int *h);
void m3_image_free(void *px);

/* Loads a .glb (one file holding its buffers and pictures); NULL with the reason in why. */
m3_model *m3_load_glb(const uint8_t *data, size_t len, char *why, size_t whylen);
m3_model *m3_load_file(const char *path, char *why, size_t whylen);
void m3_free(m3_model *m);

/* An animation by name (or a number "0", "1"...), -1 if there is none */
int m3_anim_find(const m3_model *m, const char *name);
/* Poses the model: each animation in `which` (up to 8, -1 ends) at its own time (seconds; looped by the
 * caller if it wants), then the world matrices and the bounds. Without animations: the model as loaded. */
void m3_pose(m3_model *m, const int *which, const float *times);

/* ---- drawing ---- */

typedef struct m3_view {
    /* the camera: orbiting `target` (the model's centre if fit), or the model's own (use_camera) */
    float yaw, pitch;    /* degrees; yaw turns round the up axis */
    float roll;
    float distance;      /* 0: fitted so the model fills the picture */
    float fov;           /* degrees, vertical; 0: 30 */
    bool ortho, use_camera, fit;
    m3v target;
    float zoom;          /* 1: fitted exactly */
    /* light: a key light from a direction (where it comes from), its colour and strength; the environment */
    m3v light_dir;
    float light_color[3], light_intensity;
    float env_intensity;
    float env_top[3], env_horizon[3], env_bottom[3]; /* the studio's sky, horizon and floor (linear) */
    float env_rotation;  /* degrees */
    bool shadows;        /* the model's shadows on itself */
    bool ground_shadow;  /* a soft shadow under it, on whatever it stands on */
    float exposure;      /* 1: as is */
    int supersample;     /* 1..3 */
    int threads;
    uint64_t hover_node; /* unused by the renderer; for callers */
} m3_view;

void m3_view_default(m3_view *v);

/* Draws into out (w x h, premultiplied ARGB, 0xAARRGGBB, transparent where there is no model) and, if pick is
 * given, which node each pixel shows (node+1, 0 for none). Returns milliseconds spent. */
double m3_render(m3_model *m, const m3_view *v, uint32_t *out, uint16_t *pick, int w, int h);
/* The ground shadow alone, for drawing under the model in a layer below: alpha only, into out (w x h) */

/* The clickable part (node index) under a pixel of the last pick buffer, -1 for none */
static inline int m3_part_at(const m3_model *m, const uint16_t *pick, int w, int h, int x, int y) {
    if (!pick || x < 0 || y < 0 || x >= w || y >= h)
        return -1;
    int n = (int)pick[y * w + x] - 1;
    return n >= 0 && n < m->nnodes ? m->nodes[n].part : -1;
}

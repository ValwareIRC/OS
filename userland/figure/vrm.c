/* A VRM model as her body: someone's own avatar instead of a generated one.
 *
 * A VRM is a glTF binary (GLB) with a humanoid map (which node is the hips,
 * the left upper arm ...), a licence, expressions and material settings.
 * Its humanoid bones get our (VRM 1.0) names, so the gestures, the life
 * layer and recorded clips drive it as they drive a generated figure. Her
 * face then moves through what the model has: a blink shape, its jaw, its
 * eye bones. The model is drawn as it was made (its licence may forbid
 * changes), only scaled to her height and turned to face the camera.
 *
 * The bytes are untrusted: every index and size is checked. This runs in
 * os-figure, which the sandbox keeps from touching anything else. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "../lib/json.h"
#include "figure.h"

typedef struct {
    const unsigned char *bin;
    size_t binlen;
    json *root;
    char *why;
    size_t whylen;
} glb;

static bool fail(glb *g, const char *msg) {
    snprintf(g->why, g->whylen, "%s", msg);
    return false;
}

/* an accessor's data: count elements of `comps` components each, as floats */
static float *read_floats(glb *g, int acc, int comps, int *count) {
    json *a = json_at(json_get(g->root, "accessors"), acc);
    if (!a)
        return NULL;
    int n = (int)json_num(json_get(a, "count"), 0), ct = (int)json_num(json_get(a, "componentType"), 0);
    bool norm = json_bool(json_get(a, "normalized"), false);
    json *bv = json_at(json_get(g->root, "bufferViews"), (int)json_num(json_get(a, "bufferView"), -1));
    if (!bv || n <= 0 || n > 4000000)
        return NULL;
    size_t off = (size_t)json_num(json_get(bv, "byteOffset"), 0) + (size_t)json_num(json_get(a, "byteOffset"), 0);
    size_t blen = (size_t)json_num(json_get(bv, "byteLength"), 0);
    int csize = ct == 5126 ? 4 : ct == 5123 || ct == 5122 ? 2 : ct == 5121 || ct == 5120 ? 1 : ct == 5125 ? 4 : 0;
    if (!csize)
        return NULL;
    size_t stride = (size_t)json_num(json_get(bv, "byteStride"), 0);
    if (!stride)
        stride = (size_t)csize * comps;
    size_t start = (size_t)json_num(json_get(bv, "byteOffset"), 0);
    if (off + stride * (size_t)(n - 1) + (size_t)csize * comps > start + blen || start + blen > g->binlen)
        return NULL;
    float *out = malloc(sizeof *out * (size_t)n * comps);
    if (!out)
        return NULL;
    for (int i = 0; i < n; i++) {
        const unsigned char *p = g->bin + off + stride * i;
        for (int c = 0; c < comps; c++) {
            const unsigned char *q = p + (size_t)c * csize;
            float v;
            switch (ct) {
            case 5126: memcpy(&v, q, 4); break;
            case 5125: { unsigned u; memcpy(&u, q, 4); v = (float)u; break; }
            case 5123: { unsigned short u; memcpy(&u, q, 2); v = norm ? u / 65535.0f : u; break; }
            case 5122: { short u; memcpy(&u, q, 2); v = norm ? u / 32767.0f : u; break; }
            case 5121: v = norm ? q[0] / 255.0f : q[0]; break;
            default: v = norm ? (signed char)q[0] / 127.0f : (signed char)q[0]; break;
            }
            out[(size_t)i * comps + c] = v;
        }
    }
    *count = n;
    return out;
}

/* VRM 0.x humanoid names to ours (VRM 1.0's) */
static int bone_of(const char *n) {
    static const struct { const char *vrm; int bn; } map[] = {
        {"hips", BN_HIPS}, {"spine", BN_SPINE}, {"chest", BN_CHEST}, {"upperChest", BN_UPPERCHEST}, {"neck", BN_NECK},
        {"head", BN_HEAD}, {"leftEye", BN_LEYE}, {"rightEye", BN_REYE},
        {"leftShoulder", BN_LSHOULDER}, {"leftUpperArm", BN_LUPPERARM}, {"leftLowerArm", BN_LLOWERARM}, {"leftHand", BN_LHAND},
        {"rightShoulder", BN_RSHOULDER}, {"rightUpperArm", BN_RUPPERARM}, {"rightLowerArm", BN_RLOWERARM}, {"rightHand", BN_RHAND},
        {"leftUpperLeg", BN_LUPPERLEG}, {"leftLowerLeg", BN_LLOWERLEG}, {"leftFoot", BN_LFOOT}, {"leftToes", BN_LTOES},
        {"rightUpperLeg", BN_RUPPERLEG}, {"rightLowerLeg", BN_RLOWERLEG}, {"rightFoot", BN_RFOOT}, {"rightToes", BN_RTOES},
    };
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (!strcmp(map[i].vrm, n))
            return map[i].bn;
    /* fingers: thumb (0.x proximal/intermediate/distal, 1.0 metacarpal/proximal/distal), then the four */
    for (int s = 0; s < 2; s++) {
        const char *side = s ? "right" : "left";
        int base = s ? BN_RFINGERS : BN_LFINGERS;
        size_t sl = strlen(side);
        if (strncmp(n, side, sl))
            continue;
        const char *r = n + sl;
        static const char *const f[5] = {"Thumb", "Index", "Middle", "Ring", "Little"};
        for (int k = 0; k < 5; k++) {
            size_t fl = strlen(f[k]);
            if (strncmp(r, f[k], fl))
                continue;
            const char *part = r + fl;
            int j = -1;
            if (k == 0)
                j = !strcmp(part, "Metacarpal") || !strcmp(part, "Proximal") ? (strcmp(part, "Proximal") ? 0 : -2)
                    : !strcmp(part, "Intermediate") ? 1 : !strcmp(part, "Distal") ? 2 : -1;
            else
                j = !strcmp(part, "Proximal") ? 0 : !strcmp(part, "Intermediate") ? 1 : !strcmp(part, "Distal") ? 2 : -1;
            if (j == -2) /* thumbProximal: 0.x's first thumb bone, unless 1.0 named a metacarpal too */
                j = 0;
            return j < 0 ? -1 : base + (k == 0 ? j : 3 + (k - 1) * 3 + j);
        }
    }
    return -1;
}

static void downscale(unsigned char *src, int w, int h, int maxside, ftex *t) {
    int f = 1;
    while (w / f > maxside || h / f > maxside)
        f *= 2;
    t->w = w / f, t->h = h / f;
    t->px = malloc((size_t)t->w * t->h * 4);
    if (!t->px)
        return;
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            unsigned s[4] = {0};
            for (int j = 0; j < f; j++)
                for (int i = 0; i < f; i++) {
                    const unsigned char *p = src + ((size_t)(y * f + j) * w + (x * f + i)) * 4;
                    for (int c = 0; c < 4; c++)
                        s[c] += p[c];
                }
            unsigned n = (unsigned)(f * f);
            t->px[y * t->w + x] = (s[3] / n) << 24 | (s[0] / n) << 16 | (s[1] / n) << 8 | (s[2] / n);
        }
}

bool figure_load_vrm(figure *f, const unsigned char *data, size_t len, char *why, size_t whylen) {
    memset(f, 0, sizeof *f);
    f->jaw = f->leye = f->reye = -1;
    look_default(&f->lk);
    glb g = {.why = why, .whylen = whylen};
    if (len < 20 || memcmp(data, "glTF", 4))
        return fail(&g, "not a VRM (glTF binary) file");
    unsigned jlen, jtype;
    memcpy(&jlen, data + 12, 4), memcpy(&jtype, data + 16, 4);
    if (jtype != 0x4E4F534A || 20 + (size_t)jlen + 8 > len)
        return fail(&g, "its contents are damaged");
    unsigned blen, btype;
    memcpy(&blen, data + 20 + jlen, 4), memcpy(&btype, data + 24 + jlen, 4);
    if (btype != 0x004E4942 || 28 + (size_t)jlen + blen > len)
        return fail(&g, "its data are damaged");
    g.bin = data + 28 + jlen, g.binlen = blen;
    g.root = json_parse((const char *)data + 20, jlen);
    if (!g.root)
        return fail(&g, "its description can't be read");
    json *ext = json_get(g.root, "extensions"), *v0 = ext ? json_get(ext, "VRM") : NULL, *v1 = ext ? json_get(ext, "VRMC_vrm") : NULL;
    if (!v0 && !v1) {
        json_free(g.root);
        return fail(&g, "a glTF model, but not a VRM (no humanoid in it)");
    }
    /* who made it, and what may be done with it */
    if (v0) {
        json *m = json_get(v0, "meta");
        snprintf(f->vrm_title, sizeof f->vrm_title, "%s", json_str(json_get(m, "title"), "a VRM model"));
        snprintf(f->vrm_author, sizeof f->vrm_author, "%s", json_str(json_get(m, "author"), "someone"));
        snprintf(f->vrm_licence, sizeof f->vrm_licence, "%s%s%s", json_str(json_get(m, "licenseName"), "?"),
                 json_get(m, "otherLicenseUrl") ? " " : "", json_str(json_get(m, "otherLicenseUrl"), ""));
    } else {
        json *m = json_get(v1, "meta");
        snprintf(f->vrm_title, sizeof f->vrm_title, "%s", json_str(json_get(m, "name"), "a VRM model"));
        snprintf(f->vrm_author, sizeof f->vrm_author, "%s", json_str(json_at(json_get(m, "authors"), 0), "someone"));
        snprintf(f->vrm_licence, sizeof f->vrm_licence, "%s", json_str(json_get(m, "licenseUrl"), "?"));
    }
    json *nodes = json_get(g.root, "nodes");
    int nn = json_len(nodes);
    if (nn <= 0 || nn > 4096) {
        json_free(g.root);
        return fail(&g, "it has no skeleton");
    }
    /* each node's parent and place (VRM keeps bind poses as plain offsets) */
    int *parent = malloc(sizeof(int) * nn), *mine = malloc(sizeof(int) * nn), *bn = malloc(sizeof(int) * nn);
    v3 *pos = malloc(sizeof(v3) * nn);
    bool *used = calloc((size_t)nn, 1);
    for (int i = 0; i < nn; i++)
        parent[i] = -1, mine[i] = -1, bn[i] = -1;
    bool tilted = false;
    for (int i = 0; i < nn; i++) {
        json *n = json_at(nodes, i), *ch = json_get(n, "children");
        for (int c = 0; c < json_len(ch); c++) {
            int k = (int)json_num(json_at(ch, c), -1);
            if (k >= 0 && k < nn)
                parent[k] = i;
        }
        json *r = json_get(n, "rotation");
        if (json_get(n, "matrix") || (r && fabs(json_num(json_at(r, 3), 1) - 1) > 1e-4))
            tilted = true;
    }
    if (tilted) {
        free(parent), free(mine), free(bn), free(pos), free(used), json_free(g.root);
        return fail(&g, "its skeleton isn't in VRM's standard (normalized) pose");
    }
    for (int i = 0; i < nn; i++) {
        v3 p = V3(0, 0, 0);
        for (int k = i, guard = 0; k >= 0 && guard < nn; k = parent[k], guard++) {
            json *t = json_get(json_at(nodes, k), "translation");
            if (t)
                p = vadd(p, V3((float)json_num(json_at(t, 0), 0), (float)json_num(json_at(t, 1), 0), (float)json_num(json_at(t, 2), 0)));
        }
        pos[i] = p;
    }
    /* the humanoid map */
    if (v0) {
        json *hb = json_get(json_get(v0, "humanoid"), "humanBones");
        for (int i = 0; i < json_len(hb); i++) {
            json *b = json_at(hb, i);
            int node = (int)json_num(json_get(b, "node"), -1);
            const char *name = json_str(json_get(b, "bone"), "");
            if (node >= 0 && node < nn) {
                bn[node] = bone_of(name);
                if (!strcmp(name, "jaw"))
                    bn[node] = -100;
            }
        }
    } else {
        json *hb = json_get(json_get(v1, "humanoid"), "humanBones");
        for (json *b = hb ? hb->child : NULL; b; b = b->next) {
            int node = (int)json_num(json_get(b, "node"), -1);
            if (node >= 0 && node < nn) {
                bn[node] = bone_of(b->key);
                if (!strcmp(b->key, "jaw"))
                    bn[node] = -100;
            }
        }
    }
    /* bones: every node a skin uses, and the nodes above them */
    json *skins = json_get(g.root, "skins");
    for (int s = 0; s < json_len(skins); s++) {
        json *js = json_get(json_at(skins, s), "joints");
        for (int k = 0; k < json_len(js); k++) {
            int node = (int)json_num(json_at(js, k), -1);
            for (int a = node, guard = 0; a >= 0 && a < nn && guard < nn; a = parent[a], guard++)
                used[a] = true;
        }
    }
    for (int i = 0; i < nn; i++)
        if (bn[i] != -1)
            used[i] = true;
    /* model space: her height, facing +Z (VRM 0.x faces -Z) */
    float top = 0;
    json *meshes = json_get(g.root, "meshes");
    /* height from the meshes' bounds (accessor max of POSITION) */
    for (int mi = 0; mi < json_len(meshes); mi++) {
        json *prims = json_get(json_at(meshes, mi), "primitives");
        for (int p = 0; p < json_len(prims); p++) {
            json *acc = json_at(json_get(g.root, "accessors"), (int)json_num(json_get(json_get(json_at(prims, p), "attributes"), "POSITION"), -1));
            json *mx = acc ? json_get(acc, "max") : NULL;
            if (mx && json_num(json_at(mx, 1), 0) > top)
                top = (float)json_num(json_at(mx, 1), 0);
        }
    }
    if (top <= 0.1f)
        top = 1.6f;
    float scale = 1.55f / top, flip = v0 ? -1.0f : 1.0f; /* 0.x: turned round */
#define CONV(p) V3((p).x * scale * flip, (p).y * scale, (p).z * scale * flip)
    fmodel *m = &f->m;
    fig_model_init(m);
    /* parents first: walk down from each root */
    int *stack = malloc(sizeof(int) * nn), sp = 0;
    for (int i = nn - 1; i >= 0; i--)
        if (parent[i] < 0)
            stack[sp++] = i;
    while (sp > 0 && m->nbones < FIG_MAX_BONES - 40) {
        int i = stack[--sp];
        json *ch = json_get(json_at(nodes, i), "children");
        for (int c = json_len(ch) - 1; c >= 0 && sp < nn; c--) {
            int k = (int)json_num(json_at(ch, c), -1);
            if (k >= 0 && k < nn)
                stack[sp++] = k;
        }
        if (!used[i])
            continue;
        int pb = -1;
        for (int a = parent[i]; a >= 0; a = parent[a])
            if (mine[a] >= 0) {
                pb = mine[a];
                break;
            }
        char nm[24];
        if (bn[i] >= 0)
            snprintf(nm, sizeof nm, "%s", body_bone_names[bn[i]]);
        else
            snprintf(nm, sizeof nm, "n%d", i);
        v3 h = CONV(pos[i]);
        mine[i] = fig_add_bone(m, nm, pb, h, vadd(h, V3(0, 0.05f, 0)));
        if (bn[i] >= 0)
            f->body.bone[bn[i]] = mine[i] + 1; /* +1: 0 means unset, below */
        if (bn[i] == -100)
            f->jaw = mine[i];
    }
    free(stack);
    /* humanoid bones the model lacks: harmless stand-ins, so every part of
     * life can name them */
    body_info *bi = &f->body;
    int got[BN_COUNT];
    for (int b = 0; b < BN_COUNT; b++)
        got[b] = bi->bone[b] - 1;
    if (got[BN_HIPS] < 0 || got[BN_HEAD] < 0 || got[BN_LUPPERARM] < 0 || got[BN_RUPPERARM] < 0) {
        free(parent), free(mine), free(bn), free(pos), free(used), json_free(g.root);
        return fail(&g, "its humanoid map lacks the hips, head or arms");
    }
    for (int b = 0; b < BN_COUNT; b++) {
        if (got[b] >= 0) {
            bi->bone[b] = got[b];
            continue;
        }
        int p = got[BN_HIPS];
        if (b == BN_UPPERCHEST && got[BN_CHEST] >= 0)
            p = got[BN_CHEST];
        if (b >= BN_LFINGERS) {
            int hand = b < BN_RFINGERS ? BN_LHAND : BN_RHAND;
            if (got[hand] >= 0)
                p = got[hand];
        }
        v3 h = m->bone[p].head;
        bi->bone[b] = fig_add_bone(m, body_bone_names[b], p, h, h);
    }
    f->leye = bi->bone[BN_LEYE], f->reye = bi->bone[BN_REYE];
    /* textures */
    json *images = json_get(g.root, "images"), *textures = json_get(g.root, "textures"), *mats = json_get(g.root, "materials");
    int tex_of_image[FIG_MAX_TEX * 4];
    for (int i = 0; i < FIG_MAX_TEX * 4; i++)
        tex_of_image[i] = -1;
    for (int i = 0; i < json_len(images) && i < FIG_MAX_TEX * 4 && m->ntex < FIG_MAX_TEX; i++) {
        json *im = json_at(images, i), *bv = json_at(json_get(g.root, "bufferViews"), (int)json_num(json_get(im, "bufferView"), -1));
        if (!bv)
            continue;
        size_t off = (size_t)json_num(json_get(bv, "byteOffset"), 0), bl = (size_t)json_num(json_get(bv, "byteLength"), 0);
        if (off + bl > g.binlen || bl > (64u << 20))
            continue;
        int w, h, c;
        unsigned char *px = stbi_load_from_memory(g.bin + off, (int)bl, &w, &h, &c, 4);
        if (!px)
            continue;
        if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
            ftex *t = &m->tex[m->ntex];
            downscale(px, w, h, 1024, t);
            if (t->px)
                tex_of_image[i] = m->ntex++;
        }
        stbi_image_free(px);
    }
    int mat_of[64];
    for (int i = 0; i < json_len(mats) && i < 64; i++) {
        json *mt = json_at(mats, i), *pbr = json_get(mt, "pbrMetallicRoughness");
        json *bt = pbr ? json_get(pbr, "baseColorTexture") : NULL;
        int tex = -1;
        if (bt) {
            json *tx = json_at(textures, (int)json_num(json_get(bt, "index"), -1));
            int img = tx ? (int)json_num(json_get(tx, "source"), -1) : -1;
            if (img >= 0 && img < FIG_MAX_TEX * 4)
                tex = tex_of_image[img];
        }
        json *bc = pbr ? json_get(pbr, "baseColorFactor") : NULL;
        rgb base = bc ? (rgb)((int)(json_num(json_at(bc, 0), 1) * 255) << 16 | (int)(json_num(json_at(bc, 1), 1) * 255) << 8 |
                              (int)(json_num(json_at(bc, 2), 1) * 255))
                      : 0xffffff;
        fmat fm = fig_mat(MAT_CLOTH, base);
        fm.tex = tex;
        fm.shift = -0.35f, fm.toony = 0.3f, fm.rim = 0.08f;
        fm.shade = rgb_mix(rgb_scale(base, 0.82f), 0x6a5a8a, 0.18f);
        const char *am = json_str(json_get(mt, "alphaMode"), "OPAQUE");
        fm.cutout = strcmp(am, "OPAQUE") != 0 && tex >= 0;
        fm.two_sided = json_bool(json_get(mt, "doubleSided"), false) || fm.cutout;
        fm.line_col = 0x3a2a34;
        fm.line_outer = true;
        mat_of[i] = fig_add_mat(m, fm);
    }
    /* meshes */
    int mesh_node[256];
    int nmn = 0;
    for (int i = 0; i < nn && nmn < 256; i++)
        if (json_get(json_at(nodes, i), "mesh"))
            mesh_node[nmn++] = i;
    for (int k = 0; k < nmn; k++) {
        json *node = json_at(nodes, mesh_node[k]);
        json *mesh = json_at(meshes, (int)json_num(json_get(node, "mesh"), -1));
        json *skin = json_at(skins, (int)json_num(json_get(node, "skin"), -1));
        json *joints = skin ? json_get(skin, "joints") : NULL;
        json *prims = json_get(mesh, "primitives");
        json *names = json_get(json_get(mesh, "extras"), "targetNames");
        for (int p = 0; p < json_len(prims) && m->nmeshes < FIG_MAX_MESHES; p++) {
            json *pr = json_at(prims, p), *at = json_get(pr, "attributes");
            int nv = 0, nn2 = 0, nuv = 0, nj = 0, nw = 0, ni = 0;
            float *P = read_floats(&g, (int)json_num(json_get(at, "POSITION"), -1), 3, &nv);
            float *N = read_floats(&g, (int)json_num(json_get(at, "NORMAL"), -1), 3, &nn2);
            float *UV = read_floats(&g, (int)json_num(json_get(at, "TEXCOORD_0"), -1), 2, &nuv);
            float *J = read_floats(&g, (int)json_num(json_get(at, "JOINTS_0"), -1), 4, &nj);
            float *WT = read_floats(&g, (int)json_num(json_get(at, "WEIGHTS_0"), -1), 4, &nw);
            float *I = read_floats(&g, (int)json_num(json_get(pr, "indices"), -1), 1, &ni);
            if (!P || !I || ni % 3) {
                free(P), free(N), free(UV), free(J), free(WT), free(I);
                continue;
            }
            int mi = (int)json_num(json_get(pr, "material"), 0);
            fmesh *me = fig_add_mesh(m, nv, ni / 3, mi >= 0 && mi < 64 && mi < json_len(mats) ? mat_of[mi] : 0, UV != NULL, false);
            if (!me) {
                free(P), free(N), free(UV), free(J), free(WT), free(I);
                break;
            }
            int rigid = mine[mesh_node[k]] >= 0 ? mine[mesh_node[k]] : 0;
            for (int i = 0; i < nv; i++) {
                me->pos[i] = CONV(V3(P[i * 3], P[i * 3 + 1], P[i * 3 + 2]));
                me->nrm[i] = N && nn2 == nv ? vnorm(V3(N[i * 3] * flip, N[i * 3 + 1], N[i * 3 + 2] * flip)) : V3(0, 0, 1);
                if (UV && nuv == nv)
                    me->uv[i * 2] = UV[i * 2], me->uv[i * 2 + 1] = UV[i * 2 + 1];
                float s = 0;
                for (int c = 0; c < 4; c++) {
                    int jj = J && nj == nv ? (int)J[i * 4 + c] : -1;
                    int node2 = jj >= 0 && joints ? (int)json_num(json_at(joints, jj), -1) : -1;
                    float w = WT && nw == nv ? WT[i * 4 + c] : (c == 0 ? 1 : 0);
                    int b = node2 >= 0 && node2 < nn && mine[node2] >= 0 ? mine[node2] : rigid;
                    me->bi[i][c] = (uint8_t)b, me->bw[i][c] = w > 0 ? w : 0;
                    s += me->bw[i][c];
                }
                if (s <= 0)
                    me->bi[i][0] = (uint8_t)rigid, me->bw[i][0] = 1, s = 1;
                for (int c = 0; c < 4; c++)
                    me->bw[i][c] /= s;
            }
            bool bad = false;
            for (int t = 0; t < ni; t++) {
                int ix = (int)I[t];
                if (ix < 0 || ix >= nv)
                    bad = true;
                me->tri[t] = ix < 0 || ix >= nv ? 0 : ix;
            }
            (void)bad;
            /* expressions kept as shapes: blink, angry */
            json *targets = json_get(pr, "targets");
            for (int ti = 0; ti < json_len(targets) && me->nmorph < FIG_MAX_MORPH; ti++) {
                const char *tn = json_str(json_at(names, ti), "");
                bool blink = strstr(tn, "Blink") || strstr(tn, "blink") || !strcmp(tn, "Fcl_EYE_Close");
                bool angry = strstr(tn, "Angry") || strstr(tn, "angry");
                if (!blink && !angry)
                    continue;
                int nt = 0;
                float *D = read_floats(&g, (int)json_num(json_get(json_at(targets, ti), "POSITION"), -1), 3, &nt);
                if (!D || nt != nv) {
                    free(D);
                    continue;
                }
                v3 *dv = malloc(sizeof(v3) * nv);
                for (int i = 0; i < nv && dv; i++)
                    dv[i] = V3(D[i * 3] * scale * flip, D[i * 3 + 1] * scale, D[i * 3 + 2] * scale * flip);
                free(D);
                if (!dv)
                    continue;
                int slot = me->nmorph++;
                me->morph[slot] = dv;
                me->mw[slot] = 0;
                int mm = f->nmorph_mesh;
                if (mm == 0 || f->morph_mesh[mm - 1] != m->nmeshes - 1) {
                    if (mm >= 8)
                        continue;
                    f->morph_mesh[mm] = m->nmeshes - 1, f->morph_blink[mm] = f->morph_angry[mm] = -1;
                    f->nmorph_mesh = ++mm;
                }
                if (blink && f->morph_blink[mm - 1] < 0)
                    f->morph_blink[mm - 1] = slot;
                if (angry && f->morph_angry[mm - 1] < 0)
                    f->morph_angry[mm - 1] = slot;
            }
            free(P), free(N), free(UV), free(J), free(WT), free(I);
        }
    }
#undef CONV
    free(parent), free(mine), free(bn), free(pos), free(used);
    json_free(g.root);
    if (m->nmeshes == 0)
        return fail(&g, "no meshes could be read");
    /* the measures life and framing use */
    float H = 0;
    for (int i = 0; i < m->nmeshes; i++)
        for (int v = 0; v < m->mesh[i].nv; v++)
            if (m->mesh[i].pos[v].y > H)
                H = m->mesh[i].pos[v].y;
    bi->H = H, bi->s = H / 1.52f;
    v3 head = m->bone[bi->bone[BN_HEAD]].head, neck = m->bone[bi->bone[BN_NECK]].head;
    v3 eyes = vlerp(m->bone[bi->bone[BN_LEYE]].head, m->bone[bi->bone[BN_REYE]].head, 0.5f);
    if (eyes.y < head.y + 0.01f) /* no eye bones: about where eyes are */
        eyes = vadd(head, V3(0, (H - head.y) * 0.45f, 0.06f * bi->s));
    bi->hips_y = m->bone[bi->bone[BN_HIPS]].head.y;
    bi->chest_y = m->bone[bi->bone[BN_CHEST]].head.y + 0.05f * bi->s;
    bi->neck_y = neck.y;
    bi->shoulder_y = m->bone[bi->bone[BN_LUPPERARM]].head.y;
    bi->shoulder_x = fabsf(m->bone[bi->bone[BN_LUPPERARM]].head.x);
    bi->chin_y = head.y;
    bi->hp = head_defaults();
    bi->hp.h = (H - head.y) * 0.95f;
    f->head.center = V3(head.x, (head.y + H) * 0.5f, head.z + 0.01f * bi->s);
    (void)eyes;
    f->vrm = true;
    f->face_tex = -1;
    snprintf(f->lk.name, sizeof f->lk.name, "%s", f->vrm_title);
    return true;
}

void figure_vrm_face(figure *f, const face_state *fs, fpose *pose) {
    float blink = fs->eyes == FE_CLOSED || fs->eyes == FE_HAPPY ? 1
                  : fs->eyes == FE_HALF                         ? 0.55f
                  : fs->eyes == FE_WINK_L || fs->eyes == FE_WINK_R ? 0.5f
                                                                : 0;
    float angry = fs->eyes == FE_ANGRY || fs->brows == FB_FURROW ? 0.7f : 0;
    for (int k = 0; k < f->nmorph_mesh; k++) {
        fmesh *me = &f->m.mesh[f->morph_mesh[k]];
        if (f->morph_blink[k] >= 0)
            me->mw[f->morph_blink[k]] = blink;
        if (f->morph_angry[k] >= 0)
            me->mw[f->morph_angry[k]] = angry;
    }
    bool changed = false;
    float open = fs->mouth == FM_OPEN_SMILE ? 0.5f : fs->open;
    if (f->jaw >= 0 && open > 0.01f) {
        pose->local[f->jaw] = qaxis(V3(1, 0, 0), open * 0.3f);
        changed = true;
    }
    if (f->leye >= 0 && f->reye >= 0 && (fs->gx != 0 || fs->gy != 0)) {
        quat q = qeuler(-fs->gy * 0.18f, fs->gx * 0.3f, 0);
        pose->local[f->leye] = q, pose->local[f->reye] = q;
        changed = true;
    }
    if (changed)
        fig_pose_update(&f->m, pose);
}

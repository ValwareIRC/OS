/* Reading a glTF 2.0 binary (.glb) whole: nodes, meshes, materials (metal/roughness, with the extensions
 * Blender writes for emission strength, transmission, index of refraction and clearcoat), textures (PNG and
 * JPEG, mipmapped), animations, cameras and lights; and what Blender's object names and custom properties say
 * a part does. Everything read from the file is checked against the file's own sizes first. */
#include "m3d.h"
#include "../../lib/json.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>


#define MAX_COUNT 8000000 /* elements in one accessor */

typedef struct {
    json *root;
    const uint8_t *bin;
    size_t binlen;
    char *why;
    size_t whylen;
} rd;

static void *fail(rd *g, const char *msg) {
    if (g->why && !g->why[0])
        snprintf(g->why, g->whylen, "%s", msg);
    return NULL;
}

static int comps_of(const char *type) {
    return !strcmp(type, "SCALAR") ? 1 : !strcmp(type, "VEC2") ? 2 : !strcmp(type, "VEC3") ? 3 : !strcmp(type, "VEC4") ? 4
         : !strcmp(type, "MAT4") ? 16 : !strcmp(type, "MAT3") ? 9 : !strcmp(type, "MAT2") ? 4 : 0;
}

/* where an accessor's bytes are, checked: start, stride, element size */
static bool locate(rd *g, json *a, int n, int comps, int csize, const uint8_t **start, size_t *stride) {
    json *bvj = json_get(a, "bufferView");
    if (!bvj)
        return false; /* all zeros (sparse only): not used by what Blender writes for meshes */
    json *bv = json_at(json_get(g->root, "bufferViews"), (int)bvj->num);
    if (!bv || (int)json_num(json_get(bv, "buffer"), 0) != 0)
        return false;
    size_t vo = (size_t)json_num(json_get(bv, "byteOffset"), 0), vl = (size_t)json_num(json_get(bv, "byteLength"), 0);
    size_t ao = (size_t)json_num(json_get(a, "byteOffset"), 0);
    size_t st = (size_t)json_num(json_get(bv, "byteStride"), 0);
    size_t elem = (size_t)csize * comps;
    if (!st)
        st = elem;
    if (vo + vl > g->binlen || vo + vl < vo || st < elem)
        return false;
    if (n > 0 && ao + st * (size_t)(n - 1) + elem > vl)
        return false;
    *start = g->bin + vo + ao;
    *stride = st;
    return true;
}

/* an accessor as floats: count elements of `want` components (fewer in the file: the rest 0, alpha 1) */
static float *read_floats(rd *g, int acc, int want, int *count) {
    json *a = json_at(json_get(g->root, "accessors"), acc);
    if (!a)
        return NULL;
    int n = (int)json_num(json_get(a, "count"), 0), ct = (int)json_num(json_get(a, "componentType"), 0);
    int comps = comps_of(json_str(json_get(a, "type"), ""));
    bool norm = json_bool(json_get(a, "normalized"), false);
    int csize = ct == 5126 || ct == 5125 ? 4 : ct == 5123 || ct == 5122 ? 2 : ct == 5121 || ct == 5120 ? 1 : 0;
    if (n <= 0 || n > MAX_COUNT || !comps || !csize)
        return NULL;
    const uint8_t *p0;
    size_t stride;
    if (!locate(g, a, n, comps, csize, &p0, &stride))
        return NULL;
    float *out = malloc(sizeof *out * (size_t)n * want);
    if (!out)
        return NULL;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = p0 + stride * (size_t)i;
        for (int c = 0; c < want; c++) {
            float v = c == 3 && want == 4 ? 1.0f : 0.0f;
            if (c < comps) {
                const uint8_t *q = p + (size_t)c * csize;
                switch (ct) {
                case 5126: memcpy(&v, q, 4); break;
                case 5125: { uint32_t u; memcpy(&u, q, 4); v = (float)u; break; }
                case 5123: { uint16_t u; memcpy(&u, q, 2); v = norm ? u / 65535.0f : u; break; }
                case 5122: { int16_t u; memcpy(&u, q, 2); v = norm ? fmaxf(u / 32767.0f, -1) : u; break; }
                case 5121: v = norm ? q[0] / 255.0f : q[0]; break;
                default: v = norm ? fmaxf((int8_t)q[0] / 127.0f, -1) : (int8_t)q[0]; break;
                }
                if (!isfinite(v))
                    v = 0;
            }
            out[(size_t)i * want + c] = v;
        }
    }
    *count = n;
    return out;
}

static uint32_t *read_indices(rd *g, int acc, int *count, int nverts) {
    json *a = json_at(json_get(g->root, "accessors"), acc);
    if (!a)
        return NULL;
    int n = (int)json_num(json_get(a, "count"), 0), ct = (int)json_num(json_get(a, "componentType"), 0);
    int csize = ct == 5125 ? 4 : ct == 5123 ? 2 : ct == 5121 ? 1 : 0;
    const uint8_t *p0;
    size_t stride;
    if (n <= 0 || n > MAX_COUNT * 3 || !csize || !locate(g, a, n, 1, csize, &p0, &stride))
        return NULL;
    uint32_t *out = malloc(sizeof *out * (size_t)n);
    if (!out)
        return NULL;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = p0 + stride * (size_t)i;
        uint32_t v = 0;
        if (csize == 4)
            memcpy(&v, p, 4);
        else if (csize == 2) {
            uint16_t u;
            memcpy(&u, p, 2);
            v = u;
        } else
            v = p[0];
        if (v >= (uint32_t)nverts) { /* an index past the vertices: the file is broken */
            free(out);
            return NULL;
        }
        out[i] = v;
    }
    *count = n;
    return out;
}

/* ---- textures ---- */

static uint32_t avg4(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8)
        r |= ((((a >> s) & 255) + ((b >> s) & 255) + ((c >> s) & 255) + ((d >> s) & 255) + 2) / 4) << s;
    return r;
}

static void make_mips(m3_tex *t) {
    t->levels = 1;
    int w = t->w, h = t->h;
    while ((w > 1 || h > 1) && t->levels < 12) {
        int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        uint32_t *src = t->mip[t->levels - 1], *dst = malloc(sizeof *dst * (size_t)nw * nh);
        if (!dst)
            break;
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++) {
                int x0 = x * 2 < w ? x * 2 : w - 1, y0 = y * 2 < h ? y * 2 : h - 1;
                int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
                dst[y * nw + x] = avg4(src[y0 * w + x0], src[y0 * w + x1], src[y1 * w + x0], src[y1 * w + x1]);
            }
        t->mip[t->levels++] = dst;
        w = nw, h = nh;
    }
}

static bool load_image(rd *g, json *img, m3_tex *t) {
    json *bvj = json_get(img, "bufferView");
    json *bv = bvj ? json_at(json_get(g->root, "bufferViews"), (int)bvj->num) : NULL;
    if (!bv)
        return false; /* a picture in another file: a .glb keeps everything in itself */
    size_t off = (size_t)json_num(json_get(bv, "byteOffset"), 0), len = (size_t)json_num(json_get(bv, "byteLength"), 0);
    if (off + len > g->binlen || len > 0x7fffffff)
        return false;
    int w, h;
    unsigned char *px = m3_image_decode(g->bin + off, (int)len, &w, &h);
    if (!px)
        return false;
    int f = 1; /* very large pictures: halved until they fit 2048 (the interface never needs more) */
    while (w / f > 2048 || h / f > 2048)
        f *= 2;
    t->w = w / f > 0 ? w / f : 1, t->h = h / f > 0 ? h / f : 1;
    t->mip[0] = malloc(sizeof(uint32_t) * (size_t)t->w * t->h);
    if (!t->mip[0]) {
        m3_image_free(px);
        return false;
    }
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            uint32_t v;
            memcpy(&v, px + ((size_t)(y * f) * w + (size_t)x * f) * 4, 4);
            t->mip[0][y * t->w + x] = v;
        }
    m3_image_free(px);
    make_mips(t);
    return true;
}

/* textures[i] -> the picture it shows (index into m->texs, which follow "images") */
static int tex_ref(rd *g, json *info) {
    if (!info)
        return -1;
    json *t = json_at(json_get(g->root, "textures"), (int)json_num(json_get(info, "index"), -1));
    if (!t)
        return -1;
    json *src = json_get(t, "source");
    if (!src) { /* EXT_texture_webp and friends keep it in an extension */
        json *ext = json_get(t, "extensions");
        for (json *e = ext ? ext->child : NULL; e && !src; e = e->next)
            src = json_get(e, "source");
    }
    return src ? (int)src->num : -1;
}

/* ---- materials ---- */

static void srgb_ok(float *c) {
    for (int i = 0; i < 4; i++)
        c[i] = c[i] < 0 ? 0 : c[i];
}

static void load_material(rd *g, json *mj, m3_mat *mt) {
    memset(mt, 0, sizeof *mt);
    snprintf(mt->name, sizeof mt->name, "%s", json_str(json_get(mj, "name"), ""));
    mt->base[0] = mt->base[1] = mt->base[2] = mt->base[3] = 1;
    mt->metallic = 1, mt->roughness = 1;
    mt->base_tex = mt->mr_tex = mt->normal_tex = mt->occ_tex = mt->emissive_tex = -1;
    mt->normal_scale = 1, mt->occ_strength = 1;
    mt->alpha_cutoff = 0.5f;
    mt->ior = 1.5f;
    json *pbr = json_get(mj, "pbrMetallicRoughness");
    if (pbr) {
        json *bc = json_get(pbr, "baseColorFactor");
        for (int i = 0; i < 4 && bc; i++)
            mt->base[i] = (float)json_num(json_at(bc, i), 1);
        srgb_ok(mt->base);
        mt->base_tex = tex_ref(g, json_get(pbr, "baseColorTexture"));
        mt->metallic = (float)json_num(json_get(pbr, "metallicFactor"), 1);
        mt->roughness = (float)json_num(json_get(pbr, "roughnessFactor"), 1);
        mt->mr_tex = tex_ref(g, json_get(pbr, "metallicRoughnessTexture"));
    }
    json *nt = json_get(mj, "normalTexture");
    mt->normal_tex = tex_ref(g, nt);
    mt->normal_scale = (float)json_num(json_get(nt, "scale"), 1);
    json *ot = json_get(mj, "occlusionTexture");
    mt->occ_tex = tex_ref(g, ot);
    mt->occ_strength = (float)json_num(json_get(ot, "strength"), 1);
    json *ef = json_get(mj, "emissiveFactor");
    for (int i = 0; i < 3 && ef; i++)
        mt->emissive[i] = (float)json_num(json_at(ef, i), 0);
    mt->emissive_tex = tex_ref(g, json_get(mj, "emissiveTexture"));
    const char *am = json_str(json_get(mj, "alphaMode"), "OPAQUE");
    mt->alpha_mode = !strcmp(am, "MASK") ? M3_MASK : !strcmp(am, "BLEND") ? M3_BLEND : M3_OPAQUE;
    mt->alpha_cutoff = (float)json_num(json_get(mj, "alphaCutoff"), 0.5);
    mt->double_sided = json_bool(json_get(mj, "doubleSided"), false);
    json *ext = json_get(mj, "extensions");
    if (ext) {
        json *es = json_get(ext, "KHR_materials_emissive_strength");
        float k = (float)json_num(json_get(es, "emissiveStrength"), 1);
        for (int i = 0; i < 3; i++)
            mt->emissive[i] *= k;
        json *tr = json_get(ext, "KHR_materials_transmission");
        mt->transmission = (float)json_num(json_get(tr, "transmissionFactor"), 0);
        mt->ior = (float)json_num(json_get(json_get(ext, "KHR_materials_ior"), "ior"), 1.5);
        json *cc = json_get(ext, "KHR_materials_clearcoat");
        mt->clearcoat = (float)json_num(json_get(cc, "clearcoatFactor"), 0);
        mt->clearcoat_roughness = (float)json_num(json_get(cc, "clearcoatRoughnessFactor"), 0);
        mt->unlit = json_get(ext, "KHR_materials_unlit") != NULL;
    }
    mt->metallic = fminf(fmaxf(mt->metallic, 0), 1);
    mt->roughness = fminf(fmaxf(mt->roughness, 0), 1);
}

/* ---- meshes ---- */

static void bounds(m3_prim *p) {
    p->lo = (m3v){1e30f, 1e30f, 1e30f}, p->hi = (m3v){-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < p->nverts; i++) {
        const float *v = p->pos + i * 3;
        p->lo.x = fminf(p->lo.x, v[0]), p->lo.y = fminf(p->lo.y, v[1]), p->lo.z = fminf(p->lo.z, v[2]);
        p->hi.x = fmaxf(p->hi.x, v[0]), p->hi.y = fmaxf(p->hi.y, v[1]), p->hi.z = fmaxf(p->hi.z, v[2]);
    }
}

/* smooth normals where the file has none */
static void make_normals(m3_prim *p) {
    p->nrm = calloc((size_t)p->nverts * 3, sizeof(float));
    if (!p->nrm)
        return;
    for (int t = 0; t + 2 < p->nidx; t += 3) {
        const float *a = p->pos + p->idx[t] * 3, *b = p->pos + p->idx[t + 1] * 3, *c = p->pos + p->idx[t + 2] * 3;
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 3; j++)
                p->nrm[p->idx[t + k] * 3 + j] += n[j];
    }
    for (int i = 0; i < p->nverts; i++) {
        float *n = p->nrm + i * 3, l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 1e-12f)
            n[0] /= l, n[1] /= l, n[2] /= l;
        else
            n[1] = 1;
    }
}

/* tangents for a normal map where the file has none: per triangle from the texture coordinates, summed */
static void make_tangents(m3_prim *p) {
    if (!p->uv || !p->nrm)
        return;
    float *acc = calloc((size_t)p->nverts * 6, sizeof(float));
    p->tan = malloc(sizeof(float) * (size_t)p->nverts * 4);
    if (!acc || !p->tan) {
        free(acc), free(p->tan), p->tan = NULL;
        return;
    }
    for (int t = 0; t + 2 < p->nidx; t += 3) {
        uint32_t i0 = p->idx[t], i1 = p->idx[t + 1], i2 = p->idx[t + 2];
        const float *a = p->pos + i0 * 3, *b = p->pos + i1 * 3, *c = p->pos + i2 * 3;
        const float *ua = p->uv + i0 * 2, *ub = p->uv + i1 * 2, *uc = p->uv + i2 * 2;
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float s1 = ub[0] - ua[0], t1 = ub[1] - ua[1], s2 = uc[0] - ua[0], t2 = uc[1] - ua[1];
        float d = s1 * t2 - s2 * t1;
        if (fabsf(d) < 1e-20f)
            continue;
        float r = 1 / d;
        float sd[3], td[3];
        for (int j = 0; j < 3; j++)
            sd[j] = (t2 * e1[j] - t1 * e2[j]) * r, td[j] = (s1 * e2[j] - s2 * e1[j]) * r;
        uint32_t ids[3] = {i0, i1, i2};
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 3; j++)
                acc[ids[k] * 6 + j] += sd[j], acc[ids[k] * 6 + 3 + j] += td[j];
    }
    for (int i = 0; i < p->nverts; i++) {
        const float *n = p->nrm + i * 3, *s = acc + i * 6, *tt = acc + i * 6 + 3;
        float dn = n[0] * s[0] + n[1] * s[1] + n[2] * s[2];
        float x = s[0] - n[0] * dn, y = s[1] - n[1] * dn, z = s[2] - n[2] * dn, l = sqrtf(x * x + y * y + z * z);
        if (l < 1e-12f) { /* any direction across the normal */
            x = fabsf(n[0]) < 0.9f ? 1 : 0, y = x ? 0 : 1, z = 0;
            dn = n[0] * x + n[1] * y;
            x -= n[0] * dn, y -= n[1] * dn, z -= n[2] * dn;
            l = sqrtf(x * x + y * y + z * z);
        }
        float *o = p->tan + i * 4;
        o[0] = x / l, o[1] = y / l, o[2] = z / l;
        float cx = n[1] * o[2] - n[2] * o[1], cy = n[2] * o[0] - n[0] * o[2], cz = n[0] * o[1] - n[1] * o[0];
        o[3] = cx * tt[0] + cy * tt[1] + cz * tt[2] < 0 ? -1.0f : 1.0f;
    }
    free(acc);
}

static bool load_prim(rd *g, json *pj, m3_prim *p, const m3_mat *mats, int nmats) {
    memset(p, 0, sizeof *p);
    int mode = (int)json_num(json_get(pj, "mode"), 4);
    if (mode != 4 && mode != 5 && mode != 6)
        return false; /* points and lines: nothing to show in an interface */
    json *at = json_get(pj, "attributes");
    json *pa = json_get(at, "POSITION");
    if (!pa)
        return false;
    int n = 0, k;
    p->pos = read_floats(g, (int)pa->num, 3, &n);
    if (!p->pos)
        return false;
    p->nverts = n;
    json *a;
    if ((a = json_get(at, "NORMAL")) && (p->nrm = read_floats(g, (int)a->num, 3, &k)) && k != n)
        free(p->nrm), p->nrm = NULL;
    if ((a = json_get(at, "TEXCOORD_0")) && (p->uv = read_floats(g, (int)a->num, 2, &k)) && k != n)
        free(p->uv), p->uv = NULL;
    if ((a = json_get(at, "TANGENT")) && (p->tan = read_floats(g, (int)a->num, 4, &k)) && k != n)
        free(p->tan), p->tan = NULL;
    if ((a = json_get(at, "COLOR_0")) && (p->col = read_floats(g, (int)a->num, 4, &k)) && k != n)
        free(p->col), p->col = NULL;
    json *ij = json_get(pj, "indices");
    uint32_t *raw = NULL;
    int nraw = 0;
    if (ij)
        raw = read_indices(g, (int)ij->num, &nraw, n);
    else {
        raw = malloc(sizeof *raw * (size_t)n);
        for (int i = 0; raw && i < n; i++)
            raw[i] = (uint32_t)i;
        nraw = raw ? n : 0;
    }
    if (!raw)
        return false;
    if (mode == 4) {
        p->idx = raw, p->nidx = nraw - nraw % 3;
    } else { /* strips and fans become triangles */
        int nt = nraw >= 3 ? nraw - 2 : 0;
        p->idx = malloc(sizeof *p->idx * (size_t)nt * 3 + 1);
        for (int i = 0; p->idx && i < nt; i++) {
            uint32_t *o = p->idx + i * 3;
            if (mode == 5)
                o[0] = raw[i + (i & 1)], o[1] = raw[i + 1 - (i & 1)], o[2] = raw[i + 2];
            else
                o[0] = raw[0], o[1] = raw[i + 1], o[2] = raw[i + 2];
        }
        p->nidx = p->idx ? nt * 3 : 0;
        free(raw);
    }
    if (!p->idx)
        return false;
    p->mat = (int)json_num(json_get(pj, "material"), -1);
    if (p->mat >= nmats)
        p->mat = -1;
    if (!p->nrm)
        make_normals(p);
    if (!p->tan && p->mat >= 0 && mats[p->mat].normal_tex >= 0)
        make_tangents(p);
    bounds(p);
    return true;
}

/* ---- nodes: what a part does ---- */

/* Blender adds ".001" to a repeated name */
static void base_name(const char *in, char *out, size_t n) {
    snprintf(out, n, "%s", in);
    char *dot = strrchr(out, '.');
    if (dot && strlen(dot) == 4 && isdigit((unsigned char)dot[1]) && isdigit((unsigned char)dot[2]) &&
        isdigit((unsigned char)dot[3]))
        *dot = 0;
}

static void take_extras(m3_node *nd, json *extras) {
    for (json *e = extras ? extras->child : NULL; e; e = e->next) {
        if (!e->key || e->type != JSON_STRING)
            continue;
        if (!strcasecmp(e->key, "action") || !strcasecmp(e->key, "click") || !strcasecmp(e->key, "onclick"))
            snprintf(nd->action, sizeof nd->action, "%s", e->str);
        else if (!strcasecmp(e->key, "hover"))
            snprintf(nd->hover, sizeof nd->hover, "%s", e->str);
        else if (!strcasecmp(e->key, "cursor"))
            snprintf(nd->cursor, sizeof nd->cursor, "%s", e->str);
        else if (!strcasecmp(e->key, "title") || !strcasecmp(e->key, "tooltip") || !strcasecmp(e->key, "label"))
            snprintf(nd->title, sizeof nd->title, "%s", e->str);
        else if (!strcasecmp(e->key, "app"))
            snprintf(nd->action, sizeof nd->action, "open:%s", e->str);
    }
}

/* "app:files", "open:notes", "action:toggle:#menu", "url:https://..." in the name: what clicking it does */
static void name_action(m3_node *nd) {
    char nm[64];
    base_name(nd->name, nm, sizeof nm);
    static const char *const verbs[] = {"app:", "open:", "action:", "url:", "run:", "toggle:", "show:", "do:"};
    for (size_t i = 0; i < sizeof verbs / sizeof *verbs && !nd->action[0]; i++) {
        size_t l = strlen(verbs[i]);
        if (strncasecmp(nm, verbs[i], l) || !nm[l])
            continue;
        if (i == 0)
            snprintf(nd->action, sizeof nd->action, "open:%s", nm + l);
        else if (i == 2 || i == 7)
            snprintf(nd->action, sizeof nd->action, "%s", nm + l);
        else
            snprintf(nd->action, sizeof nd->action, "%s", nm);
    }
}

static void quat_norm(float *q) {
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (l < 1e-12f)
        q[0] = q[1] = q[2] = 0, q[3] = 1;
    else
        for (int i = 0; i < 4; i++)
            q[i] /= l;
}

/* a node's matrix given directly: taken apart into translation, rotation and scale (no shear) */
static void decompose(const float *m, float *t, float *r, float *s) {
    t[0] = m[12], t[1] = m[13], t[2] = m[14];
    for (int c = 0; c < 3; c++)
        s[c] = sqrtf(m[c * 4] * m[c * 4] + m[c * 4 + 1] * m[c * 4 + 1] + m[c * 4 + 2] * m[c * 4 + 2]);
    float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
    if (det < 0)
        s[0] = -s[0];
    float R[9];
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 3; k++)
            R[c * 3 + k] = s[c] != 0 ? m[c * 4 + k] / s[c] : 0;
    float tr = R[0] + R[4] + R[8];
    if (tr > 0) {
        float q = sqrtf(tr + 1) * 2;
        r[3] = q / 4, r[0] = (R[5] - R[7]) / q, r[1] = (R[6] - R[2]) / q, r[2] = (R[1] - R[3]) / q;
    } else if (R[0] > R[4] && R[0] > R[8]) {
        float q = sqrtf(1 + R[0] - R[4] - R[8]) * 2;
        r[3] = (R[5] - R[7]) / q, r[0] = q / 4, r[1] = (R[3] + R[1]) / q, r[2] = (R[6] + R[2]) / q;
    } else if (R[4] > R[8]) {
        float q = sqrtf(1 + R[4] - R[0] - R[8]) * 2;
        r[3] = (R[6] - R[2]) / q, r[0] = (R[3] + R[1]) / q, r[1] = q / 4, r[2] = (R[7] + R[5]) / q;
    } else {
        float q = sqrtf(1 + R[8] - R[0] - R[4]) * 2;
        r[3] = (R[1] - R[3]) / q, r[0] = (R[6] + R[2]) / q, r[1] = (R[7] + R[5]) / q, r[2] = q / 4;
    }
    quat_norm(r);
}

/* ---- the whole file ---- */

m3_model *m3_load_glb(const uint8_t *data, size_t len, char *why, size_t whylen) {
    if (why && whylen)
        why[0] = 0;
    rd g = {.why = why, .whylen = whylen};
    uint32_t magic, version, total;
    if (len < 20)
        return fail(&g, "not a glTF binary (.glb) file");
    memcpy(&magic, data, 4), memcpy(&version, data + 4, 4), memcpy(&total, data + 8, 4);
    if (magic != 0x46546c67u)
        return fail(&g, "not a glTF binary (.glb) file");
    if (version != 2)
        return fail(&g, "a glTF 1.0 file; export it again as glTF 2.0");
    if (total > len)
        return fail(&g, "the file is cut short");
    const char *jsontext = NULL;
    size_t jsonlen = 0;
    for (size_t o = 12; o + 8 <= total;) {
        uint32_t cl, ct;
        memcpy(&cl, data + o, 4), memcpy(&ct, data + o + 4, 4);
        if (o + 8 + (size_t)cl > total)
            return fail(&g, "the file is cut short");
        if (ct == 0x4e4f534au && !jsontext)
            jsontext = (const char *)data + o + 8, jsonlen = cl;
        else if (ct == 0x004e4942u && !g.bin)
            g.bin = data + o + 8, g.binlen = cl;
        o += 8 + (size_t)((cl + 3) & ~3u);
    }
    if (!jsontext)
        return fail(&g, "the file has no description of its contents");
    g.root = json_parse(jsontext, jsonlen);
    if (!g.root)
        return fail(&g, "its description can't be read");
    json *bufs = json_get(g.root, "buffers");
    if (json_len(bufs) > 1 || (json_at(bufs, 0) && json_get(json_at(bufs, 0), "uri"))) {
        json_free(g.root);
        return fail(&g, "it keeps its data in other files; export it as a single .glb");
    }
    m3_model *m = calloc(1, sizeof *m);
    if (!m) {
        json_free(g.root);
        return NULL;
    }

    /* pictures */
    json *imgs = json_get(g.root, "images");
    m->ntexs = json_len(imgs);
    m->texs = calloc((size_t)(m->ntexs ? m->ntexs : 1), sizeof *m->texs);
    for (int i = 0; i < m->ntexs; i++)
        load_image(&g, json_at(imgs, i), &m->texs[i]);

    /* materials */
    json *mats = json_get(g.root, "materials");
    m->nmats = json_len(mats);
    m->mats = calloc((size_t)(m->nmats ? m->nmats : 1), sizeof *m->mats);
    for (int i = 0; i < m->nmats; i++) {
        load_material(&g, json_at(mats, i), &m->mats[i]);
        int *refs[] = {&m->mats[i].base_tex, &m->mats[i].mr_tex, &m->mats[i].normal_tex, &m->mats[i].occ_tex,
                       &m->mats[i].emissive_tex};
        for (int k = 0; k < 5; k++)
            if (*refs[k] >= m->ntexs || (*refs[k] >= 0 && !m->texs[*refs[k]].mip[0]))
                *refs[k] = -1;
    }

    /* meshes */
    json *meshes = json_get(g.root, "meshes");
    m->nmeshes = json_len(meshes);
    m->meshes = calloc((size_t)(m->nmeshes ? m->nmeshes : 1), sizeof *m->meshes);
    long long tris = 0;
    for (int i = 0; i < m->nmeshes; i++) {
        json *mj = json_at(meshes, i), *prims = json_get(mj, "primitives");
        m3_mesh *me = &m->meshes[i];
        snprintf(me->name, sizeof me->name, "%s", json_str(json_get(mj, "name"), ""));
        int np = json_len(prims);
        me->prims = calloc((size_t)(np ? np : 1), sizeof *me->prims);
        for (int k = 0; k < np; k++)
            if (load_prim(&g, json_at(prims, k), &me->prims[me->nprims], m->mats, m->nmats))
                tris += me->prims[me->nprims++].nidx / 3;
    }
    if (tris > 3000000) {
        m3_free(m);
        json_free(g.root);
        return fail(&g, "too detailed to draw as part of the interface (over 3 million triangles); decimate it");
    }

    /* cameras and lights */
    json *cams = json_get(g.root, "cameras");
    m->ncams = json_len(cams);
    m->cams = calloc((size_t)(m->ncams ? m->ncams : 1), sizeof *m->cams);
    for (int i = 0; i < m->ncams; i++) {
        json *c = json_at(cams, i);
        m3_camera *cm = &m->cams[i];
        cm->ortho = !strcmp(json_str(json_get(c, "type"), ""), "orthographic");
        json *p = json_get(c, cm->ortho ? "orthographic" : "perspective");
        cm->yfov = (float)json_num(json_get(p, "yfov"), 0.6);
        cm->aspect = (float)json_num(json_get(p, "aspectRatio"), 0);
        cm->xmag = (float)json_num(json_get(p, "xmag"), 1), cm->ymag = (float)json_num(json_get(p, "ymag"), 1);
        cm->znear = (float)json_num(json_get(p, "znear"), 0.05), cm->zfar = (float)json_num(json_get(p, "zfar"), 1000);
    }
    json *lights = json_path(g.root, "extensions.KHR_lights_punctual.lights");
    m->nlights = json_len(lights);
    m->lights = calloc((size_t)(m->nlights ? m->nlights : 1), sizeof *m->lights);
    for (int i = 0; i < m->nlights; i++) {
        json *l = json_at(lights, i);
        m3_light *ml = &m->lights[i];
        const char *t = json_str(json_get(l, "type"), "point");
        ml->type = !strcmp(t, "directional") ? 0 : !strcmp(t, "spot") ? 2 : 1;
        json *c = json_get(l, "color");
        for (int k = 0; k < 3; k++)
            ml->color[k] = c ? (float)json_num(json_at(c, k), 1) : 1;
        ml->intensity = (float)json_num(json_get(l, "intensity"), 1);
        ml->range = (float)json_num(json_get(l, "range"), 0);
        ml->inner = (float)json_num(json_path(l, "spot.innerConeAngle"), 0);
        ml->outer = (float)json_num(json_path(l, "spot.outerConeAngle"), 0.785);
    }

    /* nodes */
    json *nodes = json_get(g.root, "nodes");
    m->nnodes = json_len(nodes);
    if (m->nnodes > 65000) {
        m3_free(m);
        json_free(g.root);
        return fail(&g, "too many objects (over 65 000)");
    }
    m->nodes = calloc((size_t)(m->nnodes ? m->nnodes : 1), sizeof *m->nodes);
    for (int i = 0; i < m->nnodes; i++) {
        m3_node *nd = &m->nodes[i];
        nd->parent = -1, nd->part = -1;
        nd->s[0] = nd->s[1] = nd->s[2] = 1, nd->r[3] = 1;
    }
    for (int i = 0; i < m->nnodes; i++) {
        json *nj = json_at(nodes, i);
        m3_node *nd = &m->nodes[i];
        snprintf(nd->name, sizeof nd->name, "%s", json_str(json_get(nj, "name"), ""));
        nd->mesh = (int)json_num(json_get(nj, "mesh"), -1);
        if (nd->mesh >= m->nmeshes)
            nd->mesh = -1;
        nd->camera = (int)json_num(json_get(nj, "camera"), -1);
        if (nd->camera >= m->ncams)
            nd->camera = -1;
        nd->light = (int)json_num(json_path(nj, "extensions.KHR_lights_punctual.light"), -1);
        if (nd->light >= m->nlights)
            nd->light = -1;
        json *mx = json_get(nj, "matrix");
        if (json_len(mx) == 16) {
            float mm[16];
            for (int k = 0; k < 16; k++)
                mm[k] = (float)json_num(json_at(mx, k), 0);
            decompose(mm, nd->t, nd->r, nd->s);
        } else {
            json *t = json_get(nj, "translation"), *r = json_get(nj, "rotation"), *s = json_get(nj, "scale");
            for (int k = 0; k < 3; k++) {
                if (t) nd->t[k] = (float)json_num(json_at(t, k), 0);
                if (s) nd->s[k] = (float)json_num(json_at(s, k), 1);
            }
            for (int k = 0; r && k < 4; k++)
                nd->r[k] = (float)json_num(json_at(r, k), k == 3);
            quat_norm(nd->r);
        }
        take_extras(nd, json_get(nj, "extras"));
        if (nd->mesh >= 0 && !nd->action[0]) /* custom properties of the mesh data count too */
            take_extras(nd, json_get(json_at(meshes, nd->mesh), "extras"));
        name_action(nd);
        json *kids = json_get(nj, "children");
        for (int k = 0; k < json_len(kids); k++) {
            int c = (int)json_num(json_at(kids, k), -1);
            if (c >= 0 && c < m->nnodes && c != i && m->nodes[c].parent < 0)
                m->nodes[c].parent = i;
        }
    }
    /* a parent loop in a broken file: cut */
    for (int i = 0; i < m->nnodes; i++) {
        int p = m->nodes[i].parent, steps = 0;
        while (p >= 0 && steps++ <= m->nnodes)
            p = m->nodes[p].parent;
        if (p >= 0)
            m->nodes[i].parent = -1;
    }
    /* the scene's roots (the default scene, else every node without a parent) */
    json *scenes = json_get(g.root, "scenes");
    json *sc = json_at(scenes, (int)json_num(json_get(g.root, "scene"), 0));
    json *sn = json_get(sc, "nodes");
    snprintf(m->title, sizeof m->title, "%s", json_str(json_get(sc, "name"), ""));
    m->roots = malloc(sizeof(int) * (size_t)(m->nnodes ? m->nnodes : 1));
    if (sn) {
        for (int k = 0; k < json_len(sn); k++) {
            int r = (int)json_num(json_at(sn, k), -1);
            if (r >= 0 && r < m->nnodes && m->nodes[r].parent < 0)
                m->roots[m->nroots++] = r;
        }
    } else {
        for (int i = 0; i < m->nnodes; i++)
            if (m->nodes[i].parent < 0)
                m->roots[m->nroots++] = i;
    }
    /* clickable parts: a node that does something, and everything under it */
    for (int i = 0; i < m->nnodes; i++) {
        for (int p = i; p >= 0; p = m->nodes[p].parent)
            if (m->nodes[p].action[0] || m->nodes[p].hover[0]) {
                m->nodes[i].part = p;
                break;
            }
        if (m->nodes[i].part == i)
            m->nparts++;
    }

    /* animations */
    json *anims = json_get(g.root, "animations");
    m->nanims = json_len(anims);
    m->anims = calloc((size_t)(m->nanims ? m->nanims : 1), sizeof *m->anims);
    for (int i = 0; i < m->nanims; i++) {
        json *aj = json_at(anims, i), *chs = json_get(aj, "channels"), *sms = json_get(aj, "samplers");
        m3_anim *an = &m->anims[i];
        snprintf(an->name, sizeof an->name, "%s", json_str(json_get(aj, "name"), ""));
        int nc = json_len(chs);
        an->ch = calloc((size_t)(nc ? nc : 1), sizeof *an->ch);
        for (int k = 0; k < nc; k++) {
            json *cj = json_at(chs, k), *tg = json_get(cj, "target");
            int node = (int)json_num(json_get(tg, "node"), -1);
            const char *path = json_str(json_get(tg, "path"), "");
            int pt = !strcmp(path, "translation") ? M3_T : !strcmp(path, "rotation") ? M3_R : !strcmp(path, "scale") ? M3_S : -1;
            json *sj = json_at(sms, (int)json_num(json_get(cj, "sampler"), -1));
            if (node < 0 || node >= m->nnodes || pt < 0 || !sj)
                continue;
            m3_channel *c = &an->ch[an->nch];
            c->node = node, c->path = pt, c->comps = pt == M3_R ? 4 : 3;
            const char *ip = json_str(json_get(sj, "interpolation"), "LINEAR");
            c->interp = !strcmp(ip, "STEP") ? 1 : !strcmp(ip, "CUBICSPLINE") ? 2 : 0;
            int ni = 0, no = 0;
            c->in = read_floats(&g, (int)json_num(json_get(sj, "input"), -1), 1, &ni);
            c->out = read_floats(&g, (int)json_num(json_get(sj, "output"), -1), c->comps, &no);
            if (!c->in || !c->out || no != ni * (c->interp == 2 ? 3 : 1) || ni < 1) {
                free(c->in), free(c->out);
                continue;
            }
            c->n = ni;
            if (c->in[ni - 1] > an->duration)
                an->duration = c->in[ni - 1];
            an->nch++;
        }
    }
    json_free(g.root);
    m3_pose(m, NULL, NULL);
    return m;
}

m3_model *m3_load_file(const char *path, char *why, size_t whylen) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(why, whylen, "there is no file %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 512L * 1024 * 1024) {
        fclose(f);
        snprintf(why, whylen, n > 0 ? "the file is over 512 MB" : "the file is empty");
        return NULL;
    }
    uint8_t *d = malloc((size_t)n);
    size_t got = d ? fread(d, 1, (size_t)n, f) : 0;
    fclose(f);
    m3_model *m = got == (size_t)n ? m3_load_glb(d, (size_t)n, why, whylen) : NULL;
    if (!m && got != (size_t)n)
        snprintf(why, whylen, "the file couldn't be read");
    free(d);
    return m;
}

void m3_free(m3_model *m) {
    if (!m)
        return;
    for (int i = 0; i < m->ntexs; i++)
        for (int l = 0; l < 12; l++)
            free(m->texs[i].mip[l]);
    for (int i = 0; i < m->nmeshes; i++) {
        for (int k = 0; k < m->meshes[i].nprims; k++) {
            m3_prim *p = &m->meshes[i].prims[k];
            free(p->pos), free(p->nrm), free(p->uv), free(p->tan), free(p->col), free(p->idx);
        }
        free(m->meshes[i].prims);
    }
    for (int i = 0; i < m->nanims; i++) {
        for (int k = 0; k < m->anims[i].nch; k++)
            free(m->anims[i].ch[k].in), free(m->anims[i].ch[k].out);
        free(m->anims[i].ch);
    }
    free(m->texs), free(m->mats), free(m->meshes), free(m->nodes), free(m->roots), free(m->anims), free(m->cams);
    free(m->lights);
    free(m);
}

int m3_anim_find(const m3_model *m, const char *name) {
    if (!name || !*name)
        return -1;
    for (int i = 0; i < m->nanims; i++)
        if (!strcasecmp(m->anims[i].name, name))
            return i;
    char *end;
    long k = strtol(name, &end, 10);
    return !*end && k >= 0 && k < m->nanims ? (int)k : -1;
}

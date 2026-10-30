/* Animation clips: reading and writing the text form, laying a clip over a
 * pose, and the gestures the OS makes itself from key poses (placeholders
 * until recordings made with FreeMoCap and our Blender script replace
 * them: a recorded clip with the same name wins). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clip.h"

void clip_free(clip *c) {
    free(c->rot), free(c->root);
    memset(c, 0, sizeof *c);
}

static const char *next_line(const char *s, char *line, size_t n) {
    if (!*s)
        return NULL;
    size_t i = 0;
    while (*s && *s != '\n') {
        if (i + 1 < n)
            line[i++] = *s;
        s++;
    }
    line[i] = 0;
    return *s ? s + 1 : s;
}

bool clip_parse(clip *c, const char *text) {
    memset(c, 0, sizeof *c);
    c->fps = 30;
    char line[8192];
    const char *s = text;
    bool head = false, has_root = false;
    int f = 0;
    while ((s = next_line(s, line, sizeof line))) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p || *p == '#')
            continue;
        if (!head) {
            if (strncmp(p, "osclip 1", 8))
                return false;
            head = true;
            continue;
        }
        if (!strncmp(p, "name ", 5))
            snprintf(c->name, sizeof c->name, "%s", p + 5);
        else if (!strncmp(p, "fps ", 4))
            c->fps = (float)atof(p + 4);
        else if (!strncmp(p, "frames ", 7))
            c->frames = atoi(p + 7);
        else if (!strncmp(p, "loop ", 5))
            c->loop = atoi(p + 5) != 0;
        else if (!strncmp(p, "root ", 5))
            has_root = atoi(p + 5) != 0;
        else if (!strncmp(p, "bones ", 6)) {
            char *tok = strtok(p + 6, " \t");
            while (tok && c->nb < CLIP_MAX_BONES) {
                snprintf(c->bone[c->nb++], 24, "%s", tok);
                tok = strtok(NULL, " \t");
            }
        } else if (!strncmp(p, "contact ", 8)) {
            if (c->ncontact < CLIP_MAX_CONTACTS) {
                clip_contact *k = &c->contact[c->ncontact];
                if (sscanf(p + 8, "%d %d %23s %11s", &k->f0, &k->f1, k->bone, k->kind) == 4)
                    k->b = -1, c->ncontact++;
            }
        } else if (p[0] == 'f' && p[1] == ' ') {
            if (c->frames <= 0 || c->nb <= 0 || c->fps <= 0 || c->frames > 100000)
                return false;
            if (!c->rot) {
                c->rot = calloc((size_t)c->frames * c->nb, sizeof *c->rot);
                if (has_root)
                    c->root = calloc((size_t)c->frames, sizeof *c->root);
                if (!c->rot || (has_root && !c->root))
                    return false;
            }
            if (f >= c->frames)
                continue;
            char *q = p + 2;
            for (int b = 0; b < c->nb; b++) {
                quat r;
                r.x = strtof(q, &q), r.y = strtof(q, &q), r.z = strtof(q, &q), r.w = strtof(q, &q);
                c->rot[(size_t)f * c->nb + b] = qnorm(r);
            }
            if (c->root) {
                v3 o;
                o.x = strtof(q, &q), o.y = strtof(q, &q), o.z = strtof(q, &q);
                c->root[f] = o;
            }
            f++;
        }
    }
    if (!c->rot || f < c->frames) {
        clip_free(c);
        return false;
    }
    for (int b = 0; b < CLIP_MAX_BONES; b++)
        c->map[b] = -1;
    return true;
}

char *clip_format(const clip *c) {
    size_t cap = 512 + (size_t)c->frames * (c->nb * 4 * 12 + 48), n = 0;
    char *out = malloc(cap);
    if (!out)
        return NULL;
#define PUT(...) n += (size_t)snprintf(out + n, cap - n, __VA_ARGS__)
    PUT("osclip 1\nname %s\nfps %g\nframes %d\nloop %d\nroot %d\nbones", c->name, c->fps, c->frames, c->loop ? 1 : 0,
        c->root ? 1 : 0);
    for (int b = 0; b < c->nb; b++)
        PUT(" %s", c->bone[b]);
    PUT("\n");
    for (int k = 0; k < c->ncontact; k++)
        PUT("contact %d %d %s %s\n", c->contact[k].f0, c->contact[k].f1, c->contact[k].bone, c->contact[k].kind);
    for (int f = 0; f < c->frames && n + c->nb * 48 + 64 < cap; f++) {
        PUT("f");
        for (int b = 0; b < c->nb; b++) {
            quat r = c->rot[(size_t)f * c->nb + b];
            PUT(" %.5f %.5f %.5f %.5f", r.x, r.y, r.z, r.w);
        }
        if (c->root)
            PUT(" %.5f %.5f %.5f", c->root[f].x, c->root[f].y, c->root[f].z);
        PUT("\n");
    }
#undef PUT
    return out;
}

void clip_bind(clip *c, const fmodel *m) {
    for (int b = 0; b < c->nb; b++)
        c->map[b] = fig_find_bone(m, c->bone[b]);
    for (int k = 0; k < c->ncontact; k++)
        c->contact[k].b = fig_find_bone(m, c->contact[k].bone);
}

float clip_length(const clip *c) { return c->fps > 0 ? (c->frames - 1) / c->fps : 0; }

static void frame_at(const clip *c, float t, int *f0, int *f1, float *a) {
    float x = t * c->fps;
    if (c->loop && c->frames > 1) {
        float len = (float)(c->frames - 1);
        x -= len * (float)ifloor(x / len);
    }
    x = clampf(x, 0, (float)(c->frames - 1));
    *f0 = (int)x;
    *f1 = *f0 + 1 < c->frames ? *f0 + 1 : *f0;
    *a = x - *f0;
}

void clip_apply(const clip *c, float t, fpose *p, float w) {
    if (!c->rot || w <= 0)
        return;
    int f0, f1;
    float a;
    frame_at(c, t, &f0, &f1, &a);
    for (int b = 0; b < c->nb; b++) {
        int mb = c->map[b];
        if (mb < 0)
            continue;
        quat q = qslerp(c->rot[(size_t)f0 * c->nb + b], c->rot[(size_t)f1 * c->nb + b], a);
        p->local[mb] = w >= 1 ? q : qslerp(p->local[mb], q, w);
    }
}

v3 clip_root(const clip *c, float t) {
    if (!c->root)
        return V3(0, 0, 0);
    int f0, f1;
    float a;
    frame_at(c, t, &f0, &f1, &a);
    return vlerp(c->root[f0], c->root[f1], a);
}

bool clip_from_keys(clip *c, const char *name, float fps, bool loop, int nb, const char *const *bones, const clip_key *keys,
                    int nkeys) {
    memset(c, 0, sizeof *c);
    if (nb > CLIP_MAX_BONES || nkeys < 1)
        return false;
    snprintf(c->name, sizeof c->name, "%s", name);
    c->fps = fps, c->loop = loop, c->nb = nb;
    for (int b = 0; b < nb; b++) {
        snprintf(c->bone[b], 24, "%s", bones[b]);
        c->map[b] = -1;
    }
    float len = keys[nkeys - 1].t;
    c->frames = (int)(len * fps + 0.5f) + 1;
    c->rot = calloc((size_t)c->frames * nb, sizeof *c->rot);
    if (!c->rot)
        return false;
    for (int f = 0; f < c->frames; f++) {
        float t = f / fps;
        int k = 0;
        while (k < nkeys - 2 && t > keys[k + 1].t)
            k++;
        const clip_key *k0 = &keys[k], *k1 = &keys[nkeys > 1 ? k + 1 : k];
        float a = k1->t > k0->t ? smoothf(k0->t, k1->t, t) : 0;
        for (int b = 0; b < nb; b++) {
            v3 e0 = k0->euler[b], e1 = k1->euler[b];
            c->rot[(size_t)f * nb + b] = qslerp(qeuler(e0.x, e0.y, e0.z), qeuler(e1.x, e1.y, e1.z), a);
        }
    }
    return true;
}

/* ---- the OS's own gestures ---- */

const char *const clip_builtin_names[] = {"wave", "nod", "shake", "bow", "clap", "point", "shrug", "think", "laugh",
                                          "cheer", "yawn", "stretch", "tilt", NULL};

#define E V3
/* the arms as they hang when relaxed (see body_pose_relaxed), as eulers */
#define R_UA_DOWN E(0, 0.12f, 1.2f)
#define L_UA_DOWN E(0, -0.12f, -1.2f)
#define R_LA_REST E(0, 0.28f, 0)
#define L_LA_REST E(0, -0.28f, 0)

bool clip_builtin(clip *c, const char *name) {
    if (!strcmp(name, "wave")) {
        static const char *const b[] = {"rightUpperArm", "rightLowerArm", "rightHand", "head"};
        clip_key k[] = {
            {0.0f, {R_UA_DOWN, R_LA_REST, E(0, 0, 0), E(0, 0, 0)}},
            {0.35f, {E(0, 0.35f, -0.45f), E(0, 0.2f, -1.35f), E(0, 0, 0), E(0, 0, 0.08f)}},
            {0.55f, {E(0, 0.35f, -0.45f), E(0, 0.2f, -1.05f), E(0, 0, 0.15f), E(0, 0, 0.08f)}},
            {0.75f, {E(0, 0.35f, -0.45f), E(0, 0.2f, -1.6f), E(0, 0, -0.15f), E(0, 0, 0.08f)}},
            {0.95f, {E(0, 0.35f, -0.45f), E(0, 0.2f, -1.05f), E(0, 0, 0.15f), E(0, 0, 0.08f)}},
            {1.15f, {E(0, 0.35f, -0.45f), E(0, 0.2f, -1.6f), E(0, 0, -0.15f), E(0, 0, 0.08f)}},
            {1.35f, {E(0, 0.35f, -0.45f), E(0, 0.2f, -1.3f), E(0, 0, 0), E(0, 0, 0.05f)}},
            {1.75f, {R_UA_DOWN, R_LA_REST, E(0, 0, 0), E(0, 0, 0)}},
        };
        return clip_from_keys(c, name, 30, false, 4, b, k, 8);
    }
    if (!strcmp(name, "nod")) {
        static const char *const b[] = {"neck", "head"};
        clip_key k[] = {{0, {E(0, 0, 0), E(0, 0, 0)}},          {0.18f, {E(0.08f, 0, 0), E(0.22f, 0, 0)}},
                        {0.36f, {E(-0.02f, 0, 0), E(-0.04f, 0, 0)}}, {0.54f, {E(0.07f, 0, 0), E(0.18f, 0, 0)}},
                        {0.8f, {E(0, 0, 0), E(0, 0, 0)}}};
        return clip_from_keys(c, name, 30, false, 2, b, k, 5);
    }
    if (!strcmp(name, "shake")) {
        static const char *const b[] = {"neck", "head"};
        clip_key k[] = {{0, {E(0, 0, 0), E(0, 0, 0)}},       {0.16f, {E(0, 0.1f, 0), E(0, 0.25f, 0)}},
                        {0.36f, {E(0, -0.1f, 0), E(0, -0.25f, 0)}}, {0.56f, {E(0, 0.08f, 0), E(0, 0.2f, 0)}},
                        {0.74f, {E(0, -0.05f, 0), E(0, -0.12f, 0)}}, {0.95f, {E(0, 0, 0), E(0, 0, 0)}}};
        return clip_from_keys(c, name, 30, false, 2, b, k, 6);
    }
    if (!strcmp(name, "bow")) {
        static const char *const b[] = {"spine", "chest", "neck", "head", "rightUpperArm", "leftUpperArm"};
        clip_key k[] = {{0, {E(0, 0, 0), E(0, 0, 0), E(0, 0, 0), E(0, 0, 0), R_UA_DOWN, L_UA_DOWN}},
                        {0.45f, {E(0.32f, 0, 0), E(0.22f, 0, 0), E(0.12f, 0, 0), E(0.15f, 0, 0), E(0.15f, 0.3f, 1.25f), E(0.15f, -0.3f, -1.25f)}},
                        {0.95f, {E(0.32f, 0, 0), E(0.22f, 0, 0), E(0.12f, 0, 0), E(0.15f, 0, 0), E(0.15f, 0.3f, 1.25f), E(0.15f, -0.3f, -1.25f)}},
                        {1.5f, {E(0, 0, 0), E(0, 0, 0), E(0, 0, 0), E(0, 0, 0), R_UA_DOWN, L_UA_DOWN}}};
        return clip_from_keys(c, name, 30, false, 6, b, k, 4);
    }
    if (!strcmp(name, "clap")) {
        static const char *const b[] = {"rightUpperArm", "rightLowerArm", "leftUpperArm", "leftLowerArm", "rightHand", "leftHand"};
        v3 rua = E(0, 0.75f, 0.85f), lua = E(0, -0.75f, -0.85f);
        clip_key k[] = {
            {0, {R_UA_DOWN, R_LA_REST, L_UA_DOWN, L_LA_REST, E(0, 0, 0), E(0, 0, 0)}},
            {0.3f, {rua, E(0, 1.05f, 0), lua, E(0, -1.05f, 0), E(-0.5f, 0, 0), E(-0.5f, 0, 0)}},
            {0.45f, {rua, E(0, 1.3f, 0), lua, E(0, -1.3f, 0), E(-0.5f, 0, 0), E(-0.5f, 0, 0)}},
            {0.6f, {rua, E(0, 1.05f, 0), lua, E(0, -1.05f, 0), E(-0.5f, 0, 0), E(-0.5f, 0, 0)}},
            {0.75f, {rua, E(0, 1.3f, 0), lua, E(0, -1.3f, 0), E(-0.5f, 0, 0), E(-0.5f, 0, 0)}},
            {0.9f, {rua, E(0, 1.05f, 0), lua, E(0, -1.05f, 0), E(-0.5f, 0, 0), E(-0.5f, 0, 0)}},
            {1.05f, {rua, E(0, 1.3f, 0), lua, E(0, -1.3f, 0), E(-0.5f, 0, 0), E(-0.5f, 0, 0)}},
            {1.45f, {R_UA_DOWN, R_LA_REST, L_UA_DOWN, L_LA_REST, E(0, 0, 0), E(0, 0, 0)}},
        };
        return clip_from_keys(c, name, 30, false, 6, b, k, 8);
    }
    if (!strcmp(name, "point")) {
        /* her right arm out towards the picture's left, index finger out */
        static const char *const b[] = {"rightUpperArm", "rightLowerArm", "rightHand", "rightMiddleProximal",
                                        "rightRingProximal", "rightLittleProximal", "rightIndexProximal", "head"};
        v3 curl = E(0, 0, 1.3f);
        clip_key k[] = {{0, {R_UA_DOWN, R_LA_REST, E(0, 0, 0), E(0, 0, 0.2f), E(0, 0, 0.25f), E(0, 0, 0.3f), E(0, 0, 0.18f), E(0, 0, 0)}},
                        {0.4f, {E(0, 0.55f, 0.05f), E(0, 0.1f, 0), E(0, 0, 0), curl, curl, curl, E(0, 0, 0), E(0, -0.2f, 0)}},
                        {1.3f, {E(0, 0.55f, 0.05f), E(0, 0.1f, 0), E(0, 0, 0), curl, curl, curl, E(0, 0, 0), E(0, -0.2f, 0)}},
                        {1.75f, {R_UA_DOWN, R_LA_REST, E(0, 0, 0), E(0, 0, 0.2f), E(0, 0, 0.25f), E(0, 0, 0.3f), E(0, 0, 0.18f), E(0, 0, 0)}}};
        return clip_from_keys(c, name, 30, false, 8, b, k, 4);
    }
    if (!strcmp(name, "shrug")) {
        static const char *const b[] = {"rightShoulder", "leftShoulder", "rightUpperArm", "leftUpperArm", "rightLowerArm",
                                        "leftLowerArm", "head", "rightHand", "leftHand"};
        clip_key k[] = {{0, {E(0, 0, 0.06f), E(0, 0, -0.06f), R_UA_DOWN, L_UA_DOWN, R_LA_REST, L_LA_REST, E(0, 0, 0), E(0, 0, 0), E(0, 0, 0)}},
                        {0.35f, {E(0, 0, -0.2f), E(0, 0, 0.2f), E(0, 0.2f, 1.05f), E(0, -0.2f, -1.05f), E(0, 1.2f, 0.3f), E(0, -1.2f, -0.3f),
                                 E(0, 0, 0.18f), E(-1.2f, 0, 0), E(-1.2f, 0, 0)}},
                        {0.85f, {E(0, 0, -0.2f), E(0, 0, 0.2f), E(0, 0.2f, 1.05f), E(0, -0.2f, -1.05f), E(0, 1.2f, 0.3f), E(0, -1.2f, -0.3f),
                                 E(0, 0, 0.18f), E(-1.2f, 0, 0), E(-1.2f, 0, 0)}},
                        {1.3f, {E(0, 0, 0.06f), E(0, 0, -0.06f), R_UA_DOWN, L_UA_DOWN, R_LA_REST, L_LA_REST, E(0, 0, 0), E(0, 0, 0), E(0, 0, 0)}}};
        return clip_from_keys(c, name, 30, false, 9, b, k, 4);
    }
    if (!strcmp(name, "think")) {
        /* a hand to her chin, eyes up; held */
        static const char *const b[] = {"rightUpperArm", "rightLowerArm", "rightHand", "head", "neck"};
        clip_key k[] = {{0, {R_UA_DOWN, R_LA_REST, E(0, 0, 0), E(0, 0, 0), E(0, 0, 0)}},
                        {0.5f, {E(0, 0.6f, 1.05f), E(0, 2.2f, 0), E(0, 0, -0.5f), E(-0.1f, 0.1f, 0.12f), E(0, 0, 0.04f)}},
                        {2.5f, {E(0, 0.6f, 1.05f), E(0, 2.25f, 0), E(0, 0, -0.5f), E(-0.12f, 0.12f, 0.14f), E(0, 0, 0.04f)}}};
        return clip_from_keys(c, name, 30, false, 5, b, k, 3);
    }
    if (!strcmp(name, "laugh")) {
        static const char *const b[] = {"chest", "upperChest", "head", "rightShoulder", "leftShoulder"};
        clip_key k[] = {{0, {E(0, 0, 0), E(0, 0, 0), E(0, 0, 0), E(0, 0, 0.06f), E(0, 0, -0.06f)}},
                        {0.15f, {E(0.05f, 0, 0), E(0.04f, 0, 0), E(-0.12f, 0, 0.05f), E(0, 0, -0.04f), E(0, 0, 0.04f)}},
                        {0.3f, {E(-0.02f, 0, 0), E(0, 0, 0), E(-0.18f, 0, 0.06f), E(0, 0, 0.04f), E(0, 0, -0.04f)}},
                        {0.45f, {E(0.05f, 0, 0), E(0.04f, 0, 0), E(-0.12f, 0, 0.05f), E(0, 0, -0.04f), E(0, 0, 0.04f)}},
                        {0.6f, {E(-0.02f, 0, 0), E(0, 0, 0), E(-0.18f, 0, 0.06f), E(0, 0, 0.04f), E(0, 0, -0.04f)}},
                        {0.75f, {E(0.04f, 0, 0), E(0.03f, 0, 0), E(-0.1f, 0, 0.04f), E(0, 0, -0.02f), E(0, 0, 0.02f)}},
                        {1.2f, {E(0, 0, 0), E(0, 0, 0), E(0, 0, 0), E(0, 0, 0.06f), E(0, 0, -0.06f)}}};
        return clip_from_keys(c, name, 30, false, 5, b, k, 7);
    }
    if (!strcmp(name, "cheer")) {
        static const char *const b[] = {"rightUpperArm", "leftUpperArm", "rightLowerArm", "leftLowerArm", "head", "chest"};
        clip_key k[] = {{0, {R_UA_DOWN, L_UA_DOWN, R_LA_REST, L_LA_REST, E(0, 0, 0), E(0, 0, 0)}},
                        {0.3f, {E(0, 0.3f, -1.1f), E(0, -0.3f, 1.1f), E(0, 0.4f, -0.3f), E(0, -0.4f, 0.3f), E(-0.15f, 0, 0), E(-0.05f, 0, 0)}},
                        {0.5f, {E(0, 0.3f, -0.95f), E(0, -0.3f, 0.95f), E(0, 0.4f, -0.3f), E(0, -0.4f, 0.3f), E(-0.1f, 0, 0), E(0, 0, 0)}},
                        {0.7f, {E(0, 0.3f, -1.15f), E(0, -0.3f, 1.15f), E(0, 0.4f, -0.3f), E(0, -0.4f, 0.3f), E(-0.15f, 0, 0), E(-0.05f, 0, 0)}},
                        {1.25f, {R_UA_DOWN, L_UA_DOWN, R_LA_REST, L_LA_REST, E(0, 0, 0), E(0, 0, 0)}}};
        return clip_from_keys(c, name, 30, false, 6, b, k, 5);
    }
    if (!strcmp(name, "yawn")) {
        static const char *const b[] = {"rightUpperArm", "rightLowerArm", "rightHand", "head", "neck", "upperChest"};
        clip_key k[] = {{0, {R_UA_DOWN, R_LA_REST, E(0, 0, 0), E(0, 0, 0), E(0, 0, 0), E(0, 0, 0)}},
                        {0.6f, {E(0, 0.65f, 1.0f), E(0, 2.35f, 0), E(0, 0, -0.3f), E(-0.22f, 0, 0.06f), E(-0.05f, 0, 0), E(-0.06f, 0, 0)}},
                        {1.8f, {E(0, 0.65f, 1.0f), E(0, 2.35f, 0), E(0, 0, -0.3f), E(-0.25f, 0, 0.08f), E(-0.06f, 0, 0), E(-0.08f, 0, 0)}},
                        {2.5f, {R_UA_DOWN, R_LA_REST, E(0, 0, 0), E(0, 0, 0), E(0, 0, 0), E(0, 0, 0)}}};
        return clip_from_keys(c, name, 30, false, 6, b, k, 4);
    }
    if (!strcmp(name, "stretch")) {
        static const char *const b[] = {"rightUpperArm", "leftUpperArm", "rightLowerArm", "leftLowerArm", "spine", "chest", "head"};
        clip_key k[] = {{0, {R_UA_DOWN, L_UA_DOWN, R_LA_REST, L_LA_REST, E(0, 0, 0), E(0, 0, 0), E(0, 0, 0)}},
                        {0.7f, {E(0, 0.1f, -1.4f), E(0, -0.1f, 1.4f), E(0, 0, -0.1f), E(0, 0, 0.1f), E(-0.06f, 0, 0.08f), E(-0.06f, 0, 0.05f),
                                E(-0.15f, 0, 0.1f)}},
                        {1.6f, {E(0, 0.1f, -1.45f), E(0, -0.1f, 1.45f), E(0, 0, -0.1f), E(0, 0, 0.1f), E(-0.06f, 0, -0.08f),
                                E(-0.06f, 0, -0.05f), E(-0.15f, 0, -0.1f)}},
                        {2.4f, {R_UA_DOWN, L_UA_DOWN, R_LA_REST, L_LA_REST, E(0, 0, 0), E(0, 0, 0), E(0, 0, 0)}}};
        return clip_from_keys(c, name, 30, false, 7, b, k, 4);
    }
    if (!strcmp(name, "tilt")) {
        static const char *const b[] = {"neck", "head"};
        clip_key k[] = {{0, {E(0, 0, 0), E(0, 0, 0)}}, {0.4f, {E(0, 0, 0.08f), E(0.04f, 0.08f, 0.22f)}},
                        {1.4f, {E(0, 0, 0.08f), E(0.04f, 0.08f, 0.22f)}}, {1.9f, {E(0, 0, 0), E(0, 0, 0)}}};
        return clip_from_keys(c, name, 30, false, 2, b, k, 4);
    }
    return false;
}
#undef E

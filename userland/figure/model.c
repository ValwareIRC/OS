/* Building a figure's model: bones, materials, meshes, textures. */
#include <stdlib.h>
#include <string.h>

#include "fig.h"

void fig_model_init(fmodel *m) { memset(m, 0, sizeof *m); }

void fig_model_free(fmodel *m) {
    for (int i = 0; i < m->nmeshes; i++) {
        fmesh *me = &m->mesh[i];
        free(me->pos), free(me->nrm), free(me->uv), free(me->aux), free(me->bi), free(me->bw), free(me->tri), free(me->tpart), free(me->sun);
        for (int k = 0; k < me->nmorph; k++)
            free(me->morph[k]);
    }
    for (int i = 0; i < m->ntex; i++)
        free(m->tex[i].px);
    memset(m, 0, sizeof *m);
}

int fig_add_bone(fmodel *m, const char *name, int parent, v3 head, v3 tail) {
    if (m->nbones >= FIG_MAX_BONES)
        return -1;
    fbone *b = &m->bone[m->nbones];
    memset(b, 0, sizeof *b);
    strncpy(b->name, name, sizeof b->name - 1);
    b->parent = parent;
    b->head = head;
    b->tail = tail;
    return m->nbones++;
}

int fig_find_bone(const fmodel *m, const char *name) {
    for (int i = 0; i < m->nbones; i++)
        if (!strcmp(m->bone[i].name, name))
            return i;
    return -1;
}

fmat fig_mat(int kind, rgb base) {
    fmat t = {.kind = kind, .base = base, .shift = 0.0f, .toony = 0.12f, .rim = 0.16f, .tex = -1, .line = true};
    /* a shade colour a little darker, warmer for skin, cooler for the rest */
    if (kind == MAT_SKIN || kind == MAT_FACE)
        t.shade = rgb_mix(rgb_scale(base, 0.86f), 0xd9786a, 0.18f), t.shift = -0.25f, t.toony = 0.22f;
    else if (kind == MAT_HAIR)
        t.shade = rgb_mix(rgb_scale(base, 0.68f), 0x40306a, 0.15f), t.spec = 0.6f;
    else
        t.shade = rgb_mix(rgb_scale(base, 0.72f), 0x3a3550, 0.12f);
    t.line_col = rgb_mix(rgb_scale(base, 0.38f), 0x201820, 0.35f);
    return t;
}

int fig_add_mat(fmodel *m, fmat mt) {
    if (m->nmats >= FIG_MAX_MATS)
        return 0;
    m->mat[m->nmats] = mt;
    return m->nmats++;
}

fmesh *fig_add_mesh(fmodel *m, int nv, int nt, int mat, bool uv, bool aux) {
    if (m->nmeshes >= FIG_MAX_MESHES)
        return NULL;
    fmesh *me = &m->mesh[m->nmeshes++];
    memset(me, 0, sizeof *me);
    me->nv = nv, me->nt = nt, me->mat = mat;
    me->pos = calloc((size_t)nv, sizeof *me->pos);
    me->nrm = calloc((size_t)nv, sizeof *me->nrm);
    me->bi = calloc((size_t)nv, sizeof *me->bi);
    me->bw = calloc((size_t)nv, sizeof *me->bw);
    me->tri = calloc((size_t)nt * 3, sizeof *me->tri);
    if (uv)
        me->uv = calloc((size_t)nv * 2, sizeof *me->uv);
    if (aux)
        me->aux = calloc((size_t)nv, sizeof *me->aux);
    return me;
}

void fig_mesh_normals(fmesh *me) {
    for (int i = 0; i < me->nv; i++)
        me->nrm[i] = V3(0, 0, 0);
    for (int t = 0; t < me->nt; t++) {
        int a = me->tri[t * 3], b = me->tri[t * 3 + 1], c = me->tri[t * 3 + 2];
        v3 n = vcross(vsub(me->pos[b], me->pos[a]), vsub(me->pos[c], me->pos[a]));
        me->nrm[a] = vadd(me->nrm[a], n);
        me->nrm[b] = vadd(me->nrm[b], n);
        me->nrm[c] = vadd(me->nrm[c], n);
    }
    for (int i = 0; i < me->nv; i++)
        me->nrm[i] = vnorm(me->nrm[i]);
}

void fig_mesh_rigid(fmesh *me, int bone) {
    for (int i = 0; i < me->nv; i++) {
        me->bi[i][0] = (uint8_t)bone;
        me->bw[i][0] = 1;
        me->bw[i][1] = me->bw[i][2] = me->bw[i][3] = 0;
    }
}

int fig_add_tex(fmodel *m, int w, int h) {
    if (m->ntex >= FIG_MAX_TEX)
        return -1;
    ftex *t = &m->tex[m->ntex];
    t->w = w, t->h = h;
    t->px = calloc((size_t)w * h, 4);
    return m->ntex++;
}

/* A look made into a figure: the body laid out and meshed, the head with its
 * drawn face, hair, clothes, and the key light's shadows baked in. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "figure.h"

void look_default(look *lk) {
    memset(lk, 0, sizeof *lk);
    snprintf(lk->name, sizeof lk->name, "%s", "her");
    lk->body = body_defaults();
    lk->face = face_defaults();
    lk->hair = hair_defaults();
    lk->outfit = outfit_defaults();
    lk->body.skin = lk->face.skin;
    lk->wall = 0x3a3150, lk->wall2 = 0x2a2440, lk->floor_col = 0x5a4a5e, lk->lamp = 0xffd9a0;
    lk->props = ROOM_LAMP | ROOM_PLANT | ROOM_WINDOW | ROOM_FAIRY;
    lk->energy = 0.6f, lk->shyness = 0.3f;
    lk->walk = WALK_LIGHT;
}

v3 figure_head_center(const figure *f) { return f->head.center; }

bool figure_build(figure *f, const look *lk, float cell) {
    memset(f, 0, sizeof *f);
    f->lk = *lk;
    fmodel *m = &f->m;
    fig_model_init(m);
    body_params bp = lk->body;
    bp.skin = lk->face.skin;
    body_layout(m, &bp, &f->body);
    body_info *bi = &f->body;
    float s = bi->s;
    f->head = head_build(m, &bi->hp, bi->head_center, bi->bone[BN_HEAD], lk->face.skin, 768);
    f->face_tex = f->head.tex;
    if (!body_build_mesh(m, bi, f->head.skin_mat, cell > 0 ? cell : 0.011f * s))
        return false;
    body_build_hands(m, bi, f->head.skin_mat);
    collider col[] = {{V3(0, bi->shoulder_y - 0.03f * s, -0.01f), 0.07f * s},
                      {V3(0.1f * s, bi->shoulder_y - 0.035f * s, -0.01f), 0.07f * s},
                      {V3(-0.1f * s, bi->shoulder_y - 0.035f * s, -0.01f), 0.07f * s},
                      {V3(0, bi->neck_y + 0.02f * s, -0.015f), 0.04f * s},
                      {V3(bi->shoulder_x, bi->shoulder_y - 0.02f * s, 0), 0.06f * s},
                      {V3(-bi->shoulder_x, bi->shoulder_y - 0.02f * s, 0), 0.06f * s}};
    hair_build(m, &f->head, &lk->hair, bi->bone[BN_HEAD], col, 6, &f->hair);
    outfit_build(m, bi, &lk->outfit);
    /* the key light's shadows, in her usual pose */
    fpose rest;
    fig_pose_rest(m, &rest);
    body_pose_relaxed(bi, &rest);
    fig_pose_update(m, &rest);
    fig_bake_sun(m, &rest, vnorm(V3(-0.5f, 0.65f, 0.75f)), 8);
    face_state fs = {0};
    fs.mouth = FM_SMILE;
    face_draw(&m->tex[f->face_tex], &bi->hp, &lk->face, &fs);
    f->drawn = fs;
    return true;
}

void figure_free(figure *f) { fig_model_free(&f->m); }

void figure_face(figure *f, const face_state *fs) {
    if (f->vrm) /* a VRM's face moves by its own shapes and bones (figure_vrm_face) */
        return;
    if (!memcmp(fs, &f->drawn, sizeof *fs))
        return;
    face_draw(&f->m.tex[f->face_tex], &f->body.hp, &f->lk.face, fs);
    f->drawn = *fs;
}

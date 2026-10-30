/* Framing and lighting her. */
#include <string.h>

#include "scene.h"

void scene_defaults(scene *sc) {
    memset(sc, 0, sizeof *sc);
    sc->w = sc->h = 256;
    sc->ss = 2;
    sc->shot = SHOT_BUST;
    sc->key = 0xfff6ee, sc->ambient = 0xc4c8e6, sc->rim = 0x8f9fe8;
}

int scene_shot_from_name(const char *name) {
    if (!strcmp(name, "face"))
        return SHOT_FACE;
    if (!strcmp(name, "waist"))
        return SHOT_WAIST;
    if (!strcmp(name, "full"))
        return SHOT_FULL;
    return SHOT_BUST;
}

void scene_camera(const figure *f, const scene *sc, fcam *cam) {
    const body_info *bi = &f->body;
    float s = bi->s;
    v3 target;
    float view_h;
    switch (sc->shot) {
    case SHOT_FACE:
        target = vadd(f->head.center, V3(0, -0.03f * s, 0));
        view_h = 0.36f * s;
        break;
    case SHOT_WAIST:
        target = V3(0, bi->chest_y + 0.05f * s, 0);
        view_h = 0.95f * s;
        break;
    case SHOT_FULL:
        target = V3(0, bi->H * 0.5f, 0);
        view_h = bi->H * 1.12f;
        break;
    default:
        target = vadd(f->head.center, V3(0, -0.085f * s, 0));
        view_h = 0.5f * s;
        break;
    }
    if (sc->pull > 0.001f && sc->shot != SHOT_FULL && sc->shot != SHOT_WAIST) {
        /* back towards waist-up, so a gesture's hands are in the picture */
        float k = smoothf(0, 1, sc->pull);
        target = vlerp(target, V3(0, bi->chest_y + 0.05f * s, 0), k);
        view_h = lerpf(view_h, 0.95f * s, k);
    }
    float fov = 0.36f;
    float aspect = sc->w > 0 && sc->h > 0 ? (float)sc->w / sc->h : 1;
    if (aspect < 1) /* a tall picture: the height is what matters */
        view_h /= aspect > 0.4f ? 1 : aspect / 0.4f;
    float dist = view_h * 0.5f / tanf(fov * 0.5f);
    v3 eye = vadd(target, V3(sinf(sc->yaw) * cosf(sc->pitch) * dist, sinf(sc->pitch) * dist + 0.02f * s,
                             cosf(sc->yaw) * cosf(sc->pitch) * dist));
    memset(cam, 0, sizeof *cam);
    cam->view = m4lookat(eye, target, V3(0, 1, 0));
    cam->fovy = fov;
    cam->znear = 0.05f;
    cam->zfar = 50;
    cam->w = sc->w, cam->h = sc->h;
    cam->ss = sc->ss;
}

void scene_draw(figure *f, const scene *sc, const fcam *cam, const fpose *pose, fbuf *b, uint32_t *out) {
    flight L = {.dir = vnorm(V3(-0.5f, 0.65f, 0.75f)), .color = sc->key, .ambient = sc->ambient, .rim_col = sc->rim,
                .rim_power = 3};
    fig_render(b, cam, &L, &f->m, pose);
    float line = sc->h >= 200 ? 0.9f : sc->h >= 120 ? 0.75f : 0.6f;
    fig_resolve(b, cam, &f->m, out, NULL, line);
}

bool scene_project(const fcam *cam, v3 p, float *x, float *y) {
    v3 v = m4point(&cam->view, p);
    if (v.z > -1e-4f)
        return false;
    float f = 1.0f / tanf(cam->fovy * 0.5f);
    *x = cam->w * 0.5f + v.x / -v.z * f * cam->h * 0.5f;
    *y = cam->h * 0.5f - v.y / -v.z * f * cam->h * 0.5f;
    return true;
}

void scene_head(const figure *f, const fcam *cam, const fpose *pose, float *x, float *y, float *r) {
    int hb = f->body.bone[BN_HEAD];
    v3 c = m4point(&pose->skin[hb], f->head.center);
    v3 top = m4point(&pose->skin[hb], vadd(f->head.center, V3(0, f->body.hp.h * 0.5f, 0)));
    float tx, ty;
    *x = *y = *r = 0;
    if (scene_project(cam, c, x, y) && scene_project(cam, top, &tx, &ty))
        *r = fabsf(ty - *y) * 1.15f;
}

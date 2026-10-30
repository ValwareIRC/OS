#pragma once
/* How she's shown: the camera's framing (face, bust, waist, whole), the
 * light's colours (from the design, so she belongs to each one), and the
 * size of the picture. */
#include "figure.h"

enum { SHOT_FACE, SHOT_BUST, SHOT_WAIST, SHOT_FULL };

typedef struct {
    int w, h, ss, shot;
    rgb key, ambient, rim;
    float yaw, pitch;   /* the camera, a little round her */
    float pull;         /* 0..1: eased back to waist-up (while she gestures) */
} scene;

void scene_defaults(scene *sc);
int scene_shot_from_name(const char *name);
void scene_camera(const figure *f, const scene *sc, fcam *cam);
/* draws her into `out` (w x h, premultiplied ARGB); b is the working buffer (w*ss x h*ss) */
void scene_draw(figure *f, const scene *sc, const fcam *cam, const fpose *pose, fbuf *b, uint32_t *out);
/* where her head is in the picture: centre and radius, in pixels */
void scene_head(const figure *f, const fcam *cam, const fpose *pose, float *x, float *y, float *r);
/* a point in model space to picture pixels; false behind the camera */
bool scene_project(const fcam *cam, v3 p, float *x, float *y);

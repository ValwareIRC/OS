#pragma once
/* A whole figure: a look (everything chosen about her) made into a model. */
#include "body.h"
#include "face.h"
#include "hair.h"
#include "head.h"
#include "outfit.h"

typedef struct {
    char name[48];
    body_params body;
    face_params face;
    hair_params hair;
    outfit_params outfit;
    /* her room, seen behind her in the frame */
    rgb wall, wall2, floor_col, lamp;
    int props;          /* ROOM_* bits */
    /* manner */
    float energy, shyness;
    int walk;           /* WALK_* */
} look;

enum { ROOM_LAMP = 1, ROOM_PLANT = 2, ROOM_WINDOW = 4, ROOM_SHELF = 8, ROOM_POSTERS = 16, ROOM_FAIRY = 32 };
enum { WALK_LIGHT, WALK_BOUNCY, WALK_CALM, WALK_SHY };

typedef struct {
    fmodel m;
    look lk;
    body_info body;
    head_info head;
    hair_info hair;
    int face_tex;          /* the face texture's index */
    face_state drawn;      /* what the face texture shows now */
    bool face_dirty;
    /* a VRM model someone brought instead of a generated body */
    bool vrm;
    int jaw, leye, reye;     /* bones (-1 none) */
    int morph_mesh[8], morph_blink[8], morph_angry[8], nmorph_mesh;
    char vrm_title[64], vrm_author[64], vrm_licence[300];
} figure;

/* a VRM (0.x or 1.0) as her body, from the file's bytes; false with a reason */
bool figure_load_vrm(figure *f, const unsigned char *data, size_t len, char *why, size_t whylen);
/* her face's state on a VRM: blinking, the jaw, the eyes (adjusts the pose) */
void figure_vrm_face(figure *f, const face_state *fs, fpose *pose);

void look_default(look *lk);
/* builds the model; `cell` is how fine the body is (0: the usual) */
bool figure_build(figure *f, const look *lk, float cell);
void figure_free(figure *f);
/* redraws the face texture if the state changed (cheaply where it can) */
void figure_face(figure *f, const face_state *fs);
/* where the head is in model space (for framing and captions) */
v3 figure_head_center(const figure *f);

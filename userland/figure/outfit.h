#pragma once
#include "body.h"

enum { TOP_NONE, TOP_TEE, TOP_BLOUSE, TOP_SWEATER, TOP_TANK, TOP_CROP, TOP_DRESS, TOP_STYLES };
enum { BOTTOM_NONE, BOTTOM_SKIRT, BOTTOM_PLEATED, BOTTOM_SHORTS, BOTTOM_PANTS, BOTTOM_LONGSKIRT, BOTTOM_STYLES };
enum { LEGS_NONE, LEGS_THIGHHIGHS, LEGS_KNEESOCKS, LEGS_TIGHTS, LEGS_ANKLESOCKS, LEGS_STYLES };
enum { SHOES_NONE, SHOES_SNEAKERS, SHOES_BOOTS, SHOES_LOAFERS, SHOES_STYLES };
enum { NECK_ROUND, NECK_SCOOP, NECK_V, NECK_HIGH };

typedef struct {
    int top, bottom, legs, shoes, neckline;
    rgb top_col, top_col2;     /* main colour, and its pattern's */
    rgb bottom_col, bottom_col2;
    rgb legs_col, shoes_col, sole_col;
    int top_pattern, bottom_pattern;
    float sleeve;              /* 0 none .. 0.35 short .. 1 to the wrist */
    float hem;                 /* the top's length: 0 cropped .. 1 over the hips */
    float skirt_len;           /* 0.4 very short .. 1 to the knee .. 1.6 long */
    float flare;               /* how much the skirt spreads, 0..1 */
} outfit_params;

outfit_params outfit_defaults(void);
/* the clothes, fitted to the body (built in its T-pose) */
void outfit_build(fmodel *m, const body_info *bi, const outfit_params *op);

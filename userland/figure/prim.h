#pragma once
#include "fig.h"

fmesh *prim_ellipsoid(fmodel *m, int mat, v3 c, v3 r, quat q, int bone, int rings, int segs);
fmesh *prim_tube(fmodel *m, int mat, const v3 *path, const float *rad, int n, int segs, bool caps, v3 up);
void prim_skin_along(fmesh *me, int bone_a, int bone_b, v3 from, v3 to, float blend);

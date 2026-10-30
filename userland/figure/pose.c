/* From a pose (each bone's rotation) to the matrices that move the meshes.
 * Bones are listed parents first, and every bone's bind rotation is the
 * identity, as in VRM's normalized humanoid: a bone's local rotation turns
 * it about its own head, in the parent's frame. */
#include "fig.h"

void fig_pose_rest(const fmodel *m, fpose *p) {
    for (int i = 0; i < m->nbones; i++)
        p->local[i] = qid();
    p->root = V3(0, 0, 0);
    fig_pose_update(m, p);
}

void fig_pose_update(const fmodel *m, fpose *p) {
    for (int i = 0; i < m->nbones; i++) {
        const fbone *b = &m->bone[i];
        if (b->parent < 0) {
            m4 t = m4trs(vadd(b->head, p->root), p->local[i], 1);
            p->world[i] = t;
        } else {
            v3 off = vsub(b->head, m->bone[b->parent].head);
            m4 t = m4trs(off, p->local[i], 1);
            p->world[i] = m4mul(p->world[b->parent], t);
        }
        m4 unbind = m4trs(vmul(b->head, -1), qid(), 1);
        p->skin[i] = m4mul(p->world[i], unbind);
    }
}

#include <lkpi/all.h>

/* kernel/linuxkpi: red-black trees, the textbook way (insert and erase with their fix-ups), on Linux's node layout:
 * the parent pointer and the colour share a word (bit 0: 1 black, 0 red), as rb_link_node leaves a new node red. */

#define RED 0UL
#define BLACK 1UL

static inline struct rb_node *parent_of(const struct rb_node *n) { return (struct rb_node *)(n->__rb_parent_color & ~3UL); }
static inline unsigned long color_of(const struct rb_node *n) { return n->__rb_parent_color & 1UL; }
static inline bool is_red(const struct rb_node *n) { return n && color_of(n) == RED; }
static inline bool is_black(const struct rb_node *n) { return !n || color_of(n) == BLACK; }
static inline void set_parent(struct rb_node *n, struct rb_node *p) { n->__rb_parent_color = (unsigned long)p | color_of(n); }
static inline void set_color(struct rb_node *n, unsigned long c) { n->__rb_parent_color = (n->__rb_parent_color & ~1UL) | c; }

static void replace_child(struct rb_node *parent, struct rb_node *old, struct rb_node *new_, struct rb_root *root) {
    if (!parent)
        root->rb_node = new_;
    else if (parent->rb_left == old)
        parent->rb_left = new_;
    else
        parent->rb_right = new_;
}

static void rotate_left(struct rb_node *x, struct rb_root *root) {
    struct rb_node *y = x->rb_right, *p = parent_of(x);
    x->rb_right = y->rb_left;
    if (y->rb_left)
        set_parent(y->rb_left, x);
    set_parent(y, p);
    replace_child(p, x, y, root);
    y->rb_left = x;
    set_parent(x, y);
}

static void rotate_right(struct rb_node *x, struct rb_root *root) {
    struct rb_node *y = x->rb_left, *p = parent_of(x);
    x->rb_left = y->rb_right;
    if (y->rb_right)
        set_parent(y->rb_right, x);
    set_parent(y, p);
    replace_child(p, x, y, root);
    y->rb_right = x;
    set_parent(x, y);
}

void rb_insert_color(struct rb_node *n, struct rb_root *root) {
    struct rb_node *p;
    while ((p = parent_of(n)) && is_red(p)) {
        struct rb_node *g = parent_of(p); /* (a red node is never the root, so it has a parent) */
        if (p == g->rb_left) {
            struct rb_node *u = g->rb_right;
            if (is_red(u)) {
                set_color(p, BLACK), set_color(u, BLACK), set_color(g, RED);
                n = g;
                continue;
            }
            if (n == p->rb_right) {
                rotate_left(p, root);
                n = p;
                p = parent_of(n);
            }
            set_color(p, BLACK), set_color(g, RED);
            rotate_right(g, root);
        } else {
            struct rb_node *u = g->rb_left;
            if (is_red(u)) {
                set_color(p, BLACK), set_color(u, BLACK), set_color(g, RED);
                n = g;
                continue;
            }
            if (n == p->rb_left) {
                rotate_right(p, root);
                n = p;
                p = parent_of(n);
            }
            set_color(p, BLACK), set_color(g, RED);
            rotate_left(g, root);
        }
    }
    set_color(root->rb_node, BLACK);
}

/* after a black node left: `n` (perhaps NULL) under `parent` is short of one black */
static void erase_fixup(struct rb_node *n, struct rb_node *parent, struct rb_root *root) {
    while (is_black(n) && n != root->rb_node) {
        if (parent->rb_left == n) {
            struct rb_node *s = parent->rb_right;
            if (is_red(s)) {
                set_color(s, BLACK), set_color(parent, RED);
                rotate_left(parent, root);
                s = parent->rb_right;
            }
            if (is_black(s->rb_left) && is_black(s->rb_right)) {
                set_color(s, RED);
                n = parent;
                parent = parent_of(n);
            } else {
                if (is_black(s->rb_right)) {
                    set_color(s->rb_left, BLACK), set_color(s, RED);
                    rotate_right(s, root);
                    s = parent->rb_right;
                }
                set_color(s, color_of(parent)), set_color(parent, BLACK);
                if (s->rb_right)
                    set_color(s->rb_right, BLACK);
                rotate_left(parent, root);
                n = root->rb_node;
                break;
            }
        } else {
            struct rb_node *s = parent->rb_left;
            if (is_red(s)) {
                set_color(s, BLACK), set_color(parent, RED);
                rotate_right(parent, root);
                s = parent->rb_left;
            }
            if (is_black(s->rb_left) && is_black(s->rb_right)) {
                set_color(s, RED);
                n = parent;
                parent = parent_of(n);
            } else {
                if (is_black(s->rb_left)) {
                    set_color(s->rb_right, BLACK), set_color(s, RED);
                    rotate_left(s, root);
                    s = parent->rb_left;
                }
                set_color(s, color_of(parent)), set_color(parent, BLACK);
                if (s->rb_left)
                    set_color(s->rb_left, BLACK);
                rotate_right(parent, root);
                n = root->rb_node;
                break;
            }
        }
    }
    if (n)
        set_color(n, BLACK);
}

void rb_erase(struct rb_node *z, struct rb_root *root) {
    struct rb_node *child, *parent;
    unsigned long color;
    if (z->rb_left && z->rb_right) {
        /* two children: its successor takes its place (and its colour); the successor's own spot is what shrinks */
        struct rb_node *y = z->rb_right;
        while (y->rb_left)
            y = y->rb_left;
        replace_child(parent_of(z), z, y, root);
        child = y->rb_right;
        parent = parent_of(y);
        color = color_of(y);
        if (parent == z) {
            parent = y;
        } else {
            if (child)
                set_parent(child, parent);
            parent->rb_left = child;
            y->rb_right = z->rb_right;
            set_parent(z->rb_right, y);
        }
        y->__rb_parent_color = z->__rb_parent_color;
        y->rb_left = z->rb_left;
        set_parent(z->rb_left, y);
    } else {
        child = z->rb_left ? z->rb_left : z->rb_right;
        parent = parent_of(z);
        color = color_of(z);
        if (child)
            set_parent(child, parent);
        replace_child(parent, z, child, root);
    }
    if (color == BLACK)
        erase_fixup(child, parent, root);
}

struct rb_node *rb_first(const struct rb_root *root) {
    struct rb_node *n = root->rb_node;
    if (n)
        while (n->rb_left)
            n = n->rb_left;
    return n;
}

struct rb_node *rb_last(const struct rb_root *root) {
    struct rb_node *n = root->rb_node;
    if (n)
        while (n->rb_right)
            n = n->rb_right;
    return n;
}

struct rb_node *rb_next(const struct rb_node *n) {
    if (RB_EMPTY_NODE(n))
        return NULL;
    if (n->rb_right) {
        n = n->rb_right;
        while (n->rb_left)
            n = n->rb_left;
        return (struct rb_node *)n;
    }
    struct rb_node *p;
    while ((p = parent_of(n)) && n == p->rb_right)
        n = p;
    return p;
}

struct rb_node *rb_prev(const struct rb_node *n) {
    if (RB_EMPTY_NODE(n))
        return NULL;
    if (n->rb_left) {
        n = n->rb_left;
        while (n->rb_right)
            n = n->rb_right;
        return (struct rb_node *)n;
    }
    struct rb_node *p;
    while ((p = parent_of(n)) && n == p->rb_left)
        n = p;
    return p;
}

void rb_replace_node(struct rb_node *victim, struct rb_node *new_, struct rb_root *root) {
    struct rb_node *p = parent_of(victim);
    *new_ = *victim;
    if (victim->rb_left)
        set_parent(victim->rb_left, new_);
    if (victim->rb_right)
        set_parent(victim->rb_right, new_);
    replace_child(p, victim, new_, root);
}

static struct rb_node *left_deepest(const struct rb_node *n) {
    for (;;) {
        if (n->rb_left)
            n = n->rb_left;
        else if (n->rb_right)
            n = n->rb_right;
        else
            return (struct rb_node *)n;
    }
}

struct rb_node *rb_first_postorder(const struct rb_root *root) { return root->rb_node ? left_deepest(root->rb_node) : NULL; }

struct rb_node *rb_next_postorder(const struct rb_node *n) {
    if (!n)
        return NULL;
    struct rb_node *p = parent_of(n);
    if (p && n == p->rb_left && p->rb_right)
        return left_deepest(p->rb_right);
    return p;
}

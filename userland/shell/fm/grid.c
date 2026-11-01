/* CSS grid: sized tracks (px, %, fr, auto, minmax(), repeat(), auto-fill),
 * items placed by line numbers, spans or named areas, the rest flowing into
 * the first free cells. Used for measuring and for arranging (layout.c). */
#include "fmi.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAXT 24   /* tracks each way */
#define MAXR 64   /* rows when they come by themselves */

enum { T_PX, T_FR, T_AUTO };
typedef struct {
    int n;
    char kind[MAXT];
    float v[MAXT];  /* px, or fr */
    float min[MAXT]; /* minmax(): the least, px */
} tracks;

static const char *ws(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n')
        s++;
    return s;
}

/* one size: 120px, 25%, 1fr, auto, min-content, minmax(a, b) */
static void one_size(const char *s, float avail, char *kind, float *v, float *mn) {
    s = ws(s);
    *mn = 0;
    if (!strncasecmp(s, "minmax(", 7)) {
        char a[32] = "", b[32] = "";
        sscanf(s + 7, " %31[^,] , %31[^)]", a, b);
        char k2;
        float v2, m2;
        one_size(a, avail, &k2, &v2, &m2);
        *mn = k2 == T_PX ? v2 : 0;
        one_size(b, avail, kind, v, &m2);
        return;
    }
    if (!strncasecmp(s, "auto", 4) || !strncasecmp(s, "min-content", 11) || !strncasecmp(s, "max-content", 11) ||
        !strncasecmp(s, "fit-content", 11)) {
        *kind = T_AUTO, *v = 0;
        return;
    }
    char *e;
    float x = strtof(s, &e);
    if (!strncasecmp(e, "fr", 2))
        *kind = T_FR, *v = x;
    else if (*e == '%')
        *kind = T_PX, *v = avail * x / 100;
    else
        *kind = T_PX, *v = x;
}

/* the next item of a track list (brackets kept whole) into out */
static const char *next_item(const char *p, char *out, size_t n) {
    p = ws(p);
    size_t k = 0;
    int depth = 0;
    while (*p && (depth || (*p != ' ' && *p != '\t' && *p != '\n'))) {
        if (*p == '(') depth++;
        if (*p == ')') depth--;
        if (k < n - 1)
            out[k++] = *p;
        p++;
    }
    out[k] = 0;
    return p;
}

static void add_track(tracks *t, const char *item, float avail) {
    if (t->n >= MAXT || !item[0])
        return;
    one_size(item, avail, &t->kind[t->n], &t->v[t->n], &t->min[t->n]);
    t->n++;
}

/* "100px 1fr repeat(3, 1fr) repeat(auto-fill, minmax(120px, 1fr))" for `avail` px */
static void read_tracks(const char *spec, float avail, float gap, tracks *t) {
    t->n = 0;
    char item[120];
    for (const char *p = spec; *(p = ws(p));) {
        p = next_item(p, item, sizeof item);
        if (!strncasecmp(item, "repeat(", 7)) {
            const char *in = item + 7;
            char count[24] = "";
            sscanf(in, " %23[^,]", count);
            const char *comma = strchr(in, ',');
            if (!comma)
                continue;
            char inner[100];
            snprintf(inner, sizeof inner, "%s", comma + 1);
            char *close = strrchr(inner, ')');
            if (close)
                *close = 0;
            int times;
            if (!strncasecmp(ws(count), "auto-fill", 9) || !strncasecmp(ws(count), "auto-fit", 8)) {
                /* as many as fit: each at its least (or its size, if fixed) */
                tracks one = {0};
                read_tracks(inner, avail, gap, &one);
                float each = 0;
                for (int i = 0; i < one.n; i++)
                    each += (one.kind[i] == T_PX ? one.v[i] : one.min[i] > 0 ? one.min[i] : 100) + gap;
                times = each > 0 ? (int)((avail + gap) / each) : 1;
                if (times < 1)
                    times = 1;
            } else {
                times = atoi(count);
            }
            for (int r = 0; r < times && t->n < MAXT; r++) {
                char it[60];
                for (const char *q = inner; *(q = ws(q));) {
                    q = next_item(q, it, sizeof it);
                    add_track(t, it, avail);
                }
            }
        } else {
            add_track(t, item, avail);
        }
    }
}

/* ---- named areas: "head head" "side main" ---- */

typedef struct {
    int rows, cols;
    char name[MAXR][MAXT][20];
} areas;

static void read_areas(const char *spec, areas *a) {
    a->rows = a->cols = 0;
    for (const char *p = spec; (p = strchr(p, '"')) && a->rows < MAXR;) {
        const char *e = strchr(p + 1, '"');
        if (!e)
            break;
        int c = 0;
        for (const char *q = p + 1; q < e && c < MAXT;) {
            while (q < e && *q == ' ')
                q++;
            if (q >= e)
                break;
            int k = 0;
            while (q < e && *q != ' ') {
                if (k < 19)
                    a->name[a->rows][c][k++] = *q;
                q++;
            }
            a->name[a->rows][c][k] = 0;
            c++;
        }
        if (c > a->cols)
            a->cols = c;
        a->rows++;
        p = e + 1;
    }
}

static bool area_of(const areas *a, const char *name, int *r0, int *c0, int *r1, int *c1) {
    *r0 = *c0 = 1 << 20, *r1 = *c1 = -1;
    for (int r = 0; r < a->rows; r++)
        for (int c = 0; c < a->cols; c++)
            if (!strcmp(a->name[r][c], name)) {
                if (r < *r0) *r0 = r;
                if (c < *c0) *c0 = c;
                if (r > *r1) *r1 = r;
                if (c > *c1) *c1 = c;
            }
    return *r1 >= 0;
}

/* ---- solving ---- */

typedef struct {
    int nc, nr;
    float colw[MAXT], rowh[MAXR];
    int pr0[256], pc0[256], pr1[256], pc1[256]; /* each item's cells, inclusive */
    float cgap, rgap;
} solved;

static void solve(fm_doc *d, fm_node *n, fm_node **kids, int nk, float aw, float ah, solved *S) {
    const fm_style *st = &n->st;
    S->cgap = st->col_gap >= 0 ? st->col_gap : st->gap;
    S->rgap = st->row_gap >= 0 ? st->row_gap : st->gap;
    tracks cols = {0}, rows = {0};
    /* everything here is the call's own: measuring an item may solve a grid inside it */
    areas *Ap = st->grid_areas[0] ? calloc(1, sizeof *Ap) : NULL;
    areas none = {0};
    areas *Aa = Ap ? Ap : &none;
    if (Ap)
        read_areas(st->grid_areas, Ap);
#define A (*Aa)
    if (st->grid_cols[0])
        read_tracks(st->grid_cols, aw, S->cgap, &cols);
    if (!cols.n) {
        int k = A.cols ? A.cols : st->columns > 0 ? st->columns : 3;
        for (int i = 0; i < k && i < MAXT; i++)
            cols.kind[cols.n] = T_FR, cols.v[cols.n] = 1, cols.min[cols.n++] = 0;
    }
    if (st->grid_rows[0])
        read_tracks(st->grid_rows, ah > 0 ? ah : 0, S->rgap, &rows);
    int nc = cols.n;
    /* placing: named areas and line numbers first, then the rest in order into free cells */
    bool used[MAXR][MAXT];
    memset(used, 0, sizeof used);
    int maxr = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < nk && i < 256; i++) {
            fm_node *k = kids[i];
            const fm_style *ks = &k->st;
            int r0 = -1, c0 = -1, r1, c1;
            bool fixed = false;
            if (ks->grid_area[0] && A.rows) {
                fixed = area_of(&A, ks->grid_area, &r0, &c0, &r1, &c1);
            } else if (ks->gc0 > 0 || ks->gr0 > 0) {
                fixed = true;
            }
            if ((pass == 0) != fixed)
                continue;
            if (!(ks->grid_area[0] && A.rows && r0 >= 0)) {
                int cs = ks->gcs > 0 ? ks->gcs : ks->gc1 > ks->gc0 && ks->gc0 > 0 ? ks->gc1 - ks->gc0 : 1;
                int rs = ks->grs > 0 ? ks->grs : ks->gr1 > ks->gr0 && ks->gr0 > 0 ? ks->gr1 - ks->gr0 : 1;
                if (ks->gc1 < 0) /* "1 / -1": to the last line */
                    cs = nc + 2 + ks->gc1 - (ks->gc0 > 0 ? ks->gc0 : 1);
                if (cs > nc) cs = nc;
                if (cs < 1) cs = 1;
                if (ks->gc0 > 0 && ks->gr0 > 0) {
                    c0 = ks->gc0 - 1, r0 = ks->gr0 - 1;
                } else { /* the first free run of cells, row by row (a fixed column or row if given) */
                    bool found = false;
                    for (int r = ks->gr0 > 0 ? ks->gr0 - 1 : 0; r < MAXR && !found; r++) {
                        for (int c = ks->gc0 > 0 ? ks->gc0 - 1 : 0; c + cs <= nc && !found; c++) {
                            bool free_ = true;
                            for (int rr = r; rr < r + rs && free_; rr++)
                                for (int cc = c; cc < c + cs && free_; cc++)
                                    free_ = rr < MAXR && !used[rr][cc];
                            if (free_)
                                r0 = r, c0 = c, found = true;
                            if (ks->gc0 > 0)
                                break;
                        }
                        if (ks->gr0 > 0 && !found)
                            break;
                    }
                    if (!found)
                        r0 = maxr, c0 = 0;
                }
                r1 = r0 + rs - 1, c1 = c0 + cs - 1;
            }
            if (c1 >= nc) c1 = nc - 1;
            if (c0 >= nc) c0 = nc - 1;
            if (r1 >= MAXR) r1 = MAXR - 1;
            for (int rr = r0; rr <= r1; rr++)
                for (int cc = c0; cc <= c1; cc++)
                    used[rr][cc] = true;
            S->pr0[i] = r0, S->pc0[i] = c0, S->pr1[i] = r1, S->pc1[i] = c1;
            if (r1 + 1 > maxr)
                maxr = r1 + 1;
        }
    if (A.rows > maxr)
        maxr = A.rows;
#undef A
    free(Ap);
    if (rows.n > maxr)
        maxr = rows.n;
    if (maxr < 1)
        maxr = 1;
    S->nc = nc, S->nr = maxr;
    /* columns: fixed ones as they are; auto ones as wide as what is in them; fr share what is left */
    float fixed_w = S->cgap * (nc - 1), fr = 0;
    for (int c = 0; c < nc; c++) {
        S->colw[c] = 0;
        if (cols.kind[c] == T_PX)
            S->colw[c] = cols.v[c];
        else if (cols.kind[c] == T_AUTO) {
            for (int i = 0; i < nk && i < 256; i++)
                if (S->pc0[i] == c && S->pc1[i] == c) {
                    fmi_measure(d, kids[i], aw, ah);
                    S->colw[c] = fmaxf(S->colw[c], kids[i]->mw + kids[i]->st.margin[1] + kids[i]->st.margin[3]);
                }
        } else
            fr += cols.v[c];
        if (cols.kind[c] != T_FR)
            fixed_w += S->colw[c];
    }
    float left = aw - fixed_w;
    if (aw > 1e6f || left < 0) { /* measuring with no limit: fr tracks as wide as what is in them */
        for (int c = 0; c < nc; c++)
            if (cols.kind[c] == T_FR) {
                float want = cols.min[c];
                for (int i = 0; i < nk && i < 256; i++)
                    if (S->pc0[i] == c && S->pc1[i] == c) {
                        fmi_measure(d, kids[i], 1e7f, ah);
                        want = fmaxf(want, kids[i]->mw + kids[i]->st.margin[1] + kids[i]->st.margin[3]);
                    }
                S->colw[c] = want;
            }
    } else {
        for (int c = 0; c < nc; c++)
            if (cols.kind[c] == T_FR)
                S->colw[c] = fmaxf(cols.min[c], fr > 0 ? left * cols.v[c] / fr : 0);
    }
    /* rows: fixed ones as they are; the others as tall as what is in them (spans share the extra) */
    float fr_r = 0, fixed_h = S->rgap * (maxr - 1);
    for (int r = 0; r < maxr; r++) {
        S->rowh[r] = 0;
        if (r < rows.n && rows.kind[r] == T_PX)
            S->rowh[r] = rows.v[r];
        else if (r < rows.n && rows.kind[r] == T_FR)
            fr_r += rows.v[r];
    }
    for (int i = 0; i < nk && i < 256; i++) {
        float w = -S->cgap;
        for (int c = S->pc0[i]; c <= S->pc1[i]; c++)
            w += S->colw[c] + S->cgap;
        fmi_measure(d, kids[i], w, ah);
        float h = kids[i]->mh + kids[i]->st.margin[0] + kids[i]->st.margin[2];
        int r0 = S->pr0[i], r1 = S->pr1[i];
        float have = -S->rgap;
        bool flexible = false;
        for (int r = r0; r <= r1; r++) {
            have += S->rowh[r] + S->rgap;
            flexible |= !(r < rows.n && rows.kind[r] == T_PX);
        }
        if (h > have && flexible) /* the last flexible row it spans grows */
            for (int r = r1; r >= r0; r--)
                if (!(r < rows.n && rows.kind[r] == T_PX)) {
                    S->rowh[r] += h - have;
                    break;
                }
    }
    for (int r = 0; r < maxr; r++)
        if (!(r < rows.n && rows.kind[r] == T_FR))
            fixed_h += S->rowh[r];
    if (fr_r > 0 && ah > 0 && ah < 1e6f) {
        float lh = ah - fixed_h;
        for (int r = 0; r < maxr && r < rows.n; r++)
            if (rows.kind[r] == T_FR)
                S->rowh[r] = fmaxf(S->rowh[r], lh > 0 ? lh * rows.v[r] / fr_r : 0);
    }
}

void fmi_grid_measure(fm_doc *d, fm_node *n, fm_node **kids, int nk, float aw, float ah, float *w, float *h) {
    solved S;
    const fm_style *st = &n->st;
    if (!st->grid_cols[0] && !st->grid_areas[0]) { /* "columns: 3": equal cells, as wide as the widest item */
        int cols = st->columns > 0 ? st->columns : 3;
        float widest = 0, gap = st->col_gap >= 0 ? st->col_gap : st->gap;
        for (int i = 0; i < nk; i++) {
            fmi_measure(d, kids[i], aw, ah);
            widest = fmaxf(widest, kids[i]->mw + kids[i]->st.margin[1] + kids[i]->st.margin[3]);
        }
        int used = nk < cols ? nk : cols;
        float want = used * widest + gap * (used - 1);
        if (want < aw)
            aw = want;
    }
    solve(d, n, kids, nk, aw, ah, &S);
    float W = S.cgap * (S.nc - 1), H = S.rgap * (S.nr - 1);
    for (int c = 0; c < S.nc; c++)
        W += S.colw[c];
    for (int r = 0; r < S.nr; r++)
        H += S.rowh[r];
    *w = W, *h = H;
}

void fmi_grid_arrange(fm_doc *d, fm_node *n, fm_node **kids, int nk, rect c) {
    solved S;
    solve(d, n, kids, nk, (float)c.w, (float)c.h, &S);
    float xs[MAXT + 1], ys[MAXR + 1];
    xs[0] = (float)c.x;
    for (int i = 0; i < S.nc; i++)
        xs[i + 1] = xs[i] + S.colw[i] + S.cgap;
    ys[0] = (float)c.y;
    for (int i = 0; i < S.nr; i++)
        ys[i + 1] = ys[i] + S.rowh[i] + S.rgap;
    const fm_style *st = &n->st;
    for (int i = 0; i < nk && i < 256; i++) {
        fm_node *k = kids[i];
        float x0 = xs[S.pc0[i]], y0 = ys[S.pr0[i]];
        float cw = xs[S.pc1[i] + 1] - S.cgap - x0, ch = ys[S.pr1[i] + 1] - S.rgap - y0;
        cw -= k->st.margin[1] + k->st.margin[3];
        ch -= k->st.margin[0] + k->st.margin[2];
        enum fm_align a = k->st.self_align_set ? k->st.self_align : st->align;
        fmi_measure(d, k, cw, ch);
        float w = a == FM_A_STRETCH && k->st.width.unit == FM_AUTO ? cw : fminf(k->mw, cw);
        float h = a == FM_A_STRETCH && k->st.height.unit == FM_AUTO ? ch : fminf(k->mh, ch);
        float ox = a == FM_A_CENTER ? (cw - w) / 2 : a == FM_A_END ? cw - w : 0;
        float oy = a == FM_A_CENTER ? (ch - h) / 2 : a == FM_A_END ? ch - h : 0;
        fmi_arrange(d, k, (rect){(int)(x0 + ox + k->st.margin[3]), (int)(y0 + oy + k->st.margin[0]), (int)w, (int)h});
    }
}

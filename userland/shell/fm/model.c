/* <model>: a 3D model (.glb, as Blender exports it) as part of a design.
 *
 *   <model src="~/Models/wheel.glb" turn="drag" animation="spin" play="scroll" hover="lift">
 *     <part node="Dome" action="open:launcher" title="Search"/>
 *   </model>
 *
 * Each <model> has a thread of its own that loads the file and draws frames (m3d, physically based) while the
 * desktop goes on: the newest finished frame is what shows. Its parts are what Blender named them ("app:files"
 * opens Files) or gave custom properties (action, hover, title), and <part> children can add or change any of
 * that. A part under the pointer lifts towards the viewer and brightens; a click runs its action like a
 * button's. The model can turn (drag, scroll, or spin by itself) and play its own animations: in a loop, once,
 * while hovered, on a click, or step by step with the scroll wheel (a wheel of apps turning slice by slice).
 * While it moves it is drawn quicker; still, at full quality. */
#include "fmi.h"
#include "../m3d/m3d.h"
#include "../image.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_OVERRIDES 32

typedef struct {
    char node[64], action[160], hover[48], title[96];
} override;

typedef struct {
    int w, h, ss;
    m3_view v;
    int anim[4];
    float time[4];
    int hover_part;
    float hover_amt;
    bool hover_lift, hover_glow;
} request;

typedef struct mstate {
    fm_node *node; /* NULL once the node is gone: the thread ends and frees everything */
    char path[320];
    override ov[MAX_OVERRIDES];
    int nov;
    pthread_t th;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool quit, pending, busy, loaded, failed, logged;
    bool has_thread, thread_done; /* who frees it: the thread when it ends after the node, else forget */
    char why[200];
    request req;
    /* the newest finished frame (straight-alpha ARGB) and which node each pixel shows */
    uint32_t *px;
    uint16_t *pick;
    int fw, fh;
    request shown;    /* what that frame was drawn with */
    int nanims, nparts;
    float durations[16];
    char anim_names[16][48];
    char part_action[256][160]; /* by node index, for clicks from the UI thread (filled once loaded) */
    char part_title[256][96];
    int part_of[1024];          /* node -> part, for picks */
    int nnodes;
    bool has_camera;
    /* the interface's side */
    float yaw, pitch, yaw_to, pitch_to, zoom;
    bool dragging, dragged;
    int press_x, press_y, press_part;
    float yaw0, pitch0;
    int hover_part;
    float hover_amt;
    float anim_t, anim_to;      /* play="scroll"/"click"/"hover": the time eased towards */
    uint64_t clock0, last_ms;
    rect at;
    struct mstate *next;
} mstate;

static mstate *states;
static pthread_mutex_t list_mu = PTHREAD_MUTEX_INITIALIZER;

/* ---- the drawing thread ---- */

static void apply_overrides(mstate *s, m3_model *m) {
    for (int k = 0; k < s->nov; k++)
        for (int i = 0; i < m->nnodes; i++) {
            char nm[64];
            snprintf(nm, sizeof nm, "%s", m->nodes[i].name);
            char *dot = strrchr(nm, '.');
            if (dot && strlen(dot) == 4)
                *dot = 0;
            if (strcasecmp(m->nodes[i].name, s->ov[k].node) && strcasecmp(nm, s->ov[k].node))
                continue;
            if (s->ov[k].action[0])
                snprintf(m->nodes[i].action, sizeof m->nodes[i].action, "%s", s->ov[k].action);
            if (s->ov[k].hover[0])
                snprintf(m->nodes[i].hover, sizeof m->nodes[i].hover, "%s", s->ov[k].hover);
            if (s->ov[k].title[0])
                snprintf(m->nodes[i].title, sizeof m->nodes[i].title, "%s", s->ov[k].title);
        }
    /* parts again, now that more nodes may do something */
    m->nparts = 0;
    for (int i = 0; i < m->nnodes; i++) {
        m->nodes[i].part = -1;
        for (int p = i; p >= 0; p = m->nodes[p].parent)
            if ((m->nodes[p].action[0] && strcasecmp(m->nodes[p].action, "none")) || m->nodes[p].hover[0]) {
                m->nodes[i].part = p;
                break;
            }
        m->nparts += m->nodes[i].part == i;
    }
}

/* the hovered part rises towards the viewer: the camera's direction in its parent's space */
static void lift_part(m3_model *m, const request *r) {
    for (int i = 0; i < m->nnodes; i++)
        m->nodes[i].lift[0] = m->nodes[i].lift[1] = m->nodes[i].lift[2] = 0, m->nodes[i].glow = 0;
    int p = r->hover_part;
    if (p < 0 || p >= m->nnodes || r->hover_amt <= 0.001f)
        return;
    m->nodes[p].glow = r->hover_glow ? r->hover_amt : 0;
    if (!r->hover_lift)
        return;
    float yaw = r->v.yaw * (float)M_PI / 180, pitch = r->v.pitch * (float)M_PI / 180;
    float d[3] = {sinf(yaw) * cosf(pitch), sinf(pitch), cosf(yaw) * cosf(pitch)};
    if (r->v.use_camera) /* the model's own camera: towards it */
        for (int i = 0; i < m->nnodes; i++)
            if (m->nodes[i].camera >= 0) {
                const float *w = m->nodes[i].world;
                float c[3] = {(m->lo.x + m->hi.x) / 2, (m->lo.y + m->hi.y) / 2, (m->lo.z + m->hi.z) / 2};
                float e[3] = {w[12] - c[0], w[13] - c[1], w[14] - c[2]};
                float l = sqrtf(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
                if (l > 0)
                    d[0] = e[0] / l, d[1] = e[1] / l, d[2] = e[2] / l;
                break;
            }
    float size = sqrtf((m->hi.x - m->lo.x) * (m->hi.x - m->lo.x) + (m->hi.y - m->lo.y) * (m->hi.y - m->lo.y) +
                       (m->hi.z - m->lo.z) * (m->hi.z - m->lo.z));
    float amount = size * 0.035f * r->hover_amt;
    int par = m->nodes[p].parent;
    if (par < 0) {
        m->nodes[p].lift[0] = d[0] * amount, m->nodes[p].lift[1] = d[1] * amount, m->nodes[p].lift[2] = d[2] * amount;
        return;
    }
    const float *w = m->nodes[par].world; /* into the parent's space: through its rotation and scale */
    for (int k = 0; k < 3; k++) {
        float cx = w[k * 4], cy = w[k * 4 + 1], cz = w[k * 4 + 2], s2 = cx * cx + cy * cy + cz * cz;
        m->nodes[p].lift[k] = s2 > 0 ? (cx * d[0] + cy * d[1] + cz * d[2]) * amount / s2 : 0;
    }
}

static void *worker(void *arg) {
    mstate *s = arg;
    pthread_mutex_lock(&list_mu); /* until state_of has finished setting it up */
    pthread_mutex_unlock(&list_mu);
    char why[200] = "";
    m3_model *m = m3_load_file(s->path, why, sizeof why);
    pthread_mutex_lock(&s->mu);
    if (!m) {
        s->failed = true;
        snprintf(s->why, sizeof s->why, "%s", why);
    } else {
        apply_overrides(s, m);
        s->nanims = m->nanims < 16 ? m->nanims : 16;
        for (int i = 0; i < s->nanims; i++) {
            s->durations[i] = m->anims[i].duration;
            snprintf(s->anim_names[i], sizeof s->anim_names[i], "%s", m->anims[i].name);
        }
        s->nnodes = m->nnodes < 1024 ? m->nnodes : 1024;
        for (int i = 0; i < s->nnodes; i++) {
            s->part_of[i] = m->nodes[i].part < 1024 ? m->nodes[i].part : -1;
            if (i < 256) {
                snprintf(s->part_action[i], sizeof s->part_action[i], "%s", m->nodes[i].action);
                snprintf(s->part_title[i], sizeof s->part_title[i], "%s", m->nodes[i].title);
            }
            if (m->nodes[i].camera >= 0)
                s->has_camera = true;
        }
        s->nparts = m->nparts;
        fprintf(stderr, "shell: facet: model %s: %d parts, %d animations%s\n", s->path, m->nparts, m->nanims,
                s->has_camera ? ", its own camera" : "");
    }
    s->loaded = true;
    while (!s->quit && m) {
        while (!s->pending && !s->quit && s->node)
            pthread_cond_wait(&s->cv, &s->mu);
        if (s->quit || !s->node)
            break;
        request r = s->req;
        s->pending = false;
        s->busy = true;
        pthread_mutex_unlock(&s->mu);

        int which[5] = {-1, -1, -1, -1, -1};
        for (int k = 0; k < 4; k++)
            which[k] = r.anim[k];
        m3_pose(m, which, r.time);
        lift_part(m, &r);
        m3_pose(m, which, r.time);
        uint32_t *px = malloc(sizeof *px * (size_t)r.w * r.h);
        uint16_t *pick = malloc(sizeof *pick * (size_t)r.w * r.h);
        if (px && pick) {
            r.v.supersample = r.ss;
            m3_render(m, &r.v, px, pick, r.w, r.h);
            for (int i = 0; i < r.w * r.h; i++) { /* premultiplied to straight alpha, as pictures are drawn */
                uint32_t p = px[i], a = p >> 24;
                if (a && a < 255)
                    px[i] = a << 24 | (((p >> 16) & 255) * 255 / a) << 16 | (((p >> 8) & 255) * 255 / a) << 8 | ((p & 255) * 255 / a);
            }
        }
        pthread_mutex_lock(&s->mu);
        s->busy = false;
        if (px && pick) {
            free(s->px), free(s->pick);
            s->px = px, s->pick = pick, s->fw = r.w, s->fh = r.h, s->shown = r;
        } else {
            free(px), free(pick);
        }
    }
    s->thread_done = true;
    bool orphan = !s->node; /* the node went first: this thread is the last to hold it */
    pthread_mutex_unlock(&s->mu);
    m3_free(m);
    if (orphan) {
        free(s->px), free(s->pick);
        pthread_mutex_destroy(&s->mu);
        pthread_cond_destroy(&s->cv);
        free(s);
    }
    return NULL;
}

/* ---- finding a model's state ---- */

static const char *resolve(const char *src, char *buf, size_t n) {
    if (src[0] == '/')
        return src;
    if (src[0] == '~' && src[1] == '/') {
        snprintf(buf, n, "%s/%s", getenv("HOME") ? getenv("HOME") : "/root", src + 2);
        return buf;
    }
    snprintf(buf, n, "/usr/share/os/models/%s", src); /* models that come with the system */
    return buf;
}

static bool path_ok(const char *p) {
    size_t n = strlen(p);
    if (strstr(p, "..") || strstr(p, "://") || n < 5 || strcasecmp(p + n - 4, ".glb"))
        return false;
    return strncmp(p, "/proc/", 6) && strncmp(p, "/sys/", 5) && strncmp(p, "/dev/", 5) &&
           (strncmp(p, "/etc/", 5) || !strncmp(p, "/etc/os/", 8));
}

static mstate *state_of(fm_node *n, bool make) {
    pthread_mutex_lock(&list_mu);
    mstate *s = states;
    while (s && s->node != n)
        s = s->next;
    const char *src = fm_attr_get(n, "src");
    char buf[320];
    const char *path = src && *src ? resolve(src, buf, sizeof buf) : "";
    if (s && strcmp(s->path, path)) { /* a new file: start again */
        pthread_mutex_unlock(&list_mu);
        fmi_model_forget(n);
        pthread_mutex_lock(&list_mu);
        s = NULL;
    }
    if (!s && make && path[0]) {
        s = calloc(1, sizeof *s);
        if (s) {
            s->node = n;
            snprintf(s->path, sizeof s->path, "%s", path);
            pthread_mutex_init(&s->mu, NULL);
            pthread_cond_init(&s->cv, NULL);
            s->hover_part = s->press_part = -1;
            s->zoom = 1;
            s->yaw = s->yaw_to = (float)atof(fm_attr_get(n, "yaw") ? fm_attr_get(n, "yaw") : "0");
            s->pitch = s->pitch_to = (float)atof(fm_attr_get(n, "pitch") ? fm_attr_get(n, "pitch") : "8");
            s->clock0 = fm_host_time_ms();
            for (fm_node *c = n->first; c && s->nov < MAX_OVERRIDES; c = c->next) {
                if (c->kind != FM_ELEM || strcasecmp(c->tag, "part"))
                    continue;
                override *o = &s->ov[s->nov++];
                snprintf(o->node, sizeof o->node, "%s", fm_attr_get(c, "node") ? fm_attr_get(c, "node") : "");
                snprintf(o->action, sizeof o->action, "%s", fm_attr_get(c, "action") ? fm_attr_get(c, "action") : "");
                snprintf(o->hover, sizeof o->hover, "%s", fm_attr_get(c, "hover") ? fm_attr_get(c, "hover") : "");
                snprintf(o->title, sizeof o->title, "%s", fm_attr_get(c, "title") ? fm_attr_get(c, "title") : "");
            }
            if (!path_ok(path)) {
                s->failed = s->loaded = true;
                snprintf(s->why, sizeof s->why, "only .glb files on this computer can be shown");
            } else if (pthread_create(&s->th, NULL, worker, s) == 0) {
                s->has_thread = true;
                pthread_detach(s->th);
            } else {
                s->failed = s->loaded = true;
                snprintf(s->why, sizeof s->why, "no thread to draw it");
            }
            s->next = states;
            states = s;
        }
    }
    pthread_mutex_unlock(&list_mu);
    return s;
}

void fmi_model_forget(fm_node *n) {
    pthread_mutex_lock(&list_mu);
    mstate **pp = &states;
    while (*pp && (*pp)->node != n)
        pp = &(*pp)->next;
    mstate *s = *pp;
    if (s)
        *pp = s->next;
    pthread_mutex_unlock(&list_mu);
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    s->node = NULL;
    s->quit = true;
    pthread_cond_signal(&s->cv);
    bool free_now = !s->has_thread || s->thread_done; /* else the thread frees it as it ends */
    pthread_mutex_unlock(&s->mu);
    if (free_now) {
        free(s->px), free(s->pick);
        pthread_mutex_destroy(&s->mu);
        pthread_cond_destroy(&s->cv);
        free(s);
    }
}

/* ---- what the design asks for ---- */

static float attr_f(const fm_node *n, const char *k, float d) {
    const char *v = fm_attr_get(n, k);
    return v && *v ? (float)atof(v) : d;
}

static bool attr_is(const fm_node *n, const char *k, const char *val) {
    const char *v = fm_attr_get(n, k);
    return v && !strcasecmp(v, val);
}

static int anim_index(mstate *s, const char *name) {
    if (!name || !*name)
        return s->nanims ? 0 : -1;
    for (int i = 0; i < s->nanims; i++)
        if (!strcasecmp(s->anim_names[i], name))
            return i;
    char *end;
    long k = strtol(name, &end, 10);
    return !*end && k >= 0 && k < s->nanims ? (int)k : -1;
}

/* the design's light and colours, so a model belongs to the design it is in */
static void light_from_design(const fm_node *n, m3_view *v) {
    const char *l = fm_attr_get(n, "light");
    if (l && *l) { /* "from top-left", "angle 30 45", "warm", "cool", "bright", "dim" in any order */
        if (strstr(l, "left")) v->light_dir.x = -0.6f;
        if (strstr(l, "right")) v->light_dir.x = 0.6f;
        if (strstr(l, "top")) v->light_dir.y = 0.8f;
        if (strstr(l, "front")) v->light_dir.z = 0.8f;
        if (strstr(l, "back")) v->light_dir.z = -0.6f;
        if (strstr(l, "warm")) v->light_color[0] = 1, v->light_color[1] = 0.88f, v->light_color[2] = 0.72f;
        if (strstr(l, "cool")) v->light_color[0] = 0.8f, v->light_color[1] = 0.9f, v->light_color[2] = 1;
        if (strstr(l, "bright")) v->light_intensity *= 1.5f, v->env_intensity *= 1.4f;
        if (strstr(l, "dim")) v->light_intensity *= 0.6f, v->env_intensity *= 0.6f;
        float l2 = sqrtf(v->light_dir.x * v->light_dir.x + v->light_dir.y * v->light_dir.y + v->light_dir.z * v->light_dir.z);
        if (l2 > 0)
            v->light_dir.x /= l2, v->light_dir.y /= l2, v->light_dir.z /= l2;
    }
    const char *e = fm_attr_get(n, "studio");
    if (e && !strcasecmp(e, "bright")) {
        v->env_top[0] = 0.9f, v->env_top[1] = 0.92f, v->env_top[2] = 0.96f;
        v->env_horizon[0] = v->env_horizon[1] = v->env_horizon[2] = 0.5f;
    } else if (e && !strcasecmp(e, "night")) {
        v->env_top[0] = 0.05f, v->env_top[1] = 0.06f, v->env_top[2] = 0.1f;
        v->env_horizon[0] = v->env_horizon[1] = 0.03f, v->env_horizon[2] = 0.05f;
        v->env_intensity *= 0.8f;
    }
    v->exposure = attr_f(n, "exposure", 1);
}

/* ---- drawing ---- */

static void ask(mstate *s, const request *r) {
    if (s->busy || s->pending) { /* one waits; the newest wins */
        s->req = *r, s->pending = true;
        return;
    }
    s->req = *r, s->pending = true;
    pthread_cond_signal(&s->cv);
}

static bool same(const request *a, const request *b) {
    return a->w == b->w && a->h == b->h && a->ss == b->ss && fabsf(a->v.yaw - b->v.yaw) < 0.05f &&
           fabsf(a->v.pitch - b->v.pitch) < 0.05f && fabsf(a->v.zoom - b->v.zoom) < 0.001f && a->hover_part == b->hover_part &&
           fabsf(a->hover_amt - b->hover_amt) < 0.01f && !memcmp(a->anim, b->anim, sizeof a->anim) &&
           fabsf(a->time[0] - b->time[0]) < 0.001f && a->v.use_camera == b->v.use_camera &&
           !memcmp(&a->v.light_dir, &b->v.light_dir, sizeof a->v.light_dir);
}

void fmi_model_draw(fm_doc *d, canvas *c, fm_node *n, rect r, float op) {
    (void)d;
    if (r.w < 4 || r.h < 4)
        return;
    mstate *s = state_of(n, true);
    if (!s)
        return;
    pthread_mutex_lock(&s->mu);
    s->at = r;
    if (s->failed) {
        if (!s->logged) {
            fprintf(stderr, "shell: facet: <model src=\"%s\">: %s\n", fm_attr_get(n, "src") ? fm_attr_get(n, "src") : "", s->why);
            s->logged = true;
        }
        pthread_mutex_unlock(&s->mu);
        return;
    }
    uint64_t now = fm_host_time_ms();
    float dt = s->last_ms ? (float)(now - s->last_ms) / 1000.0f : 0;
    if (dt > 0.1f)
        dt = 0.1f;
    s->last_ms = now;
    bool moving = false;
    if (s->loaded) {
        /* turning: by itself (spin, degrees a second), or eased to where a drag or scroll left it */
        float spin = attr_f(n, "spin", 0);
        if (spin != 0 && !s->dragging)
            s->yaw_to += spin * dt, moving = true;
        float k = 1 - expf(-dt * 14);
        if (fabsf(s->yaw_to - s->yaw) > 0.05f || fabsf(s->pitch_to - s->pitch) > 0.05f)
            s->yaw += (s->yaw_to - s->yaw) * k, s->pitch += (s->pitch_to - s->pitch) * k, moving = true;
        else
            s->yaw = s->yaw_to, s->pitch = s->pitch_to;
        /* hovering eases in and out */
        float ht = s->hover_part >= 0 ? 1 : 0;
        if (fabsf(ht - s->hover_amt) > 0.01f)
            s->hover_amt += (ht - s->hover_amt) * (1 - expf(-dt * 12)), moving = true;
        else
            s->hover_amt = ht;
    }
    request r2;
    memset(&r2, 0, sizeof r2);
    r2.w = r.w, r2.h = r.h;
    m3_view_default(&r2.v);
    r2.v.threads = 4;
    const char *cam = fm_attr_get(n, "camera");
    bool turnable = attr_is(n, "turn", "drag") || attr_is(n, "turn", "scroll") || attr_f(n, "spin", 0) != 0;
    r2.v.use_camera = s->has_camera && !turnable && !(cam && !strcasecmp(cam, "orbit"));
    if (cam && !strcasecmp(cam, "model"))
        r2.v.use_camera = s->has_camera;
    r2.v.yaw = s->yaw, r2.v.pitch = s->pitch;
    r2.v.zoom = attr_f(n, "zoom", 1) * 0.92f; /* a little room at the edges, for shadows and glow */
    r2.v.ortho = attr_is(n, "projection", "flat");
    light_from_design(n, &r2.v);
    for (int k = 0; k < 4; k++)
        r2.anim[k] = -1;
    /* its animation */
    const char *an = fm_attr_get(n, "animation");
    const char *play = fm_attr_get(n, "play");
    int ai = s->loaded ? anim_index(s, an) : -1;
    if (ai >= 0 && !(play && !strcasecmp(play, "none"))) {
        float dur = s->durations[ai] > 0 ? s->durations[ai] : 1;
        float speed = attr_f(n, "speed", 1);
        if (!play || !strcasecmp(play, "loop")) {
            s->anim_t = fmodf((float)(now - s->clock0) / 1000.0f * speed, dur);
            moving = true;
        } else if (!strcasecmp(play, "once")) {
            s->anim_t = fminf((float)(now - s->clock0) / 1000.0f * speed, dur);
            moving |= s->anim_t < dur;
        } else { /* hover, click, scroll: eased towards anim_to */
            if (!strcasecmp(play, "hover"))
                s->anim_to = s->hover_part >= 0 || pt_in(r, d->in ? d->in->mx : -1, d->in ? d->in->my : -1) ? dur : 0;
            float diff = s->anim_to - s->anim_t;
            if (fabsf(diff) > 0.002f)
                s->anim_t += diff * (1 - expf(-dt * 10 * speed)), moving = true;
            else
                s->anim_t = s->anim_to;
        }
        r2.anim[0] = ai;
        float t = s->anim_t;
        if (play && !strcasecmp(play, "scroll")) /* a wheel turning step by step goes round and round */
            t = fmodf(fmodf(t, dur) + dur, dur);
        r2.time[0] = t;
    }
    r2.hover_part = s->hover_part >= 0 ? s->hover_part : s->shown.hover_part;
    r2.hover_amt = s->hover_amt;
    const char *hv = fm_attr_get(n, "hover");
    r2.hover_lift = !hv || strstr(hv, "lift") || !strcasecmp(hv, "both");
    r2.hover_glow = !hv || strstr(hv, "glow") || !strcasecmp(hv, "both");
    if (hv && !strcasecmp(hv, "none"))
        r2.hover_lift = r2.hover_glow = false;
    const char *q = fm_attr_get(n, "quality");
    r2.ss = moving || s->dragging ? 1 : 2;
    if (q && !strcasecmp(q, "high"))
        r2.ss = moving ? 2 : 3;
    else if (q && !strcasecmp(q, "fast"))
        r2.ss = 1;
    if (s->loaded && (!same(&r2, &s->shown) || !s->px) && !(s->pending && same(&r2, &s->req)))
        ask(s, &r2);
    if (moving || s->busy || s->pending || !s->loaded)
        fm_animating = true; /* keep frames coming while it moves or a frame is being drawn */
    if (s->px) {
        image im = {s->fw, s->fh, s->px, s->fw, s->fh};
        /* a frame drawn for another size is shown at its own, centred, until the new one is ready */
        image_draw(c, &im, r.x + (r.w - s->fw) / 2, r.y + (r.h - s->fh) / 2, op);
    }
    pthread_mutex_unlock(&s->mu);
}

/* ---- the pointer ---- */

static int part_under(mstate *s, int x, int y) {
    if (!s->pick)
        return -1;
    int px = x - (s->at.x + (s->at.w - s->fw) / 2), py = y - (s->at.y + (s->at.h - s->fh) / 2);
    if (px < 0 || py < 0 || px >= s->fw || py >= s->fh)
        return -1;
    int nd = (int)s->pick[py * s->fw + px] - 1;
    return nd >= 0 && nd < s->nnodes ? s->part_of[nd] : -1;
}

bool fmi_model_pointer(fm_doc *d, fm_node *n, const input_state *in) {
    mstate *s = state_of(n, false);
    if (!s)
        return false;
    pthread_mutex_lock(&s->mu);
    bool inside = pt_in(s->at, in->mx, in->my);
    int part = inside ? part_under(s, in->mx, in->my) : -1;
    if (!s->dragging)
        s->hover_part = part;
    bool drag_turns = attr_is(n, "turn", "drag");
    char run[160] = "";
    if (in->pressed && inside) {
        s->press_part = part;
        s->press_x = in->mx, s->press_y = in->my;
        s->dragging = drag_turns;
        s->dragged = false;
        s->yaw0 = s->yaw_to, s->pitch0 = s->pitch_to;
    }
    if (s->dragging) {
        if (in->down) {
            int dx = in->mx - s->press_x, dy = in->my - s->press_y;
            if (abs(dx) + abs(dy) > 4)
                s->dragged = true;
            if (s->dragged) {
                float per = 300.0f / (s->at.w > 1 ? s->at.w : 300); /* half a turn across the model */
                s->yaw_to = s->yaw0 - dx * per * 0.6f;
                s->pitch_to = fmaxf(-80, fminf(80, s->pitch0 + dy * per * 0.4f));
            }
        } else {
            s->dragging = false;
        }
    }
    if (in->clicked && inside && !s->dragged) {
        if (part >= 0 && part == s->press_part && part < 256 && s->part_action[part][0] &&
            strcasecmp(s->part_action[part], "none"))
            snprintf(run, sizeof run, "%s", s->part_action[part]);
        const char *play = fm_attr_get(n, "play");
        int ai = anim_index(s, fm_attr_get(n, "animation"));
        if (play && !strcasecmp(play, "click") && ai >= 0)
            s->anim_to = s->anim_to > 0 ? 0 : s->durations[ai];
    }
    bool used_wheel = false;
    if (in->wheel && inside) {
        const char *play = fm_attr_get(n, "play");
        int ai = anim_index(s, fm_attr_get(n, "animation"));
        if (play && !strcasecmp(play, "scroll") && ai >= 0) {
            float step = attr_f(n, "step", s->durations[ai] > 0 ? s->durations[ai] : 1);
            s->anim_to -= in->wheel * step;
            used_wheel = true;
        } else if (attr_is(n, "turn", "scroll") || drag_turns) {
            s->yaw_to -= in->wheel * attr_f(n, "step", 15);
            used_wheel = true;
        }
    }
    pthread_mutex_unlock(&s->mu);
    if (run[0])
        fmi_run_actions(d, n, run);
    return used_wheel;
}

/* what the pointer is over, for its cursor and a tooltip */
const char *fmi_model_hover_title(fm_node *n, bool *clickable) {
    mstate *s = state_of(n, false);
    *clickable = false;
    if (!s)
        return NULL;
    pthread_mutex_lock(&s->mu);
    int p = s->hover_part;
    const char *t = NULL;
    if (p >= 0 && p < 256) {
        *clickable = s->part_action[p][0] && strcasecmp(s->part_action[p], "none");
        t = s->part_title[p][0] ? s->part_title[p] : NULL;
    }
    pthread_mutex_unlock(&s->mu);
    return t;
}

/* Every model loaded and its newest frame drawn (for tests and pictures of a design). */
bool fmi_models_settled(void) {
    bool ok = true;
    pthread_mutex_lock(&list_mu);
    for (mstate *s = states; s && ok; s = s->next) {
        pthread_mutex_lock(&s->mu);
        ok = s->failed || (s->loaded && s->px && !s->busy && !s->pending);
        pthread_mutex_unlock(&s->mu);
    }
    pthread_mutex_unlock(&list_mu);
    return ok;
}

/* What went wrong with a model, for the design's lint ("" if nothing has) */
const char *fmi_model_problem(fm_node *n) {
    mstate *s = state_of(n, false);
    return s && s->failed ? s->why : "";
}

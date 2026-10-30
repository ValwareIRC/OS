/* Life: breathing, a slow sway, blinks (now and then two), eyes that move in
 * little jumps as eyes do, her head turning to what she looks at, moods
 * drawn on her face, gestures from clips laid over all that, and her mouth
 * shaped by her voice: how loud it is opens it, and which of two bands of
 * the voice carries more decides the vowel (a, i, u, e, o). */
#include <stdlib.h>
#include <string.h>

#include "anim.h"

const char *const mood_names[MOOD_COUNT] = {"calm", "happy", "sad", "excited", "sleepy", "curious", "proud", "worried"};

int life_mood_from_name(const char *name) {
    for (int i = 0; i < MOOD_COUNT; i++)
        if (!strcmp(name, mood_names[i]))
            return i;
    return -1;
}

static float rnd(life *L) {
    L->rng = L->rng * 1664525u + 1013904223u;
    return (L->rng >> 8) / 16777216.0f;
}

void life_init(life *L, const figure *f, unsigned seed) {
    memset(L, 0, sizeof *L);
    L->rng = seed ? seed : 12345;
    L->act = L->act_queue = L->think_clip = -1;
    L->energy = f->lk.energy;
    for (int i = 0; clip_builtin_names[i] && L->nclips < LIFE_MAX_CLIPS; i++)
        if (clip_builtin(&L->clips[L->nclips], clip_builtin_names[i])) {
            clip_bind(&L->clips[L->nclips], &f->m);
            if (!strcmp(clip_builtin_names[i], "think"))
                L->think_clip = L->nclips;
            L->nclips++;
        }
    L->next_blink = 1.5;
}

void life_free(life *L) {
    for (int i = 0; i < L->nclips; i++)
        clip_free(&L->clips[i]);
    L->nclips = 0;
}

bool life_add_clip(life *L, const figure *f, const char *text) {
    clip c;
    if (!clip_parse(&c, text))
        return false;
    clip_bind(&c, &f->m);
    for (int i = 0; i < L->nclips; i++)
        if (!strcmp(L->clips[i].name, c.name)) {
            if (L->act == i)
                L->act = -1;
            clip_free(&L->clips[i]);
            L->clips[i] = c;
            return true;
        }
    if (L->nclips >= LIFE_MAX_CLIPS) {
        clip_free(&c);
        return false;
    }
    L->clips[L->nclips++] = c;
    return true;
}

bool life_act(life *L, const char *name) {
    for (int i = 0; i < L->nclips; i++)
        if (!strcmp(L->clips[i].name, name)) {
            if (L->act >= 0)
                L->act_queue = i;
            else
                L->act = i, L->act_start = -1; /* from the next frame */
            return true;
        }
    return false;
}

bool life_busy(const life *L) {
    return L->speaking || L->act >= 0 || L->speak > 0.02f || L->t - L->blink_start < 0.4 || L->t - L->mood_since < 1.0;
}

/* a slow wander, the sum of a few unrelated waves */
static float drift(double t, float a, float b, float c) {
    return 0.5f * sinf((float)(t * a)) + 0.3f * sinf((float)(t * b + 1.7)) + 0.2f * sinf((float)(t * c + 4.1));
}

static void pick_viseme(life *L) {
    /* the first band (low) is strong for open vowels, the second (high) for
     * spread ones; neither much: rounded u */
    float f1 = L->f1, f2 = L->f2;
    if (f1 <= 0 && f2 <= 0) { /* no bands: a plausible round of shapes */
        static const int cycle[] = {FM_A, FM_O, FM_E, FM_A, FM_I, FM_U, FM_A, FM_E};
        L->viseme = cycle[(int)(L->t * 7) % 8];
        return;
    }
    float sum = f1 + f2 + 1e-4f, r = f1 / sum;
    if (sum < 0.15f)
        L->viseme = FM_U;
    else if (r > 0.68f)
        L->viseme = f1 > 0.6f ? FM_A : FM_O;
    else if (r < 0.38f)
        L->viseme = FM_I;
    else
        L->viseme = r > 0.52f ? FM_A : FM_E;
}

void life_frame(life *L, const figure *f, double now, fpose *pose, face_state *fs) {
    float dt = L->t > 0 ? (float)clampf((float)(now - L->t), 0, 0.1f) : 0;
    L->t = now;
    const body_info *bi = &f->body;
    const int *B = bi->bone;
    fig_pose_rest(&f->m, pose);
    body_pose_relaxed(bi, pose);
    if (L->mood != L->last_mood)
        L->last_mood = L->mood, L->mood_since = now;
    float en = 0.6f + 0.8f * L->energy;
    bool sleepy = L->mood == MOOD_SLEEPY || L->dozing;

    /* breathing: chest and shoulders rise and fall */
    float rate = L->dozing ? 0.17f : sleepy ? 0.2f : 0.25f + 0.05f * L->energy;
    float br = sinf((float)(now * 2 * PI_F * rate));
    pose->local[B[BN_CHEST]] = qeuler(-0.014f * br, 0, 0);
    pose->local[B[BN_UPPERCHEST]] = qeuler(-0.01f * br, 0, 0);
    pose->local[B[BN_LSHOULDER]] = qmul(qaxis(V3(0, 0, 1), 0.012f * br), pose->local[B[BN_LSHOULDER]]);
    pose->local[B[BN_RSHOULDER]] = qmul(qaxis(V3(0, 0, 1), -0.012f * br), pose->local[B[BN_RSHOULDER]]);
    /* sway */
    float s1 = drift(now, 0.43, 0.27, 0.61), s2 = drift(now + 30, 0.31, 0.53, 0.19), s3 = drift(now + 70, 0.23, 0.41, 0.67);
    pose->local[B[BN_SPINE]] = qeuler(0.012f * s1 * en, 0.02f * s2 * en, 0.016f * s3 * en);

    /* where the head looks */
    float ty, tp, roll = 0.03f * s3;
    if (L->dozing)
        ty = 0.05f * s2, tp = 0.28f, roll = 0.1f;
    else if (L->thinking)
        ty = 0.14f, tp = -0.1f, roll = 0.06f;
    else if (L->look_on)
        ty = clampf(L->look_x, -1, 1) * 0.42f, tp = -clampf(L->look_y, -1, 1) * 0.24f;
    else
        ty = 0.14f * drift(now + 11, 0.21, 0.13, 0.37), tp = 0.05f * drift(now + 5, 0.17, 0.29, 0.11);
    if (L->mood == MOOD_CURIOUS)
        roll += 0.16f;
    else if (L->mood == MOOD_PROUD)
        tp -= 0.08f;
    else if (L->mood == MOOD_SAD)
        tp += 0.1f;
    L->hx = easef(L->hx, ty, L->look_on ? 5.0f : 2.5f, dt);
    L->hy = easef(L->hy, tp, L->look_on ? 5.0f : 2.5f, dt);
    pose->local[B[BN_NECK]] = qeuler(L->hy * 0.4f, L->hx * 0.4f, roll * 0.4f);
    pose->local[B[BN_HEAD]] = qeuler(L->hy * 0.6f, L->hx * 0.6f, roll * 0.6f);

    /* the eyes: what the head hasn't turned to, plus little jumps */
    if (now >= L->next_saccade) {
        L->next_saccade = now + 0.6 + rnd(L) * (L->look_on ? 1.6f : 2.6f);
        float jx = (rnd(L) - 0.5f) * 0.35f, jy = (rnd(L) - 0.5f) * 0.25f;
        if (L->look_on)
            L->gtx = clampf((L->look_x * 0.42f - L->hx) * 2.4f + jx * 0.3f, -1, 1),
            L->gty = clampf((L->look_y * 0.24f + L->hy) * 2.4f + jy * 0.3f, -1, 1);
        else
            L->gtx = jx * 2, L->gty = jy * 1.5f;
    }
    float gtx = L->gtx, gty = L->gty;
    if (L->thinking)
        gtx = 0.45f, gty = 0.6f;
    else if (L->dozing)
        gtx = gty = 0;
    L->gx = easef(L->gx, gtx, 28, dt);
    L->gy = easef(L->gy, gty, 28, dt);

    /* the face for her mood */
    float since = (float)(now - L->mood_since);
    fs->blush = 0;
    switch (L->mood) {
    case MOOD_HAPPY:
        fs->eyes = since < 1.4f ? FE_HAPPY : FE_OPEN, fs->brows = FB_RAISED, fs->mouth = since < 2.0f ? FM_OPEN_SMILE : FM_SMILE;
        fs->blush = 0.15f;
        break;
    case MOOD_SAD: fs->eyes = FE_SAD, fs->brows = FB_WORRY, fs->mouth = FM_FROWN; break;
    case MOOD_EXCITED:
        fs->eyes = since < 2.0f ? FE_SPARKLE : FE_WIDE, fs->brows = FB_RAISED, fs->mouth = FM_OPEN_SMILE, fs->blush = 0.3f;
        break;
    case MOOD_SLEEPY: fs->eyes = FE_HALF, fs->brows = FB_NEUTRAL, fs->mouth = FM_CLOSED; break;
    case MOOD_CURIOUS: fs->eyes = FE_OPEN, fs->brows = FB_RAISED, fs->mouth = since < 1.2f ? FM_SMALL_O : FM_CLOSED; break;
    case MOOD_PROUD: fs->eyes = FE_HAPPY, fs->brows = FB_NEUTRAL, fs->mouth = FM_SMILE, fs->blush = 0.1f; break;
    case MOOD_WORRIED: fs->eyes = FE_OPEN, fs->brows = FB_WORRY, fs->mouth = FM_CLOSED; break;
    default: fs->eyes = FE_OPEN, fs->brows = FB_NEUTRAL, fs->mouth = FM_SMILE; break;
    }
    if (L->thinking)
        fs->brows = FB_RAISED, fs->mouth = FM_CLOSED;
    if (L->dozing)
        fs->eyes = FE_CLOSED, fs->mouth = FM_CLOSED;

    /* blinking */
    if (now >= L->next_blink) {
        L->blink_start = now;
        L->double_blink = rnd(L) < 0.14f;
        L->next_blink = now + (sleepy ? 1.6 + rnd(L) * 2.5 : 2.2 + rnd(L) * 4.2);
    }
    float bp = (float)(now - L->blink_start), slow = sleepy ? 1.8f : 1;
    if (L->double_blink && bp > 0.26f * slow)
        bp -= 0.26f * slow;
    bp /= slow;
    if (fs->eyes == FE_OPEN || fs->eyes == FE_WIDE || fs->eyes == FE_SAD || fs->eyes == FE_SPARKLE || fs->eyes == FE_HALF) {
        if (bp < 0.035f)
            fs->eyes = FE_HALF;
        else if (bp < 0.11f)
            fs->eyes = FE_CLOSED;
        else if (bp < 0.17f)
            fs->eyes = FE_HALF;
    }

    /* speaking */
    if (L->speak > 0.05f) {
        if (now >= L->viseme_until) {
            pick_viseme(L);
            L->viseme_until = now + 0.075;
        }
        L->mouth_open = easef(L->mouth_open, clampf(L->speak * 1.3f, 0.2f, 1), 30, dt);
        fs->mouth = L->viseme;
        fs->open = L->mouth_open;
    } else {
        L->mouth_open = easef(L->mouth_open, 0, 20, dt);
        if (L->mouth_open > 0.12f) {
            fs->mouth = L->viseme;
            fs->open = L->mouth_open;
        } else
            fs->open = 0;
    }
    /* the gaze, in steps fine enough not to see but coarse enough not to
     * redraw her face every frame */
    fs->gx = roundf(L->gx * 25) / 25, fs->gy = roundf(L->gy * 25) / 25;
    fs->open = roundf(fs->open * 12) / 12;

    /* gestures over all of it */
    if (L->act >= 0 && L->act_start < 0)
        L->act_start = now;
    if (L->act >= 0) {
        const clip *c = &L->clips[L->act];
        float t = (float)(now - L->act_start), len = clip_length(c);
        float w = smoothf(0, 0.18f, t) * (1 - smoothf(len - 0.22f, len, t));
        clip_apply(c, t, pose, w);
        if (t >= len) {
            L->act = L->act_queue, L->act_queue = -1;
            L->act_start = -1;
        }
    } else if (L->think_clip >= 0) {
        if (L->thinking && L->think_since <= 0)
            L->think_since = now;
        if (!L->thinking && L->think_since > 0 && now - L->think_since > 0.4)
            L->think_since = -now; /* letting go: remember when */
        if (L->think_since > 0) {
            float t = (float)(now - L->think_since);
            clip_apply(&L->clips[L->think_clip], fminq(t, 1.9f), pose, smoothf(0, 0.5f, t));
        } else if (L->think_since < 0) {
            float t = (float)(now + L->think_since);
            float w = 1 - smoothf(0, 0.5f, t);
            if (w > 0)
                clip_apply(&L->clips[L->think_clip], 1.9f, pose, w);
            else
                L->think_since = 0;
        }
    }
    fig_pose_update(&f->m, pose);
}

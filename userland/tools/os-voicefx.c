/* os-voicefx: the companion's voice, shaped on its way to the speaker.
 *
 *   os-voicefx RATE [pitch=N] [formant=N] [warmth=W] [breath=B] [effect=NAME]
 *
 * Reads signed 16-bit little-endian mono PCM on stdin, writes the same on stdout, as it comes (a sentence starts
 * playing while the next is made). Everything is computed here, on this computer:
 *   pitch     semitones, -12..12: higher or lower, at the same speed, the voice's body kept (formants preserved)
 *   formant   semitones, -6..6: the body alone: lower is bigger and deeper, higher is smaller and younger
 *   warmth    -1..1: warmer (more low, softer top) or brighter
 *   breath    0..1: air in the voice
 *   effect    none, robot, whisper, radio, phone, room, hall
 * Pitch, formants, robot and whisper are done on the spectrum (a phase vocoder with the spectral envelope kept
 * apart from the harmonics); the rest are filters, a little distortion and a reverb. */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define N 1024     /* frame */
#define HOP (N / 4) /* 75% overlap */
#define LIFTER 40   /* cepstral coefficients kept for the envelope */

static float rate = 24000;
static float pitch_r = 1, formant_r = 1, warmth, breath;
static enum { FX_NONE, FX_ROBOT, FX_WHISPER, FX_RADIO, FX_PHONE, FX_ROOM, FX_HALL } effect;

/* ---- FFT (complex, radix 2, in place) ---- */

static void fft(float *re, float *im, int n, int inverse) {
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j], re[j] = t;
            t = im[i], im[i] = im[j], im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = 2 * (float)M_PI / (float)len * (inverse ? 1 : -1);
        float wr = cosf(ang), wi = sinf(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr, im[b] = im[a] - ti;
                re[a] += tr, im[a] += ti;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr, cr = nr;
            }
        }
    }
    if (inverse)
        for (int i = 0; i < n; i++)
            re[i] /= (float)n, im[i] /= (float)n;
}

/* ---- the spectral stage: pitch, formants, robot, whisper ---- */

static float win[N], inbuf[N], olabuf[2 * N];
static float last_phase[N / 2 + 1], sum_phase[N / 2 + 1];
static int filled; /* samples in inbuf beyond the last frame */
static uint32_t rnd = 2463534242u;
static float frand(void) { /* 0..1 */
    rnd ^= rnd << 13, rnd ^= rnd >> 17, rnd ^= rnd << 5;
    return (float)(rnd & 0xffffff) / 16777216.0f;
}

static bool spectral(void) {
    return fabsf(pitch_r - 1) > 1e-3f || fabsf(formant_r - 1) > 1e-3f || effect == FX_ROBOT || effect == FX_WHISPER;
}

/* the spectral envelope of a magnitude spectrum: its log, smoothed through the cepstrum */
static void envelope(const float *mag, float *env) {
    static float re[N], im[N];
    for (int k = 0; k < N; k++) {
        int b = k <= N / 2 ? k : N - k;
        re[k] = logf(mag[b] + 1e-6f), im[k] = 0;
    }
    fft(re, im, N, 1);
    for (int q = LIFTER; q <= N - LIFTER; q++)
        re[q] = im[q] = 0;
    fft(re, im, N, 0);
    for (int k = 0; k <= N / 2; k++)
        env[k] = expf(re[k]);
}

static float lerp_at(const float *a, float x, int n) {
    if (x <= 0)
        return a[0];
    if (x >= n - 1)
        return a[n - 1];
    int i = (int)x;
    float f = x - (float)i;
    return a[i] * (1 - f) + a[i + 1] * f;
}

static void frame(float *out_hop) {
    static float re[N], im[N], mag[N / 2 + 1], freq[N / 2 + 1], env[N / 2 + 1], omag[N / 2 + 1], ofreq[N / 2 + 1];
    for (int i = 0; i < N; i++)
        re[i] = inbuf[i] * win[i], im[i] = 0;
    fft(re, im, N, 0);
    const float expct = 2 * (float)M_PI * HOP / N;
    for (int k = 0; k <= N / 2; k++) {
        mag[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);
        float ph = atan2f(im[k], re[k]);
        float d = ph - last_phase[k] - (float)k * expct;
        last_phase[k] = ph;
        d -= 2 * (float)M_PI * floorf(d / (2 * (float)M_PI) + 0.5f); /* wrapped to -pi..pi */
        freq[k] = (float)k + d / expct;                              /* its true frequency, in bins */
    }
    envelope(mag, env);
    memset(omag, 0, sizeof omag);
    for (int k = 0; k <= N / 2; k++)
        ofreq[k] = (float)k;
    /* the harmonics move by the pitch; the envelope (the voice's body) is put back where the formants want it */
    for (int k = 0; k <= N / 2; k++) {
        int j = (int)lrintf((float)k * pitch_r);
        if (j < 0 || j > N / 2)
            continue;
        float e = mag[k] / (env[k] + 1e-9f);
        if (e > omag[j]) { /* the stronger partial wins the bin */
            omag[j] = e;
            ofreq[j] = freq[k] * pitch_r;
        }
    }
    for (int j = 0; j <= N / 2; j++)
        omag[j] *= lerp_at(env, (float)j / formant_r, N / 2 + 1);
    for (int j = 0; j <= N / 2; j++) {
        if (effect == FX_ROBOT)
            sum_phase[j] = 0; /* every frame starts in phase: one steady buzz, at the frame rate */
        else if (effect == FX_WHISPER)
            sum_phase[j] = 2 * (float)M_PI * frand(); /* no pitch at all: breath shaped like the words */
        else
            sum_phase[j] = fmodf(sum_phase[j] + ofreq[j] * expct, 2 * (float)M_PI);
        re[j] = omag[j] * cosf(sum_phase[j]), im[j] = omag[j] * sinf(sum_phase[j]);
        if (j > 0 && j < N / 2)
            re[N - j] = re[j], im[N - j] = -im[j];
    }
    fft(re, im, N, 1);
    /* Hann at both ends, 75% overlap: the windows add up to 1.5 */
    for (int i = 0; i < N; i++)
        olabuf[i] += re[i] * win[i] / 1.5f;
    memcpy(out_hop, olabuf, sizeof(float) * HOP);
    memmove(olabuf, olabuf + HOP, sizeof(float) * (2 * N - HOP));
    memset(olabuf + 2 * N - HOP, 0, sizeof(float) * HOP);
}

/* ---- filters ---- */

typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} biquad;

static float bq(biquad *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

/* RBJ's cookbook: kind 'l' low-pass, 'h' high-pass, 'L' low shelf, 'H' high shelf */
static biquad make(char kind, float f0, float q, float gain_db) {
    float A = powf(10, gain_db / 40), w = 2 * (float)M_PI * f0 / rate, c = cosf(w), s = sinf(w);
    float al = s / (2 * q), b0, b1, b2, a0, a1, a2;
    if (kind == 'l' || kind == 'h') {
        float sg = kind == 'l' ? 1 : -1;
        b0 = (1 - sg * c) / 2, b1 = sg * (1 - sg * c), b2 = b0;
        if (kind == 'h')
            b0 = (1 + c) / 2, b1 = -(1 + c), b2 = b0;
        a0 = 1 + al, a1 = -2 * c, a2 = 1 - al;
    } else {
        float sq = 2 * sqrtf(A) * al, sg = kind == 'L' ? 1 : -1;
        b0 = A * ((A + 1) - sg * (A - 1) * c + sq);
        b1 = sg * 2 * A * ((A - 1) - sg * (A + 1) * c);
        b2 = A * ((A + 1) - sg * (A - 1) * c - sq);
        a0 = (A + 1) + sg * (A - 1) * c + sq;
        a1 = -sg * 2 * ((A - 1) + sg * (A + 1) * c);
        a2 = (A + 1) + sg * (A - 1) * c - sq;
    }
    return (biquad){b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0, 0, 0};
}

static biquad lo_shelf, hi_shelf, air_hp, band[4];
static float env_follow;

/* a small reverb: four combs into two all-passes (after Schroeder and Freeverb) */
#define NCOMB 4
static float *comb[NCOMB], comb_store[NCOMB], *ap[2];
static int comb_len[NCOMB], comb_pos[NCOMB], ap_len[2], ap_pos[2];
static float fb, damp, wet;

static void reverb_init(bool hall) {
    static const int c44[NCOMB] = {1116, 1188, 1277, 1356}, a44[2] = {556, 441};
    float scale = rate / 44100.0f * (hall ? 1.6f : 0.75f);
    for (int i = 0; i < NCOMB; i++) {
        comb_len[i] = (int)((float)c44[i] * scale);
        comb[i] = calloc((size_t)comb_len[i], sizeof(float));
    }
    for (int i = 0; i < 2; i++) {
        ap_len[i] = (int)((float)a44[i] * rate / 44100.0f);
        ap[i] = calloc((size_t)ap_len[i], sizeof(float));
    }
    fb = hall ? 0.86f : 0.72f, damp = hall ? 0.25f : 0.4f, wet = hall ? 0.38f : 0.22f;
}

static float reverb(float x) {
    float out = 0;
    for (int i = 0; i < NCOMB; i++) {
        float y = comb[i][comb_pos[i]];
        comb_store[i] = y * (1 - damp) + comb_store[i] * damp;
        comb[i][comb_pos[i]] = x * 0.25f + comb_store[i] * fb;
        comb_pos[i] = (comb_pos[i] + 1) % comb_len[i];
        out += y;
    }
    for (int i = 0; i < 2; i++) {
        float b = ap[i][ap_pos[i]];
        ap[i][ap_pos[i]] = out + b * 0.5f;
        out = b - out;
        ap_pos[i] = (ap_pos[i] + 1) % ap_len[i];
    }
    return x * (1 - wet * 0.5f) + out * wet;
}

static float held; /* the phone's lower sample rate */
static int hold_n;

static float post(float x) {
    if (warmth != 0) {
        x = bq(&lo_shelf, x);
        x = bq(&hi_shelf, x);
    }
    if (breath > 0) { /* air: noise above 2 kHz, following how loud the voice is */
        float a = fabsf(x);
        env_follow += (a - env_follow) * (a > env_follow ? 0.02f : 0.002f);
        float n = bq(&air_hp, frand() * 2 - 1);
        x = x * (1 - breath * 0.25f) + n * env_follow * breath * 1.6f;
    }
    switch (effect) {
    case FX_RADIO:
        x = bq(&band[1], bq(&band[0], x));
        x = tanhf(x * 2.6f) / tanhf(2.6f);
        x += (frand() - 0.5f) * 0.01f + (frand() < 0.0004f ? (frand() - 0.5f) * 0.3f : 0); /* hiss and crackle */
        break;
    case FX_PHONE:
        if (hold_n++ % 3 == 0) /* about 8 kHz */
            held = x;
        x = bq(&band[1], bq(&band[0], held));
        x = (x < 0 ? -1 : 1) * powf(fabsf(x), 0.75f) * 0.9f; /* squeezed, as a phone line does */
        break;
    case FX_ROOM:
    case FX_HALL:
        x = reverb(x);
        break;
    default:
        break;
    }
    return x;
}

static void out_samples(const float *x, int n) {
    static int16_t o[HOP * 4];
    for (int i = 0; i < n; i++) {
        float v = post(x[i]);
        if (fabsf(v) > 0.9f) /* a soft knee instead of a hard edge */
            v = (v > 0 ? 1 : -1) * (0.9f + 0.1f * tanhf((fabsf(v) - 0.9f) / 0.1f));
        if (v > 1)
            v = 1;
        if (v < -1)
            v = -1;
        o[i] = (int16_t)lrintf(v * 32767);
    }
    size_t want = sizeof(int16_t) * (size_t)n, done = 0;
    while (done < want) {
        ssize_t w = write(1, (char *)o + done, want - done);
        if (w <= 0)
            exit(0); /* the player is gone: nothing to say to */
        done += (size_t)w;
    }
}

/* How loud the voice was going in and comes out of the spectral stage, slowly: the stage can make it quieter (a
 * robot's frames all start in phase; partials that land on one bin are not summed), so it is brought back to the
 * level it came in at. Over about half a second, never more than 6x, so a pause is never blown up. */
static float pow_in = 1e-6f, pow_out = 1e-6f, gain = 1;

static void level(float *hop, int n) {
    float po = 0;
    for (int i = 0; i < n; i++)
        po += hop[i] * hop[i];
    pow_out += (po / (float)n - pow_out) * 0.05f;
    if (pow_in > 1e-5f && pow_out > 1e-7f) {
        float want = sqrtf(pow_in / pow_out);
        want = want < 0.5f ? 0.5f : want > 6 ? 6 : want;
        gain += (want - gain) * 0.1f;
    }
    for (int i = 0; i < n; i++)
        hop[i] *= gain;
}

/* one sample of input */
static void in_sample(float x) {
    if (!spectral()) {
        out_samples(&x, 1);
        return;
    }
    pow_in += (x * x - pow_in) * (0.05f / HOP);
    inbuf[N - HOP + filled] = x;
    if (++filled == HOP) {
        float hop[HOP];
        frame(hop);
        level(hop, HOP);
        out_samples(hop, HOP);
        memmove(inbuf, inbuf + HOP, sizeof(float) * (N - HOP));
        filled = 0;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: os-voicefx RATE [pitch=N] [formant=N] [warmth=W] [breath=B] [effect=NAME]\n");
        return 2;
    }
    rate = (float)atof(argv[1]);
    if (rate < 8000 || rate > 96000)
        rate = 24000;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        float v = (float)atof(strchr(a, '=') ? strchr(a, '=') + 1 : "0");
        if (!strncmp(a, "pitch=", 6))
            pitch_r = powf(2, (v < -12 ? -12 : v > 12 ? 12 : v) / 12);
        else if (!strncmp(a, "formant=", 8))
            formant_r = powf(2, (v < -6 ? -6 : v > 6 ? 6 : v) / 12);
        else if (!strncmp(a, "warmth=", 7))
            warmth = v < -1 ? -1 : v > 1 ? 1 : v;
        else if (!strncmp(a, "breath=", 7))
            breath = v < 0 ? 0 : v > 1 ? 1 : v;
        else if (!strncmp(a, "effect=", 7)) {
            const char *e = a + 7;
            effect = !strcmp(e, "robot")     ? FX_ROBOT
                     : !strcmp(e, "whisper") ? FX_WHISPER
                     : !strcmp(e, "radio")   ? FX_RADIO
                     : !strcmp(e, "phone")   ? FX_PHONE
                     : !strcmp(e, "room")    ? FX_ROOM
                     : !strcmp(e, "hall")    ? FX_HALL
                                             : FX_NONE;
        }
    }
    for (int i = 0; i < N; i++)
        win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * (float)i / N);
    lo_shelf = make('L', 250, 0.7f, 7 * warmth);
    hi_shelf = make('H', 3800, 0.7f, -7 * warmth);
    air_hp = make('h', 2200, 0.7f, 0);
    if (effect == FX_RADIO)
        band[0] = make('h', 450, 0.8f, 0), band[1] = make('l', 3200, 0.8f, 0);
    if (effect == FX_PHONE)
        band[0] = make('h', 320, 0.8f, 0), band[1] = make('l', 3300, 0.8f, 0);
    if (effect == FX_ROOM || effect == FX_HALL)
        reverb_init(effect == FX_HALL);
    /* the stage starts full of silence, so the first words come out after one frame, never cut */
    static int16_t buf[4096];
    ssize_t n;
    size_t carry = 0;
    while ((n = read(0, (char *)buf + carry, sizeof buf - carry)) > 0) {
        size_t have = carry + (size_t)n, whole = have / 2;
        for (size_t i = 0; i < whole; i++)
            in_sample((float)buf[i] / 32768.0f);
        carry = have % 2;
        if (carry)
            memmove(buf, (char *)buf + whole * 2, carry);
    }
    /* the end: the last frames, and the reverb's tail */
    int tail = spectral() ? N : 0;
    tail += effect == FX_HALL ? (int)(rate * 1.2f) : effect == FX_ROOM ? (int)(rate * 0.4f) : 0;
    for (int i = 0; i < tail; i++)
        in_sample(0);
    return 0;
}

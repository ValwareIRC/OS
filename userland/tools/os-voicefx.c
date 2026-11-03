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
 *   range     0.3..2: how far its melody moves: under 1 flatter (calm, sad, sleepy), over 1 livelier (excited)
 *   gain      0.2..2: louder or softer (a whisper is softer)
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
static float pitch_r = 1, formant_r = 1, warmth, breath, range = 1, gain_set = 1;
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

static bool psola_on(void);
static bool spectral(void) {
    return effect == FX_WHISPER; /* (pitch, formants and the robot are PSOLA's, below; a whisper comes after them) */
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
    x *= gain_set;
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
    while (n > HOP * 4) { /* (a whole phrase at a time from PSOLA: in pieces) */
        out_samples(x, HOP * 4);
        x += HOP * 4, n -= HOP * 4;
    }
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

/* ---- pitch and formants for speech: resampling and PSOLA ----
 * (Done first on the spectrum, by the phase vocoder above: it smeared the voice's phases and gave it a metallic,
 * "Dalek" edge. This is the way made for speech: the formants are moved by resampling, which shifts pitch, formants
 * and length together and nothing else; then pitch-synchronous overlap-add (PSOLA) takes the voice apart into
 * grains of two pitch periods each, at its own glottal pulses, and puts them back closer together or further apart
 * for the pitch it should have, at the length it had. Each grain is a real piece of the voice, so it keeps its
 * texture; the robot is the same grains at one steady pitch.) It works on whole phrases, which Kokoro sends
 * separated by silence, so nothing waits longer than the phrase already being made. */

static bool psola_on(void) {
    return fabsf(pitch_r - 1) > 1e-3f || fabsf(formant_r - 1) > 1e-3f || fabsf(range - 1) > 1e-3f || effect == FX_ROBOT;
}

/* in, at position x (fractional), through a windowed sinc low-passed at `cut` (0..1 of Nyquist) */
static float sinc_at(const float *in, int n, double x, float cut) {
    const int HALF = 12;
    int c = (int)floor(x);
    float acc = 0, wsum = 0;
    for (int i = c - HALF + 1; i <= c + HALF; i++) {
        double d = x - i;
        float w = 0.5f + 0.5f * cosf((float)(M_PI * d / HALF)); /* Hann over the taps */
        float s = fabs(d) < 1e-9 ? cut : sinf((float)(M_PI * d * cut)) / (float)(M_PI * d);
        float v = i >= 0 && i < n ? in[i] : 0;
        acc += v * s * w, wsum += s * w;
    }
    return wsum != 0 ? acc / wsum : acc; /* (a gain of 1 for a steady level) */
}

/* the period (in samples) of each 5 ms of y, 0 where it isn't voiced: normalised autocorrelation, then a median */
static int *pitch_track(const float *y, int m, int hop, int *nf) {
    int W = (int)(rate * 0.032f), lmin = (int)(rate / 500), lmax = (int)(rate / 65);
    int frames = m / hop + 1;
    int *lag = calloc((size_t)frames, sizeof(int)), *sm = calloc((size_t)frames, sizeof(int));
    for (int f = 0; f < frames; f++) {
        int s = f * hop - W / 2;
        float e0 = 0;
        for (int i = 0; i < W; i++) {
            int k = s + i;
            float v = k >= 0 && k < m ? y[k] : 0;
            e0 += v * v;
        }
        if (e0 / (float)W < 1e-5f) /* quiet: no pitch */
            continue;
        float best = 0;
        int bl = 0;
        for (int L = lmin; L <= lmax; L++) {
            float num = 0, e1 = 0, e2 = 0;
            for (int i = 0; i < W - L; i += 2) {
                int a = s + i, b = s + i + L;
                float va = a >= 0 && a < m ? y[a] : 0, vb = b >= 0 && b < m ? y[b] : 0;
                num += va * vb, e1 += va * va, e2 += vb * vb;
            }
            float r = e1 > 0 && e2 > 0 ? num / sqrtf(e1 * e2) : 0;
            if (r > best + 0.01f) /* the first strong peak wins over its multiples */
                best = r, bl = L;
        }
        lag[f] = best > 0.55f ? bl : 0;
    }
    for (int f = 0; f < frames; f++) { /* a median of five: single wrong frames go */
        int v[5], k = 0;
        for (int d = -2; d <= 2; d++)
            if (f + d >= 0 && f + d < frames)
                v[k++] = lag[f + d];
        for (int i = 0; i < k; i++)
            for (int j = i + 1; j < k; j++)
                if (v[j] < v[i]) {
                    int t = v[i];
                    v[i] = v[j], v[j] = t;
                }
        sm[f] = v[k / 2];
    }
    free(lag);
    *nf = frames;
    return sm;
}

static float *psola(const float *in, int n, int *outn) {
    /* 1. the formants: resampled by formant_r (pitch and length move with them; both are put back below) */
    float rf = formant_r;
    int m = (int)((float)n / rf);
    float *y = malloc(sizeof(float) * (size_t)(m > 0 ? m : 1));
    float cut = rf > 1 ? 1 / rf : 1;
    for (int i = 0; i < m; i++)
        y[i] = sinc_at(in, n, (double)i * rf, cut);
    /* 2. its pitch, and its glottal pulses: a mark every period, at the waveform's peak */
    int hop = (int)(rate * 0.005f), nf;
    int *lag = pitch_track(y, m, hop, &nf);
    int cap = m / 20 + 16, nm = 0;
    int *mk = malloc(sizeof(int) * (size_t)cap), *mt = malloc(sizeof(int) * (size_t)cap);
    bool *mv = malloc(sizeof(bool) * (size_t)cap);
    long long voiced_sum = 0;
    int voiced_n = 0;
    for (int pos = 0; pos < m;) {
        int f = pos / hop < nf ? pos / hop : nf - 1, T = lag[f];
        bool v = T > 0;
        if (!v)
            T = hop;
        else
            voiced_sum += T, voiced_n++;
        if (nm == cap) {
            cap *= 2;
            mk = realloc(mk, sizeof(int) * (size_t)cap), mt = realloc(mt, sizeof(int) * (size_t)cap);
            mv = realloc(mv, sizeof(bool) * (size_t)cap);
        }
        int at = pos;
        if (v) { /* on the pulse: the highest point near where the period says */
            int lo = pos - T / 4, hi = pos + T / 4, b = pos;
            for (int i = lo < 0 ? 0 : lo; i <= hi && i < m; i++)
                if (y[i] > y[b < m ? b : m - 1])
                    b = i;
            at = b;
        }
        mk[nm] = at, mt[nm] = T, mv[nm] = v, nm++;
        pos = at + T > pos ? at + T : pos + 1;
    }
    /* 3. put back together: at the pitch asked for, at the length it had */
    float rp = pitch_r / rf; /* the resampling already moved the pitch by rf */
    int robot_T = voiced_n ? (int)((float)(voiced_sum / voiced_n) / pitch_r) : (int)(rate / 140);
    int L = n;
    float *out = calloc((size_t)L + 1, sizeof(float)), *ws = calloc((size_t)L + 1, sizeof(float));
    int j = 0;
    for (float t = 0; t < (float)L && nm;) {
        float yt = t / rf; /* where in y this moment of the output comes from */
        while (j + 1 < nm && fabsf((float)mk[j + 1] - yt) <= fabsf((float)mk[j] - yt))
            j++;
        int Ta = mt[j], c = mk[j];
        /* its melody, widened or flattened around the phrase's own middle: each pulse's pitch relative to the mean,
         * raised to `range` (1 keeps it as spoken) */
        float rl = rp;
        if (mv[j] && voiced_n && range != 1) {
            float rel = (float)(voiced_sum / voiced_n) / (float)Ta; /* this pulse's pitch over the mean */
            rl = rp * powf(rel, range - 1);
            rl = rl < rp * 0.6f ? rp * 0.6f : rl > rp * 1.7f ? rp * 1.7f : rl;
        }
        float Ts = mv[j] ? (effect == FX_ROBOT ? (float)robot_T : (float)Ta / rl) : (float)Ta;
        int tc = (int)lrintf(t);
        for (int i = -Ta; i <= Ta; i++) {
            int k = c + i, o = tc + i;
            if (k < 0 || k >= m || o < 0 || o >= L)
                continue;
            float w = 0.5f + 0.5f * cosf((float)M_PI * (float)i / (float)(Ta + 1));
            out[o] += y[k] * w, ws[o] += w;
        }
        t += Ts < 1 ? 1 : Ts;
    }
    for (int i = 0; i < L; i++) /* the grains' windows add up unevenly when re-spaced: evened out */
        out[i] = ws[i] > 0.25f ? out[i] / ws[i] : out[i] * 4 * (ws[i] > 0 ? 1 : 0);
    free(y), free(lag), free(mk), free(mt), free(mv), free(ws);
    *outn = L;
    return out;
}

static void level(float *hop, int n);
static float pow_in;
/* the whisper's stage, a sample at a time */
static void spectral_push(float x) {
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

static void phrase_out(const float *x, int n) {
    if (spectral()) /* pitched first, then whispered */
        for (int i = 0; i < n; i++)
            spectral_push(x[i]);
    else
        out_samples(x, n);
}

/* phrases: gathered until a pause, then done whole */
static float *chunk;
static int chunk_n, chunk_cap, quiet_run;
static bool heard;

static void chunk_flush(void) {
    if (!chunk_n)
        return;
    if (heard) {
        int on;
        float *o = psola(chunk, chunk_n, &on);
        phrase_out(o, on);
        free(o);
    } else {
        phrase_out(chunk, chunk_n); /* silence stays silence, at once */
    }
    chunk_n = 0, quiet_run = 0, heard = false;
}

static void chunk_add(float x) {
    if (chunk_n == chunk_cap) {
        chunk_cap = chunk_cap ? chunk_cap * 2 : 1 << 16;
        chunk = realloc(chunk, sizeof(float) * (size_t)chunk_cap);
    }
    chunk[chunk_n++] = x;
    if (fabsf(x) < 0.003f)
        quiet_run++;
    else
        quiet_run = 0, heard = true;
    if ((heard && quiet_run > (int)(rate * 0.12f)) || chunk_n > (int)(rate * 20) ||
        (!heard && chunk_n > (int)(rate * 0.05f)))
        chunk_flush();
}

/* one sample of input */
static void in_sample(float x) {
    if (psola_on()) {
        chunk_add(x);
        return;
    }
    phrase_out(&x, 1);
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
        else if (!strncmp(a, "range=", 6))
            range = v < 0.3f ? 0.3f : v > 2 ? 2 : v;
        else if (!strncmp(a, "gain=", 5))
            gain_set = v < 0.2f ? 0.2f : v > 2 ? 2 : v;
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
    /* the end: the last phrase, the last frames, and the reverb's tail */
    if (psola_on())
        chunk_flush();
    if (spectral())
        for (int i = 0; i < N; i++)
            spectral_push(0);
    int tail = effect == FX_HALL ? (int)(rate * 1.2f) : effect == FX_ROOM ? (int)(rate * 0.4f) : 0;
    static float zero[HOP];
    for (int i = 0; i < tail; i += HOP)
        out_samples(zero, HOP);
    return 0;
}

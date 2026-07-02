/* audio.c — procedural one-shot SFX over miniaudio, or a silent stub.
 * See audio.h for the contract and the backend-selection rules.
 */
#include "audio.h"

#ifndef HAVE_MINIAUDIO

/* ---- null backend: the game runs silent, zero deps ------------------- */
int  au_init(void){ return 0; }
void au_shutdown(void){}
void au_play(AuSound id){ (void)id; }
void au_set_volume(float sfx, float music){ (void)sfx; (void)music; }
int  au_backend_live(void){ return 0; }

#else /* HAVE_MINIAUDIO ------------------------------------------------- */

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_DECODING          /* PCM only: no file decoders needed      */
#define MA_NO_ENCODING
#include "vat/miniaudio.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

#define AU_RATE   48000
#define AU_VOICES 16

typedef struct { float *pcm; int len; } AuBuf;     /* mono PCM            */
typedef struct { int snd; volatile int pos; int live; } AuVoice;

static AuBuf   gSnd[SND_COUNT];
static AuVoice gVoice[AU_VOICES];
static ma_device gDev;
static int   gLive = 0;
static float gVolSfx = 0.7f, gVolMusic = 0.5f;   /* music reserved (v1)   */

/* ---- tiny synth helpers (init-time only) ------------------------------ */

static float *au_alloc(int len){ return (float*)calloc((size_t)len, sizeof(float)); }

/* add a tone: sine morphed toward square by `edge`, exp decay, from t0. */
static void tone(AuBuf *b, float t0, float dur, float hz, float amp,
                 float edge, float decay){
    int i0 = (int)(t0 * AU_RATE), n = (int)(dur * AU_RATE);
    for (int i = 0; i < n && i0 + i < b->len; i++){
        float t = (float)i / AU_RATE;
        float s = sinf(6.2831853f * hz * t);
        s = (1.0f - edge) * s + edge * (s >= 0.0f ? 1.0f : -1.0f);
        b->pcm[i0 + i] += amp * s * expf(-decay * t);
    }
}

static unsigned au_rng = 0x12345u;
static float frand(void){                       /* xorshift, [-1,1) */
    au_rng ^= au_rng << 13; au_rng ^= au_rng >> 17; au_rng ^= au_rng << 5;
    return ((float)(au_rng & 0xFFFFFF) / 8388608.0f) - 1.0f;
}

static void synth_all(void){
    /* menu tick: 30 ms soft square 880 Hz */
    gSnd[SND_MENU_MOVE].len = AU_RATE * 3 / 100;
    gSnd[SND_MENU_MOVE].pcm = au_alloc(gSnd[SND_MENU_MOVE].len);
    tone(&gSnd[SND_MENU_MOVE], 0.0f, 0.03f, 880.0f, 0.25f, 0.5f, 60.0f);

    /* confirm: two rising tones */
    gSnd[SND_MENU_SELECT].len = AU_RATE / 8;
    gSnd[SND_MENU_SELECT].pcm = au_alloc(gSnd[SND_MENU_SELECT].len);
    tone(&gSnd[SND_MENU_SELECT], 0.00f, 0.06f, 660.0f, 0.25f, 0.4f, 40.0f);
    tone(&gSnd[SND_MENU_SELECT], 0.05f, 0.08f, 990.0f, 0.25f, 0.4f, 35.0f);

    /* assault horn: low saw-ish drop */
    gSnd[SND_ASSAULT].len = AU_RATE * 2 / 5;
    gSnd[SND_ASSAULT].pcm = au_alloc(gSnd[SND_ASSAULT].len);
    tone(&gSnd[SND_ASSAULT], 0.00f, 0.40f, 220.0f, 0.30f, 0.7f, 6.0f);
    tone(&gSnd[SND_ASSAULT], 0.10f, 0.30f, 110.0f, 0.30f, 0.7f, 5.0f);

    /* win: rising arpeggio C5-E5-G5 */
    gSnd[SND_WIN].len = AU_RATE / 2;
    gSnd[SND_WIN].pcm = au_alloc(gSnd[SND_WIN].len);
    tone(&gSnd[SND_WIN], 0.00f, 0.18f, 523.25f, 0.25f, 0.2f, 12.0f);
    tone(&gSnd[SND_WIN], 0.12f, 0.18f, 659.25f, 0.25f, 0.2f, 12.0f);
    tone(&gSnd[SND_WIN], 0.24f, 0.26f, 783.99f, 0.28f, 0.2f, 8.0f);

    /* lose: descending minor-ish */
    gSnd[SND_LOSE].len = AU_RATE * 3 / 5;
    gSnd[SND_LOSE].pcm = au_alloc(gSnd[SND_LOSE].len);
    tone(&gSnd[SND_LOSE], 0.00f, 0.20f, 392.00f, 0.26f, 0.3f, 10.0f);
    tone(&gSnd[SND_LOSE], 0.16f, 0.20f, 311.13f, 0.26f, 0.3f, 10.0f);
    tone(&gSnd[SND_LOSE], 0.32f, 0.28f, 246.94f, 0.30f, 0.3f, 6.0f);

    /* explosion: noise burst with exponential decay + low thump */
    gSnd[SND_BOOM].len = AU_RATE * 2 / 5;
    gSnd[SND_BOOM].pcm = au_alloc(gSnd[SND_BOOM].len);
    for (int i = 0; i < gSnd[SND_BOOM].len; i++){
        float t = (float)i / AU_RATE;
        gSnd[SND_BOOM].pcm[i] = 0.5f * frand() * expf(-9.0f * t);
    }
    tone(&gSnd[SND_BOOM], 0.0f, 0.25f, 60.0f, 0.35f, 0.0f, 10.0f);
}

/* ---- device callback: mix live voices --------------------------------- */

static void au_cb(ma_device *dev, void *out, const void *in, ma_uint32 frames){
    (void)dev; (void)in;
    float *o = (float*)out;
    memset(o, 0, (size_t)frames * 2 * sizeof(float));
    for (int v = 0; v < AU_VOICES; v++){
        if (!gVoice[v].live) continue;
        const AuBuf *b = &gSnd[gVoice[v].snd];
        int p = gVoice[v].pos;
        for (ma_uint32 f = 0; f < frames && p < b->len; f++, p++){
            float s = b->pcm[p] * gVolSfx;
            o[f * 2] += s; o[f * 2 + 1] += s;
        }
        gVoice[v].pos = p;
        if (p >= b->len) gVoice[v].live = 0;
    }
}

int au_init(void){
    if (gLive) return 0;
    synth_all();
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = AU_RATE;
    cfg.dataCallback      = au_cb;
    if (ma_device_init(NULL, &cfg, &gDev) != MA_SUCCESS) return -1;
    if (ma_device_start(&gDev) != MA_SUCCESS){ ma_device_uninit(&gDev); return -1; }
    gLive = 1;
    return 0;
}

void au_shutdown(void){
    if (!gLive) return;
    ma_device_uninit(&gDev);
    gLive = 0;
    for (int i = 0; i < SND_COUNT; i++){ free(gSnd[i].pcm); gSnd[i].pcm = NULL; }
}

void au_play(AuSound id){
    if (!gLive || id < 0 || id >= SND_COUNT || !gSnd[id].pcm) return;
    for (int v = 0; v < AU_VOICES; v++){
        if (gVoice[v].live) continue;
        gVoice[v].snd = id; gVoice[v].pos = 0; gVoice[v].live = 1;
        return;
    }
    /* all voices busy: steal slot 0 (placeholder-grade, see audio.h) */
    gVoice[0].snd = id; gVoice[0].pos = 0; gVoice[0].live = 1;
}

void au_set_volume(float sfx, float music){
    if (sfx   < 0) sfx   = 0; if (sfx   > 1) sfx   = 1;
    if (music < 0) music = 0; if (music > 1) music = 1;
    gVolSfx = sfx; gVolMusic = music;
}

int au_backend_live(void){ return gLive; }

#endif /* HAVE_MINIAUDIO */

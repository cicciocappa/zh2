/* audio.c — procedural one-shot SFX over miniaudio, or a silent stub.
 * See audio.h for the contract and the backend-selection rules.
 */
#include "audio.h"

#ifndef HAVE_MINIAUDIO

/* ---- null backend: the game runs silent, zero deps ------------------- */
int  au_init(void){ return 0; }
void au_shutdown(void){}
void au_play(AuSound id){ (void)id; }
int  au_music_play(const char *path, int loop){ (void)path; (void)loop; return 0; }
void au_music_stop(void){}
void au_set_volume(float sfx, float music){ (void)sfx; (void)music; }
int  au_backend_live(void){ return 0; }

#else /* HAVE_MINIAUDIO ------------------------------------------------- */

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING          /* playback only; decoders stay ON so we   */
                                /* can stream music (WAV/FLAC/MP3 built in) */
#include "vat/miniaudio.h"

/* OGG Vorbis is NOT built into miniaudio: it needs stb_vorbis. The Makefile
 * defines AU_HAVE_VORBIS when the user drops vat/stb_vorbis.c in (one file,
 * like miniaudio.h). Without it .ogg simply won't load — use WAV/FLAC/MP3,
 * which need no extra download. */
#ifdef AU_HAVE_VORBIS
#include "vat/stb_vorbis.c"
#endif

#include <math.h>
#include <stdio.h>
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
static float gVolSfx = 0.7f, gVolMusic = 0.5f;

/* looping music bed: one decoded stereo f32 buffer at AU_RATE, mixed in the
 * callback with its own wrap-around position. Its lifecycle (swap on
 * au_music_play, free on stop/shutdown) crosses the audio thread, so — unlike
 * the placeholder-grade lockless SFX voices — it's guarded by a short mutex
 * the callback also takes (uncontended in practice). */
static float    *gMus       = NULL;   /* interleaved L/R f32, AU_RATE        */
static ma_uint64 gMusFrames = 0;
static ma_uint64 gMusPos    = 0;
static int       gMusLoop   = 1;
static ma_mutex  gMusLock;
static int       gMusLockOn = 0;

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

    /* glass shatter: bright high-pass noise crash + scattered high tinkles */
    gSnd[SND_GLASS].len = AU_RATE * 2 / 5;      /* 0.4 s */
    gSnd[SND_GLASS].pcm = au_alloc(gSnd[SND_GLASS].len);
    { float prev = 0.0f;
      for (int i = 0; i < gSnd[SND_GLASS].len; i++){
          float t = (float)i / AU_RATE;
          float n = frand(), hp = n - prev; prev = n;   /* crude 1-pole HP: brighter */
          gSnd[SND_GLASS].pcm[i] = 0.40f * hp * expf(-24.0f * t);
      } }
    tone(&gSnd[SND_GLASS], 0.00f, 0.10f, 5200.0f, 0.12f, 0.0f, 42.0f);
    tone(&gSnd[SND_GLASS], 0.02f, 0.12f, 3700.0f, 0.10f, 0.0f, 34.0f);
    tone(&gSnd[SND_GLASS], 0.05f, 0.14f, 6100.0f, 0.09f, 0.0f, 28.0f);
    tone(&gSnd[SND_GLASS], 0.09f, 0.16f, 2600.0f, 0.09f, 0.0f, 22.0f);
    tone(&gSnd[SND_GLASS], 0.14f, 0.18f, 4400.0f, 0.07f, 0.0f, 18.0f);
}

/* ---- music: decode a whole file into a looping f32 buffer ------------- */

/* WAV/FLAC/MP3 via miniaudio's built-in decoders, converted to the device
 * format (f32 / 2ch / AU_RATE) as it decodes. malloc'd buffer + frame count;
 * NULL on failure. */
static float *au_decode_builtin(const char *path, ma_uint64 *out_frames){
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, AU_RATE);
    ma_decoder dec;
    if (ma_decoder_init_file(path, &cfg, &dec) != MA_SUCCESS) return NULL;

    ma_uint64 cap = (ma_uint64)AU_RATE * 2;               /* grow from ~2 s  */
    float *buf = (float*)malloc((size_t)cap * 2 * sizeof(float));
    ma_uint64 have = 0;
    if (!buf){ ma_decoder_uninit(&dec); return NULL; }
    for (;;){
        if (have == cap){
            cap *= 2;
            float *nb = (float*)realloc(buf, (size_t)cap * 2 * sizeof(float));
            if (!nb){ free(buf); ma_decoder_uninit(&dec); return NULL; }
            buf = nb;
        }
        ma_uint64 want = cap - have, got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&dec, buf + have * 2, want, &got);
        have += got;
        if (got < want || r != MA_SUCCESS) break;         /* EOF or error    */
    }
    ma_decoder_uninit(&dec);
    if (have == 0){ free(buf); return NULL; }
    *out_frames = have;
    return buf;
}

#ifdef AU_HAVE_VORBIS
/* OGG via stb_vorbis. To stay resampler-free the file must ALREADY be AU_RATE
 * stereo — re-encode with: ffmpeg -i in.wav -ar 48000 -ac 2 out.ogg */
static float *au_decode_ogg(const char *path, ma_uint64 *out_frames){
    int ch = 0, rate = 0; short *pcm = NULL;
    int frames = stb_vorbis_decode_filename(path, &ch, &rate, &pcm);
    if (frames <= 0 || !pcm) return NULL;
    if (ch != 2 || rate != AU_RATE){
        fprintf(stderr, "au: %s must be %d Hz stereo (got %d Hz / %d ch) — re-encode\n",
                path, AU_RATE, rate, ch);
        free(pcm); return NULL;
    }
    float *buf = (float*)malloc((size_t)frames * 2 * sizeof(float));
    if (!buf){ free(pcm); return NULL; }
    for (ma_uint64 i = 0; i < (ma_uint64)frames * 2; i++)
        buf[i] = pcm[i] * (1.0f / 32768.0f);
    free(pcm);
    *out_frames = (ma_uint64)frames;
    return buf;
}
#endif

/* ---- device callback: music bed + live one-shot voices ---------------- */

static void au_cb(ma_device *dev, void *out, const void *in, ma_uint32 frames){
    (void)dev; (void)in;
    float *o = (float*)out;
    memset(o, 0, (size_t)frames * 2 * sizeof(float));

    /* music bed first (mutex-guarded, see gMus) */
    ma_mutex_lock(&gMusLock);
    if (gMus && gMusPos < gMusFrames){
        ma_uint64 p = gMusPos;
        for (ma_uint32 f = 0; f < frames; f++){
            if (p >= gMusFrames){ if (!gMusLoop) break; p = 0; }
            o[f * 2]     += gMus[p * 2]     * gVolMusic;
            o[f * 2 + 1] += gMus[p * 2 + 1] * gVolMusic;
            p++;
        }
        gMusPos = p;   /* a one-shot ends parked at gMusFrames; freed on stop */
    }
    ma_mutex_unlock(&gMusLock);

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
    if (ma_mutex_init(&gMusLock) != MA_SUCCESS) return -1;
    gMusLockOn = 1;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = AU_RATE;
    cfg.dataCallback      = au_cb;
    if (ma_device_init(NULL, &cfg, &gDev) != MA_SUCCESS){
        ma_mutex_uninit(&gMusLock); gMusLockOn = 0; return -1;
    }
    if (ma_device_start(&gDev) != MA_SUCCESS){
        ma_device_uninit(&gDev);
        ma_mutex_uninit(&gMusLock); gMusLockOn = 0; return -1;
    }
    gLive = 1;
    return 0;
}

void au_shutdown(void){
    if (!gLive) return;
    ma_device_uninit(&gDev);           /* stops the callback first          */
    gLive = 0;
    if (gMusLockOn){
        free(gMus); gMus = NULL; gMusFrames = gMusPos = 0;
        ma_mutex_uninit(&gMusLock); gMusLockOn = 0;
    }
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

int au_music_play(const char *path, int loop){
    if (!gLive || !path) return -1;
    ma_uint64 n = 0; float *buf = NULL;
#ifdef AU_HAVE_VORBIS
    size_t L = strlen(path);
    if (L >= 4 && path[L-4] == '.' &&
        (path[L-3]=='o'||path[L-3]=='O') &&
        (path[L-2]=='g'||path[L-2]=='G') &&
        (path[L-1]=='g'||path[L-1]=='G'))
        buf = au_decode_ogg(path, &n);
    else
#endif
        buf = au_decode_builtin(path, &n);
    if (!buf) return -1;
    ma_mutex_lock(&gMusLock);
    free(gMus);
    gMus = buf; gMusFrames = n; gMusPos = 0; gMusLoop = loop ? 1 : 0;
    ma_mutex_unlock(&gMusLock);
    return 0;
}

void au_music_stop(void){
    if (!gLive) return;
    ma_mutex_lock(&gMusLock);
    free(gMus); gMus = NULL; gMusFrames = gMusPos = 0;
    ma_mutex_unlock(&gMusLock);
}

void au_set_volume(float sfx, float music){
    if (sfx   < 0) sfx   = 0; if (sfx   > 1) sfx   = 1;
    if (music < 0) music = 0; if (music > 1) music = 1;
    gVolSfx = sfx; gVolMusic = music;
}

int au_backend_live(void){ return gLive; }

#endif /* HAVE_MINIAUDIO */

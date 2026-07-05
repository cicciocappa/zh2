/* audio.h — thin audio API for the game shell (GAME_APP_DESIGN.md §3).
 *
 * Two backends, chosen at COMPILE time:
 *  - HAVE_MINIAUDIO defined (the Makefile defines it when vat/miniaudio.h
 *    exists — the USER downloads that single header): real playback via a
 *    ma_device + our own tiny one-shot mixer. v1 sounds are PROCEDURAL
 *    (PCM synthesized at init: menu blips, assault horn, win/lose
 *    stingers, explosion) so there are zero asset files to ship. On top of
 *    the SFX there is an optional looping MUSIC bed streamed from a file
 *    (au_music_play): WAV/FLAC/MP3 decode with miniaudio's built-ins, OGG
 *    needs a downloaded stb_vorbis (see audio.c) and must be 48 kHz stereo.
 *  - otherwise: null backend — every call is a successful no-op, the game
 *    runs silent. No probing, no dlopen.
 *
 * Threading note (placeholder-grade, documented): voice slots are written
 * by the main thread and read by the audio callback without locks; a torn
 * read can at worst glitch one one-shot blip. Revisit with real assets.
 */
#ifndef AUDIO_H
#define AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SND_MENU_MOVE,      /* cursor tick                      */
    SND_MENU_SELECT,    /* confirm                          */
    SND_ASSAULT,        /* assault-start horn               */
    SND_WIN,            /* mission accomplished stinger     */
    SND_LOSE,           /* base lost stinger                */
    SND_BOOM,           /* explosion                        */
    SND_GLASS,          /* glass shatter (prop vetrata)     */
    SND_COUNT
} AuSound;

int  au_init(void);                 /* 0 = ok (null backend always ok)   */
void au_shutdown(void);
void au_play(AuSound id);           /* fire-and-forget one-shot          */
int  au_music_play(const char *path, int loop); /* stream a music file;  */
                                    /* loop!=0 = seamless loop; 0 ok/-1 err */
void au_music_stop(void);
void au_set_volume(float sfx, float music);   /* 0..1 each               */
int  au_backend_live(void);         /* 1 = real audio device running     */

#ifdef __cplusplus
}
#endif
#endif /* AUDIO_H */

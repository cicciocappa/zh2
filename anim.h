/* anim.h — one-shot animation envelopes for game MECHANISMS
 * (GAME_APP_DESIGN.md §3): turret recoil v1; doors/helicopter/radar later.
 * Not a replacement for anything existing: VAT animates agents,
 * fx_particles the particles, gib/decal the gore. This is the tiny pool
 * that answers "how far into its kick is turret #7 right now?".
 *
 * A channel is keyed (kind, id): anim_fire (re)starts it, anim_value
 * reads a LINEAR 1 -> 0 envelope over `dur` seconds (0 when finished or
 * never fired). Shape it host-side (recoil = v*v, flash = v>0.5, ...).
 * Fixed pool, no malloc, deterministic; pool full = the channel closest
 * to expiry is recycled (a dropped one-shot is invisible in a horde).
 */
#ifndef ANIM_H
#define ANIM_H

#ifdef __cplusplus
extern "C" {
#endif

enum { ANIM_MAX_CH = 256 };

/* channel kinds are defined by the game; keep them here so hosts agree. */
enum { ANIM_TURRET_RECOIL = 1 };

typedef struct { int kind, id; float t, dur; } AnimCh;
typedef struct { AnimCh ch[ANIM_MAX_CH]; int n; } AnimSys;

void  anim_init(AnimSys *as);
void  anim_fire(AnimSys *as, int kind, int id, float dur);
void  anim_update(AnimSys *as, float dt);
float anim_value(const AnimSys *as, int kind, int id);  /* 1 -> 0, else 0 */
int   anim_count(const AnimSys *as);                    /* live channels  */

#ifdef __cplusplus
}
#endif
#endif /* ANIM_H */

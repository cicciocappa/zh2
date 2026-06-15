/* vat_layer — render layer 3D sopra il core particellare (analogo di
 * sprite_layer.c per il path VAT, migrazione_3d.md). Stato per-SLOT, il core
 * non sa nulla (GFX_DESIGN §7):
 *   - HEADING: EMA della velocità (il core non ha heading, la v istantanea è
 *     sporcata dal PBD); congelato quando l'agente è quasi fermo (niente
 *     piroette negli ingorghi).
 *   - FSM da |v|: IDLE/WALK/RUN con isteresi; le clip di stato (walk×6, run×2,
 *     idle×2) scelte per hash(slot) → varietà. Transizioni = crossfade a 2 tap.
 *     ATTACK/SCREAM restano per gli eventi di gioco (API futura).
 *   - FASE: walk/run avanzano con la DISTANZA percorsa / stride_m (niente
 *     foot-sliding); idle col tempo. Seedata dall'handle (slot riusato = pulito).
 *   - OUTFIT/tinta: per-slot, stabili per la vita dell'agente.
 * Produce il buffer instance (12 float/agente) consumato dal renderer VAT.
 */
#ifndef VAT_LAYER_H
#define VAT_LAYER_H
#include "sim_particles.h"

#define VAT_MAX_CLIPS 64
typedef struct { char name[32]; int startFrame, numFrames; float duration, stride; } VatClip;
typedef struct { int texW, texH, rowsPerFrame; float fps, scale; int total;
                 VatClip clip[VAT_MAX_CLIPS]; int nclips; } VatMeta;

typedef struct VatLayer VatLayer;

VatLayer     *vat_layer_create(const char *meta_path, int max_slots);
void          vat_layer_destroy(VatLayer *vl);
const VatMeta *vat_layer_meta(const VatLayer *vl);

/* Avanza heading/FSM/fase. Stesso dt di simp_step (salta se in pausa). */
void vat_layer_update(VatLayer *vl, const SimP *s, float dt);

/* Riempie inst_buf (12 float/istanza: pos.xyz, heading, scala, gA, gB, mix,
 * outfit, tint.rgb) per ogni agente vivo. Ritorna il numero di istanze. */
int  vat_layer_fill(VatLayer *vl, const SimP *s, float *inst_buf, int max_inst);

#endif

/* vat_layer — render layer 3D sopra il core particellare (analogo di
 * sprite_layer.c per il path VAT, migrazione_3d.md). Stato per-SLOT, il core
 * non sa nulla (GFX_DESIGN §7):
 *   - HEADING: EMA della velocità (il core non ha heading, la v istantanea è
 *     sporcata dal PBD); congelato quando l'agente è quasi fermo (niente
 *     piroette negli ingorghi).
 *   - FSM da |v|: IDLE/WALK/RUN con isteresi; le clip di stato (walk×6, run×2,
 *     idle×2) scelte per hash(slot) → varietà. Transizioni = crossfade a 2 tap.
 *   - ATTACK: override di condizione dal sensore d'assedio (simp_wall_pressure):
 *     l'agente che preme un muro/struttura per il goal oltre suona la clip attack.
 *   - HIT: one-shot d'evento (vat_layer_hit) su colpo non letale; interrompe la
 *     FSM per la durata della clip poi riprende.
 *   - DEATH: pool di "decedenti" renderer-side (vat_layer_die): alla morte un
 *     visuale riproduce la clip dying/death una volta poi tiene l'ultimo frame
 *     per un TTL, indipendente dall'agente sim già rimosso. Body senza la clip
 *     (es. crawler) degradano: niente evento.
 *   - FASE: walk/run avanzano con la DISTANZA percorsa / stride_m (niente
 *     foot-sliding); idle col tempo. Seedata dall'handle (slot riusato = pulito).
 *   - OUTFIT/tinta: per-slot, stabili per la vita dell'agente.
 * Produce il buffer instance (12 float/agente) consumato dal renderer VAT.
 */
#ifndef VAT_LAYER_H
#define VAT_LAYER_H
#include "sim_particles.h"

#define VAT_MAX_CLIPS 64
#define VAT_MAX_VARIANTS 8
typedef struct { char name[32]; int startFrame, numFrames; float duration, stride; } VatClip;
typedef struct { int texW, texH, rowsPerFrame; float fps, scale; int total;
                 VatClip clip[VAT_MAX_CLIPS]; int nclips; } VatMeta;

typedef struct VatLayer VatLayer;

/* Multi-modello: N asset VAT (man/fem/obeso/bambino...), mesh+texture+VAT
 * diversi → un draw call per variante (migrazione_3d.md §Multi-modello). Il
 * body di ogni agente è assegnato per-slot via hash(handle) (cosmetico, stabile
 * per la vita dell'agente). I gruppi FSM sono PER-VARIANTE (match per prefisso
 * idle/walk/run): un body può avere un layout di clip diverso (es. il crawler
 * ha 3 clip walk/run/idle invece dei 13 standard) e la stessa FSM velocità→stato
 * funziona lo stesso. */
VatLayer     *vat_layer_create_multi(const char *const *meta_paths, int nvariants, int max_slots);
VatLayer     *vat_layer_create(const char *meta_path, int max_slots); /* = _multi con 1 variante */
void          vat_layer_destroy(VatLayer *vl);
int           vat_layer_nvariants(const VatLayer *vl);
const VatMeta *vat_layer_meta_variant(const VatLayer *vl, int variant);
const VatMeta *vat_layer_meta(const VatLayer *vl); /* = variante 0 */

/* Limita l'assegnazione cosmetica casuale alle prime `n` varianti: le restanti
 * sono body "di gioco" (es. crawler) che si ottengono SOLO con pin_variant, non
 * a caso. Default = tutte. */
void vat_layer_set_random_count(VatLayer *vl, int n);
/* Forza il body di uno slot (es. al momento dello spawn di un tipo). Va chiamata
 * PRIMA che vat_layer_update veda l'agente; consumata al primo update (lo slot
 * riusato torna libero). slot = simp_slot_of(s, indice_spawnato). */
void vat_layer_pin_variant(VatLayer *vl, int slot, int variant);
/* Cambia il body model di uno slot VIVO (es. zombie ferito → mesh crawler).
 * No-op se è già quella variante; altrimenti ri-sceglie una clip valida per il
 * layout del nuovo body. A differenza di pin_variant agisce su un agente già
 * visto (mid-life). */
void vat_layer_set_variant(VatLayer *vl, int slot, int variant);

/* Avanza heading/FSM/fase + il pool decessi. Stesso dt di simp_step (salta se in pausa). */
void vat_layer_update(VatLayer *vl, const SimP *s, float dt);

/* Evento HIT one-shot su un agente vivo (colpo non letale): flinch della clip
 * hit, poi ritorno alla FSM velocità. No-op se il body non ha clip hit (crawler)
 * o se un flinch è già in corso. slot = simp_slot_of(s, indice). */
void vat_layer_hit(VatLayer *vl, int slot);

/* Spawna un decedente (morte leggera, non gib): una visuale che riproduce la
 * clip morte del body una volta a (x,y) poi tiene l'ultimo frame per un TTL.
 * Snapshot di heading/variante/outfit/tinta dallo slot; radius = simp_radius del
 * morto (scala del tank). Da chiamare alla morte, prima che lo slot sia riusato.
 * Ring buffer: pieno = sovrascrive il più vecchio (come i cadaveri M3.3). */
void vat_layer_die(VatLayer *vl, int slot, float x, float y, float radius);

/* Riempie inst_buf (12 float/istanza: pos.xyz, heading, scala, gA, gB, mix,
 * outfit, tint.rgb) con i SOLI agenti assegnati a `variant`. Ritorna il numero
 * di istanze: il chiamante lo disegna con mesh/texture di quella variante. */
int  vat_layer_fill_variant(VatLayer *vl, const SimP *s, int variant, float *inst_buf, int max_inst);
/* Tutti gli agenti, ognuno con la propria variante (solo per nvariants==1). */
int  vat_layer_fill(VatLayer *vl, const SimP *s, float *inst_buf, int max_inst);

#endif

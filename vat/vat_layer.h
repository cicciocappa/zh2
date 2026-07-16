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
#define VAT_MAX_VARIANTS 16   /* deve coprire TUTTI i body (cosmetici + tipi di gioco):
                               * create_multi clampa a questo limite -> se NVAR lo supera
                               * le varianti in coda (es. il tank) vengono perse e i loro
                               * agenti ripiegano sulla scala da raggio (= giganti). */
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
/* Fissa la scala di render di una VARIANTE, disaccoppiandola dal raggio: gli
 * slot di `variant` rendono a una scala uniforme casuale in [smin,smax] (stabile
 * per slot, scala 1.0 = altezza-modello 1.8 m) invece di raggio/0.30. Per i body
 * con proporzioni proprie — es. il tank: raggio grande per la massa fisica, ma
 * alto ~2 m, non ~3 m come darebbe il raggio. smin<=0 = default da raggio. */
void vat_layer_set_variant_scale(VatLayer *vl, int variant, float smin, float smax);

/* Cambia il body model di uno slot VIVO (es. zombie ferito → mesh crawler).
 * No-op se è già quella variante; altrimenti ri-sceglie una clip valida per il
 * layout del nuovo body e riparte pulito dalla WALK (phase azzerata). A
 * differenza di pin_variant agisce su un agente già visto (mid-life). NB: chi la
 * chiama deve garantire che l'agente non sia stunnato (la walk del crawler è
 * guidata dalla distanza: a velocità 0 resterebbe al frame 0). */
void vat_layer_set_variant(VatLayer *vl, int slot, int variant);

/* Avanza heading/FSM/fase + il pool decessi. Stesso dt di simp_step (salta se in pausa). */
void vat_layer_update(VatLayer *vl, const SimP *s, float dt);

/* Ferita visiva di uno slot: l'outfit passa alla versione INSANGUINATA (atlante
 * 4x8: outfit += 16). Idempotente; vale per ogni body (tank incluso). Da chiamare
 * quando il gioco marca lo zombie ferito. slot = simp_slot_of(s, indice). */
void vat_layer_make_bloody(VatLayer *vl, int slot);

/* Fissa l'outfit di uno slot (atlante 4x8: 0..13 puliti, +16 = insanguinato,
   clamp [0,31]). Controllo diretto per il lab FX (make_bloody è a senso unico). */
void vat_layer_set_outfit(VatLayer *vl, int slot, int outfit);

/* Forza la CLIP di locomozione di uno slot, scavalcando la FSM velocità→stato e
   la scelta casuale per-slot: l'agente riproduce sempre `clip_idx` (indice nel
   meta del suo body, avanzato per distanza/tempo come al solito). clip_idx = -1
   = torna in AUTO (FSM). Resettato a -1 al cambio body e al riuso dello slot.
   Per il lab FX (anteprima delle varianti walk/run). slot = simp_slot_of(...). */
void vat_layer_force_clip(VatLayer *vl, int slot, int clip_idx);

/* Latch esterno della clip d'ATTACCO (stesso hold+decay dell'assedio muri, che
 * il layer arma da solo via wall_pressure): per gli attacchi che quel sensore
 * non vede — il contact siege delle torrette (def_contact_turret). Chiamare
 * ogni frame finché l'agente attacca, PRIMA di vat_layer_update; (dirx,diry) =
 * direzione mondo verso il bersaglio (0,0 = tieni l'heading corrente). */
void vat_layer_attack(VatLayer *vl, int slot, float dirx, float diry);

/* Evento HIT one-shot su un agente vivo (colpo non letale): flinch della clip
 * hit, poi ritorno alla FSM velocità. No-op se il body non ha clip hit (crawler)
 * o se un flinch è già in corso. slot = simp_slot_of(s, indice). Ritorna la
 * DURATA del flinch avviato (s), 0 se nessuno: il chiamante la passa a
 * simp_stun per piantare l'agente sotto l'animazione (niente foot-sliding). */
float vat_layer_hit(VatLayer *vl, int slot);

/* Spawna un decedente (morte leggera, non gib): una visuale che riproduce la
 * clip morte del body una volta a (x,y) poi tiene l'ultimo frame per un TTL.
 * Snapshot di heading/variante/outfit/tinta dallo slot; radius = simp_radius del
 * morto (scala del tank). Da chiamare alla morte, prima che lo slot sia riusato.
 * Ring buffer: pieno = sovrascrive il più vecchio (come i cadaveri M3.3). */
void vat_layer_die(VatLayer *vl, int slot, float x, float y, float radius);

/* Gib (GFX §5 gore): burst di pezzi frattaglia balistici renderer-side a (x,y)
 * alla morte esplosiva (gib pesante). Solo visuali (niente collisione, come il
 * volo M3.2): parabola + spin, atterrano e si posano fino al TTL. Mescola rosso
 * sangue + tint dello zombie (slot). radius = taglia del corpo. seed dà varietà. */
void vat_layer_gib(VatLayer *vl, int slot, float x, float y, float radius, unsigned int seed);
/* Tre TIER di gore (stesso pool/render dei gib, taglie crescenti dei box):
 *  - _gib_wound: burst PICCOLO (pochi cubetti, bassa spinta) per i colpi non
 *    letali — sangue su sanguinamento, schizzi su mutilazione. Cubi minuscoli.
 *  - _limb: UN pezzo MEDIO allungato (un arto) con arco balistico — il chiamante
 *    lo invoca 1× per il braccio, 2× per le gambe (seed diversi → ventaglio).
 *  - _gib (sopra): il burst GRANDE della morte esplosiva. Pezzi più grossi.
 * radius = taglia del corpo (scala pezzi/spinta); seed dà varietà. */
void vat_layer_gib_wound(VatLayer *vl, int slot, float x, float y, float radius, unsigned int seed);
void vat_layer_limb(VatLayer *vl, int slot, float x, float y, float radius, unsigned int seed);
/* Emette i gib attivi: 10 float/pezzo (x, y, z_sul_suolo, hx, hy, hz, ang, r,g,b).
 * Il chiamante somma terrain_z(x,y) a z e costruisce un box per pezzo. Ritorna n. */
int  vat_layer_fill_gibs(VatLayer *vl, float *out, int max_out);

/* Mesh-gib (gore con mesh 3D vere, FX_LAB step "maim arm"): pool balistico
 * renderer-side di pezzi 3D — arto reciso + frammenti — che volteggiano (tumble
 * a 3 assi) e atterrano fino al TTL, come i gib ma con mesh anziché box. Il
 * modulo tiene SOLO la fisica + un mesh_id (0=arm, 1/2=frammenti); il tool mappa
 * id->VAO e disegna (la mesh non la conosce). vat_layer_maim_arm lancia braccio +
 * 2 frammenti da (x,y), espulsi verso il lato del corpo (heading±90°) con arco.
 * radius = taglia (scala dei pezzi/spinta); heading in rad; seed dà varietà. */
void vat_layer_maim_arm(VatLayer *vl, int slot, float x, float y, float radius,
                        float heading, unsigned int seed);
/* Come maim_arm ma per le GAMBE: recide due gambe (mesh_id 3 = leg, ×2) + un paio
 * di frammenti (gib1/gib2), espulsi BASSO-AVANTI (origine all'anca, non alla spalla)
 * con arco balistico e tumble 3D. Il chiamante passa anche il body a maimed_legs e
 * fa partire gli schizzi di sangue. heading in rad; seed dà varietà. */
void vat_layer_maim_legs(VatLayer *vl, int slot, float x, float y, float radius,
                         float heading, unsigned int seed);
/* Esplosione TOTALE: il corpo si disintegra in TUTTI i suoi pezzi mesh — 2 braccia
 * (mesh 0), 2 gambe (3), testa (4) e le due metà del torso (5,6) — espulsi radialmente
 * con arco balistico e tumble 3D, PIÙ un burst abbondante di cubetti sangue/carne. Da
 * usare al posto di gib+die: NON lascia cadavere né animazione di morte, del corpo
 * resta solo la pioggia di pezzi (che si posano fino al TTL) e una pozza. seed=varietà.
 * `force` scala la velocità iniziale dei pezzi: 1.0 = esplosione piena (mina), valori
 * più bassi (~0.4) = smembramento ravvicinato (colpo di torretta pesante). */
void vat_layer_explode(VatLayer *vl, int slot, float x, float y, float radius,
                       float force, unsigned int seed);
/* Emette i mesh-gib attivi: 9 float/pezzo (mesh_id, x, y, z_sul_suolo, ax, ay,
 * az, angle, scale). Il chiamante somma terrain_z(x,y) a z e costruisce la model
 * matrix = T(world) * rot(axis,angle) * scale. Ritorna il numero. */
int  vat_layer_fill_mesh_gibs(VatLayer *vl, float *out, int max_out);

/* Decal di sangue PERSISTENTI (GFX §5.2): macchie a terra dove i corpi si posano
 * (fine vita del decedente) o esplodono (gib). Generati internamente; il pool è
 * un ring buffer cappato (niente decay, niente collisione). Emette 6 float/decal
 * (x, y, size, r,g,b): il chiamante somma terrain_z(x,y) e disegna un disco
 * blended a terra. Ritorna il numero di decal. */
int  vat_layer_fill_decals(VatLayer *vl, float *out, int max_out);

/* Pose di morte distinte bakate per body: il decedente sceglie a caso fra il
 * gruppo 'dying' (pose 0) e 'death' (pose 1) — falle a terra ~90° diverse — quindi
 * la sagoma deve combaciare con la posa che è stata effettivamente giocata, o
 * appare ruotata. La COLONNA d'atlante = variante*VAT_CORPSE_NPOSE + posa. */
#define VAT_CORPSE_NPOSE 2
/* La RIGA d'atlante = outfit BASE (0..15) del modello; l'albedo è bakato sempre
 * con la versione INSANGUINATA (outfit+16) perché un cadavere è sempre insanguinato
 * → 16 righe, non 32. La normal map NON dipende dall'outfit (stessa geometria) →
 * una sola riga. Il fill emette outfit&15 (scarta il bit insanguinato: già implicito). */
#define VAT_CORPSE_NOUTFIT 16

/* Sagome-cadavere PERSISTENTI: come i decal di sangue ma orientate e tipizzate
 * per variante+posa+outfit. Il chiamante le rende come quad texturizzati
 * campionando l'atlante (colonna = variante*VAT_CORPSE_NPOSE + posa, riga =
 * outfit). Emesse quando un decedente svanisce, oltre alla macchia. Ring buffer
 * cappato (niente decay). 6 float/decal: x, y, heading(rad), size(raggio mondo),
 * colonna d'atlante, outfit. Ritorna il numero. */
int  vat_layer_fill_corpse_decals(VatLayer *vl, float *out, int max_out);
/* Vaporizza le sagome-cadavere entro il raggio (esplosione/fuoco): le marca
 * cancellate; fill_corpse_decals le salta. Lo slot torna vivo se un nuovo
 * cadavere lo riusa. Speculare a simp_corpse_clear per la parte VISIVA. */
void vat_layer_clear_corpses(VatLayer *vl, float x, float y, float r);

/* Mondo nuovo (rebuild/retry livello): svuota tutto il transiente — pool
 * decedenti/gib/mesh-gib, decal persistenti (sangue + sagome) e stato
 * per-slot (`seen` compreso: un SimP nuovo riusa slot+generation, gli handle
 * vecchi collidono). Chiamare accanto a scene_instantiate. */
void vat_layer_reset(VatLayer *vl);

/* Riempie inst_buf (12 float/istanza: pos.xyz, heading, scala, gA, gB, mix,
 * outfit, tint.rgb) con i SOLI agenti assegnati a `variant`. Ritorna il numero
 * di istanze: il chiamante lo disegna con mesh/texture di quella variante. */
int  vat_layer_fill_variant(VatLayer *vl, const SimP *s, int variant, float *inst_buf, int max_inst);
/* Tutti gli agenti, ognuno con la propria variante (solo per nvariants==1). */
int  vat_layer_fill(VatLayer *vl, const SimP *s, float *inst_buf, int max_inst);

#endif

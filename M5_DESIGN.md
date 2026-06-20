# M5 — Gameplay difensivo: torrette, danno, ferite, base (design tecnico)

> **DA IMPLEMENTARE.** Primo passo da demo tecnica a gioco. Copre la **fase
> difesa**: torrette che falciano l'orda, danno/ferite/morte degli zombie, la
> base al centro come bersaglio dell'orda e la condizione di sconfitta.
> **L'economia (biomassa, droni, basi di raccolta, mortai/speciali, muri
> costruiti dal giocatore) è FUORI SCOPE qui** ed è documentata a parte (vedi
> §11): questo doc descrive lo *slice 1*, un loop di difesa giocabile con
> piazzamento a budget statico.
>
> **Nota numerazione.** In `TODO.md` M5 è etichettato "GPU" e M6 "Gioco".
> Questo doc è di fatto la milestone *gioco difensivo*; la roadmap GPU è
> opzionale e va oltre i 100k agenti (non serve per giocare). Da rinumerare il
> TODO quando si parte (proposta: questa = M5 "Difesa", GPU → M7+).

Principio guida (come M3 e SIEGE): **il core resta gameplay-agnostico.** HP,
tipi di nemico, stati di ferita, torrette, ondate, sconfitta vivono nel codice
di gioco, indicizzati per **slot** (stabile, M3.1). Il core guadagna solo i
primitivi *fisici* che il gioco non può fabbricarsi da fuori — e sono **due**
sole API nuove (§2, §5). Tutto il resto riusa M3/SIEGE.

---

## 1. Cosa è core, cosa è gioco

| Meccanica                        | Dove vive            | Primitivo                                   |
|----------------------------------|----------------------|---------------------------------------------|
| Raggio/hitscan torretta          | **core (NUOVO)**     | `simp_query_ray` (§2)                        |
| Rallentare uno zombie (crawl)    | **core (NUOVO)**     | `simp_set_vpref` (§5)                        |
| Spinta del colpo pesante         | core (esiste)        | `simp_apply_impulse` (M3.2)                  |
| Cadavere alla morte              | core (esiste)        | `simp_corpse_add` (M3.3)                     |
| Assedio della base + crollo      | core (esiste)        | `simp_wall_pressure`/`_cell` + `set_wall` (SIEGE) |
| Attrazione verso la base         | core (esiste)        | `simp_set_goal` (M1)                         |
| HP, tipo, stato di ferita        | gioco (per slot)     | array paralleli, `simp_slot_of` (M3.1)       |
| Torrette, sweep, fuoco, danno    | gioco                | logica sopra le query                        |
| Ondate, budget, sconfitta        | gioco                | spawn director, contatori                    |
| Gib (arto/gambe che volano)      | renderer (FX puri)   | particelle visive, NON agenti                |
| Sangue su outfit / decal terreno | renderer             | swap texture / buffer decal (gated GFX stack)|

Solo le prime due righe toccano `sim_particles.h`. Il resto è gioco o renderer.

---

## 2. Hitscan: `simp_query_ray` (NUOVO core)

> **IMPLEMENTATO** (`test_turret.c` parte 1 PASS, vs brute force). Deviazioni
> dal piano sotto: (a) niente dedup via stamp — gli agenti sono binnati per
> cella-CENTRO (una sola volta), quindi la DDA testa per ogni cella attraversata
> il suo **alone 3×3** (un disco di raggio ≤ `grid_rmax` ≤ `ccell`/2 può
> toccare il raggio da una cella vicina); la sovrapposizione dell'alone è
> deduplicata in `ray_consider`. (b) Niente early-out esplicito per il
> non-piercing: si passa `max_out = 1` e `ray_consider` tiene il **più vicino**
> (set dei nearest-`max_out`, scrematura del più lontano quando pieno; gli
> agenti sfrattati e ri-presentati dall'alone sono rifiutati perché il loro `t`
> è ≥ del peggiore corrente). (c) Occlusione muri = DDA separata sulla griglia
> **nav** (`solid[]`) che tronca `maxdist` al primo muro. Output ordinato per
> `t` (insertion sort, `n ≤ max_out`). Fallback brute force su griglia stantia
> verificato. Helper: `wall_ray_t`, `ray_disc_t`, `ray_consider`.

**Problema.** La torretta spara un raggio fino alla sua portata che interseca
gli zombie (`considerazioni.txt` righe 6-7). Con l'upgrade *piercing* il
proiettile non si ferma al primo colpito ma prosegue colpendo tutti quelli sul
tragitto. Le query esistenti (`circle`/`nearest`, M3.4) non bastano: serve un
**segmento**.

**Soluzione.** Una terza query sulla stessa griglia di collisione già
ricostruita a fine step (M3.4), con DDA lungo il raggio (Amanatides-Woo) e test
segmento-vs-disco per cella. Stessa filosofia di `query_circle`: costo ∝ celle
attraversate, non N; valida tra uno step e l'altro; fallback brute force se
chiamata fuori sequenza.

```c
/* Hitscan lungo il segmento da (ox,oy) per (dx,dy) normalizzato, fino a
 * maxdist. Riempie out[]/out_t[] con gli INDICI degli agenti colpiti e il
 * parametro t (distanza lungo il raggio) dell'impatto, ORDINATI per t
 * crescente; ritorna quanti (saturando a max_out).
 *   - Non-piercing  = usa out[0] (il primo colpito).
 *   - Piercing      = itera su tutti gli hit fino alla portata.
 * Il raggio è OCCLUSO dai muri (line-of-sight): maxdist viene troncato alla
 * prima cella nav solida incontrata, quindi una torretta non spara attraverso
 * un muro. Flag SIMP_RAY_NOWALL per ignorare l'occlusione (colpi che bucano).
 * Volanti esclusi salvo SIMP_QUERY_FLYING (come query_circle); i cadaveri NON
 * sono mai colpiti (sono morti — il proiettile li attraversa). */
#define SIMP_RAY_NOWALL 0x2u
int simp_query_ray(const SimP *s, float ox, float oy, float dx, float dy,
                   float maxdist, int *out, float *out_t, int max_out,
                   uint32_t flags);
```

**Dettagli d'implementazione.**

- **DDA sulla griglia di collisione** (`cstart`/`corder`, cella = `2·r_max`):
  marcia di cella in cella in ordine di distanza crescente. In ogni cella, per
  ogni agente, test segmento-disco: proiezione del centro sul raggio, distanza
  perpendicolare ≤ `r_i`, e `t` dell'ingresso in `[0, maxdist]` (radice più
  vicina di `|o + t·d − c|² = r²`).
- **Dedup**: un disco può appartenere a due celle e venire testato due volte.
  Stesso meccanismo di stamp/visited già usato dalla griglia, oppure dedup nel
  buffer (gli hit sono pochi). NON contare due volte lo stesso indice.
- **Early-out non-piercing**: trovato un hit a `t*`, si può fermare la DDA
  quando la distanza d'ingresso della cella corrente supera `t* + r_max`
  (conservativo: un disco che sporge dalla cella successiva può comunque essere
  più vicino di `t*`). Per il piercing si percorre fino a `maxdist`.
- **Occlusione muri**: una seconda DDA leggera sulla griglia **nav** (celle
  solide) tronca `maxdist` alla prima cella muro. Si può fondere con la
  scansione o pre-calcolare il `t` del muro e usarlo come tetto. Coerente con il
  fatto che i muri sono già nel modello (SDF/nav). `SIMP_RAY_NOWALL` salta
  questo passo.
- **Volanti**: come per `query_circle`, non sono nella griglia → includerli
  (`SIMP_QUERY_FLYING`) forza il brute force. Caso raro (sparare a zombie in
  volo); default escluso.

**Verifica** (in `test_turret.c`, §9): 10k agenti random, M raggi casuali;
confronto dell'insieme di hit (e del primo hit) contro un brute force
O(N) segmento-disco. Più scene mirate: colonna di agenti allineati → piercing
li colpisce tutti in ordine di `t`, non-piercing solo il primo; raggio dietro
un muro → 0 hit (LoS); `SIMP_RAY_NOWALL` → buca.

---

## 3. Stato di gioco per-slot

> **IMPLEMENTATO** (§3-§6) nel modulo `defense.h` / `defense.c` — layer di gioco
> riusabile (lo useranno test e sandbox/vat), core-agnostico: parla al core
> solo via API pubbliche `simp_*`. `test_defense.c` PASS (light mowing con tutti
> e 3 i tipi di ferita, heavy senza cadavere, tank a 4 colpi, determinismo).
> Deviazioni/scelte: `DefBody` include il tank (gli screamer e le perturbazioni
> di costo restano a M5b); il roll della ferita e la prob. di cadavere sono
> **hash deterministici dello slot** (niente stato RNG, niente `rand()`); per
> slot: `hp`/`body`/`wound`/`hheat` (colpi pesanti assorbiti dal tank). NON
> ancora qui: base/sconfitta (§7) e spawn director (§8).

Array paralleli del gioco, **indicizzati per slot** (M3.1: lo slot è stabile per
tutta la vita dell'agente; gli indici densi no). Allocati a capacità
`max_agents`.

```c
typedef enum { BT_OBESE, BT_MAN, BT_WOMAN, BT_CHILD,
               BT_TANK, BT_SCREAMER, BT_COUNT } BodyType;
typedef enum { W_NONE, W_BLOODY, W_MAIMED_ARM, W_CRAWLING } WoundState;

uint16_t  hp[CAP];        /* HP correnti                                   */
uint8_t   body[CAP];      /* BodyType (determina HP max, massa, raggio…)    */
uint8_t   wound[CAP];     /* WoundState; W_NONE finché non ferito           */
uint8_t   hheat[CAP];     /* colpi pesanti assorbiti (tank); 0 per gli altri*/
```

Tabella per tipo (dati di gioco, costante):

```c
typedef struct {
    uint16_t hp_max;      /* obesi > uomini > donne > bambini (riga 18)     */
    SimPAgentDesc desc;   /* radius, v_pref, mass per simp_spawn_desc (M3.5)*/
    uint8_t  heavy_hits;  /* colpi di torretta pesante prima di gibbare (tank>1, altri=1) */
    uint16_t biomass;     /* taglia in biomassa (economia, slice 2)         */
} EnemyDef;
```

Allo spawn: `idx = simp_spawn_desc(...)`, `slot = simp_slot_of(s, idx)`, poi
`hp[slot] = def.hp_max; body[slot] = ...; wound[slot] = W_NONE; hheat[slot]=0`.
Gli speciali (tank, screamer) restano fuori scope di dettaglio qui (riga 18-19):
il tank è già un tipo M3.5 (massa ×10), gli serve solo `heavy_hits > 1`.

---

## 4. Torrette (interamente lato gioco)

Il core non sa cosa sia una torretta: è una struttura di gioco che, a cadenza,
chiama `simp_query_ray` e applica danno. Stato:

```c
typedef struct {
    float x, y;               /* posizione (m)                              */
    float ang;                /* direzione di mira corrente (rad)           */
    float arc_min, arc_max;   /* estremi dell'arco di copertura             */
    int   sweep_dir;          /* +1/-1                                      */
    float sweep_speed;        /* rad/s (upgrade, riga 3)                    */
    float range;              /* portata (upgrade, riga 4)                  */
    float fire_period;        /* s tra i colpi (upgrade; light < heavy, riga 5) */
    float fire_timer;
    float damage;             /* HP per colpo                               */
    uint8_t heavy;            /* 0 = leggera, 1 = pesante                    */
    uint8_t piercing;         /* upgrade (righe 6-7)                        */
} Turret;
```

**Update per frame** (dopo `simp_step`, quando la griglia è fresca):

1. **Acquisizione + dwell.** Cerca un bersaglio nell'arco: `simp_query_circle`
   di raggio `range` attorno alla torretta, filtra per angolo in
   `[arc_min, arc_max]`, tiene il più vicino. Se c'è, **rallenta/ferma lo
   sweep** e punta verso di lui (`ang → atan2` del bersaglio, rate-limitato);
   altrimenti spazza l'arco a `sweep_speed`, rimbalzando agli estremi
   (`sweep_dir` si inverte). Il dwell risolve l'interazione *sweep veloce vs
   rateo basso* (la torretta non scavalca il bersaglio tra due colpi) ed è
   leggibile ("acquisisce e ingaggia").
2. **Fuoco.** `fire_timer += dt`. Se `≥ fire_period` **e c'è un bersaglio**:
   `simp_query_ray(s, x, y, cos ang, sin ang, range, out, t, max, flags)`.
   - non-piercing → danno a `out[0]`;
   - piercing → danno a tutti gli `out[0..n)`.
   `fire_timer -= fire_period`. **Senza bersaglio non si spara** (riga: "solo
   quando hanno un bersaglio alla portata") → niente colpi a vuoto. (Proiettili
   gratis: nessun costo munizioni — vedi §11.)
3. **Applicazione danno** → §5 (ferite) / §6 (morte). **Trappola indici**
   (M3.4): gli `out[]` sono indici densi; convertirli SUBITO in handle, oppure
   uccidere in ordine **decrescente** d'indice. Vedi §6.

**Leggera vs pesante** (righe 8-20):
- **Leggera**: `damage` parziale, alto rateo. Colpo → `hit` + sottrai HP; se
  sopravvive entra in stato ferito (§5), se va a zero muore con cadavere
  probabile (§6). Crea logoramento: feriti lenti (jam) e cadaveri (barricate).
- **Pesante**: rateo basso, e su un normale è **kill istantanea con gib, niente
  cadavere** (riga 19) + `simp_apply_impulse` per la spinta fisica del colpo
  (si appoggia all'identità physics-based: il colpo *sposta* la folla). Sul
  tank: `hheat[slot]++`; gibba solo a `hheat ≥ def.heavy_hits` (riga 20), così
  nessun cadavere di tank da gestire.

---

## 5. Ferite: la biforcazione a 3 vie

Quando un colpo **leggero** lascia lo zombie con HP > 0 e `wound[slot] == W_NONE`,
si sceglie **una sola volta** uno dei tre esiti (righe 9-15), via RNG seedato
dallo slot (deterministico, niente `rand()`):

```c
if (hp[slot] > 0 && wound[slot] == W_NONE) {
    switch (roll3(slot)) {            /* xorshift(slot) -> {0,1,2} pesato     */
    case 0: wound[slot] = W_BLOODY;       break;   /* (1) outfit insanguinato */
    case 1: wound[slot] = W_MAIMED_ARM;   break;   /* (2) braccio mozzato     */
    case 2: wound[slot] = W_CRAWLING;              /* (3) gambe mozzate→crawl */
            simp_set_vpref(s, idx, V_CRAWL);       /*     molto più lento     */
            break;
    }
}
```

Effetti (renderer + un solo aggancio core):

- **(1) `W_BLOODY`** — puramente visivo: il renderer passa all'outfit
  insanguinato. Oggi 16 outfit in una texture 1024×1024; diventerà 1024×2048
  con i 16 insanguinati nella metà bassa (riga 10-11). Nessun tocco al core.
  Decal di sangue a terra → §11 nota render.
- **(2) `W_MAIMED_ARM`** — il renderer cambia *body type* VAT in `maimed_arm`
  (già renderizzato, esiste), opzionalmente outfit insanguinato. FX: un braccio
  che vola via (gib visivo, NON un agente). Velocità invariata. Nessun tocco al
  core (a meno di voler una versione femminile del maimed_arm per coerenza
  outfit — è lavoro di asset, riga 13).
- **(3) `W_CRAWLING`** — body type VAT `maimed_legs` + stato crawl (esiste, già
  retargetato). FX: un paio di gambe in aria che ricadono. **Unico aggancio
  core**: `simp_set_vpref(s, idx, V_CRAWL)` per rallentarlo drasticamente.
  → Sinergia M3.7: i crawler lenti **alzano il campo `jam`** → il Dijkstra
  devia l'orda attorno ai feriti **gratis**. Le ferite non sono cosmetiche,
  rimodellano il flusso.

```c
/* NUOVO core: cambia la velocità preferita di un agente vivo (post-spawn).
 * Scrive l'array per-agente di v_pref (quello impostato da spawn_desc + jitter).
 * Per la transizione crawl, e in generale per buff/debuff di velocità.
 * Index-based come simp_kill/simp_sleep: convertire da handle se serve
 * persistenza. No-op sui volanti. */
void simp_set_vpref(SimP *s, int i, float v_pref);
```

`simp_set_vpref` è l'unica seconda aggiunta al core. **IMPLEMENTATO**
(index-based, clamp a ≥0, no-op fuori range; `test_turret.c` D PASS: il
crawler a 0.2 m/s avanza 0.33 m in 2 s contro 2.2 m del walker normale). NON serve cambiare il
raggio (il crawler tiene il suo footprint; la posa strisciante è del renderer):
così non si rischia di far crescere `r_max` e ingrossare la cella di collisione
(M3.5). Se in futuro si volesse, `simp_set_radius` è simmetrico — ma solo
restringere è sicuro.

Hit successivi su uno zombie già ferito: solo sottrazione HP fino alla morte
(non si ri-tira l'esito; eventualmente `W_BLOODY` → re-sanguina la texture).

---

## 6. Morte

```c
/* hp[slot] <= 0 dopo il danno: */
float dx = simp_px(s)[idx], dy = simp_py(s)[idx];
if (!heavy_kill) {
    /* anim death (renderer) + cadavere con probabilità (M3.3) */
    if (roll(slot) < P_CORPSE)                  /* default ~0.2-0.3 */
        simp_corpse_add(s, dx, dy, simp_radius_arr(s)[idx]*0.9f, CORPSE_TTL);
} else {
    /* pesante: niente cadavere, gib FX (renderer) + spinta */
    simp_apply_impulse(s, dx, dy, GIB_R, GIB_PUSH);
}
biomass_spawn(dx, dy, def.biomass);             /* §11, slice 2 — stub ora */
/* poi: simp_kill(idx) — vedi ordinamento sotto */
```

**Ordinamento delle kill (trappola M3.4).** `simp_query_ray` ritorna indici
densi che muoiono alla prima `simp_kill` (swap-and-pop). Pattern corretto, due
opzioni:
- convertire ogni hit in **handle** (`simp_handle_of`) subito dopo la query,
  applicare il danno per slot, poi per i morti `simp_index_of` → `simp_kill`;
- **oppure** raccogliere gli indici da uccidere e chiamare `simp_kill` in
  ordine **decrescente** d'indice (lo swap porta in `i` solo indici > i).

I **mucchi di cadaveri** (riga 16-17): il pool M3.3 è a capacità fissa con TTL
(rimpiazzo del più-vicino-a-scadenza) → il costo è già limitato by design. Il
rendering "appiattito" dei cadaveri stabili (decal invece di VAT pieno) è una
LOD del renderer, fuori scope core. Da decidere il TTL/visual in M6.

---

## 7. La base e la sconfitta

> **IMPLEMENTATO** (`test_base.c` PASS, aggancio visivo in `vat_horde`). Sistema
> di **strutture distruttibili** lato gioco in `defense.c`: una struttura =
> gruppo di celle-muro con HP condivisi. API: `def_add_structure(g, hp, is_core)`,
> `def_struct_cell(g, id, cx, cy)` (assegna la cella e alza il muro),
> `def_cell_struct` (per il render), `def_struct_hp/_max/_collapsed`, `def_lost`.
> L'assedio è processato in `def_update` (dopo le torrette): per ogni agente a
> terra con `simp_wall_pressure ≥ ATTACK_MIN_P` sulla cella di una struttura, un
> timer d'attacco **per-slot** (stessi `ATTACK_PERIOD 0.8`/`ATTACK_DAMAGE 5`/
> `ATTACK_MIN_P 0.006` di `test_siege`) eroso gli HP; a HP ≤ 0 → **crollo**: le
> celle vengono liberate (`simp_set_wall false`) + `simp_terrain_commit` →
> reroute automatico; se la struttura è il **core** (`is_core`) non si fa
> reroute, si alza `def_lost` (sconfitta). Deterministico (timer per-slot +
> sim deterministica). Due nuovi accessor banali al core
> (`simp_grid_w`/`simp_grid_h`/`simp_cell_size`) per decodificare `wall_cell`.
> Nota di verifica: l'assedio morde solo con una folla CONCENTRATA (anelli
> piccoli, orda densa) — anelli grandi e orda rada diluiscono il fronte e gli
> HP scendono di pochissimo. In `vat_horde` (scena `scenes/base.scn`,
> `VAT_HORDE_BASE=1`) le 10 torrette tengono la base senza fatica; con
> `VAT_HORDE_TURRETS=0` l'orda non difesa fa crollare l'anello esterno (reroute
> visibile: la folla dilaga sul core), poi il core → `BASE PERSA`.

Il giocatore difende la base al **centro**; l'orda è attratta lì (disegniamo il
target lì) e dopo aver abbattuto mura ed eventuali barriere, se raggiunge il
core è game over.

**Soluzione: la base è la struttura assediabile più interna — riusa SIEGE per
intero.** Niente meccanica nuova:

1. Un **goal** al centro (`simp_set_goal`) tira l'orda verso il cuore.
2. Anelli di **mura/barriere** distruttibili attorno (scene `poly … solid`, M7):
   l'orda li assedia con `simp_wall_pressure`/`simp_wall_cell`, il gioco fa
   girare i timer d'attacco per-slot e gli HP delle strutture (SIEGE §3), crollo
   → `simp_set_wall(false)` → reroute automatico verso l'anello successivo.
3. Il **core** è semplicemente la struttura più interna: quando i suoi HP vanno
   a zero **non si fa reroute, si perde**. La condizione di sconfitta è
   `core_hp <= 0`, niente di più.

Vantaggi: HP del core come barra di tensione leggibile (non game-over binario su
un singolo straggler); difesa a 360° = puzzle di copertura angolare per le
torrette; tutto già verificato in `test_siege`. Con il goal centrale **sigillato**
dal core, l'orda non drena: la popolazione cala **solo** per le kill delle
torrette → se le torrette non tengono, l'assedio cresce e il core cade. Loop di
difesa chiuso, sui sistemi che esistono già.

> **Variante (opzionale).** Una piccola goal cell drenante dietro il core per
> consumare i pochi che breccia (sollievo di popolazione). Per lo slice 1 il
> goal sigillato basta.
>
> **Differita.** Con più strutture difendibili (le basi droni dell'economia,
> §11) servirà **attribuire il drain/contatto alla struttura giusta**: oggi
> `simp_step` ritorna il conteggio totale drenato, non quale goal. Per lo slice
> 1 c'è un solo core → non serve. L'attribuzione multi-goal è una questione
> aperta (vedi `TODO.md` "Politica multi-goal") da chiudere nel doc economia.

---

## 8. Spawn director + budget di piazzamento (minimi)

Tutto lato gioco, nessun primitivo nuovo.

- **Spawn director**: emette dalle rect `spawn` di scena (M7) lungo il bordo,
  con rampa nel tempo (densità/tipi crescenti). Emissione **senza burst** con
  `simp_free_at` (M3.x): l'emitter si auto-strozza alla portata dell'uscita.
  Una struttura ondata minimale (timer, conteggi, mix di tipi) basta per lo
  slice 1.
- **Budget statico**: un intero `budget` per ondata; piazzare una torretta
  costa e scala il budget. Le torrette dello slice 1 sono **punti** che non
  bloccano la nav (niente costruzione di muri da parte del giocatore ancora —
  quella è economia/M5b). Posizionamento sulle celle libere, validato a occhio
  nel sandbox.

---

## 9. Verifica — `test_turret.c` (headless)

Sullo schema dei test M3 (oracolo brute force + scena + metriche + determinismo
+ zero NaN):

1. **`simp_query_ray` vs brute force**: 10k agenti, M raggi casuali; confronto
   insieme di hit e primo hit con un O(N) segmento-disco. Scene mirate:
   colonna allineata (piercing tutti in ordine di `t`, non-pierce il primo),
   raggio dietro un muro (0 hit, LoS), `SIMP_RAY_NOWALL` (buca).
2. **Sweep + fuoco**: torretta che falcia una colonna in transito; conteggio
   kill nel tempo; due run identiche → stesso conteggio (determinismo, RNG
   seedato per slot).
3. **Ferite 3-vie**: somministrare danno leggero a una popolazione, verificare
   le transizioni di stato, la distribuzione dei tre esiti, e che i `W_CRAWLING`
   abbiano `v_pref` abbassata → il campo `jam` (`simp_jam_arr`) sale dove sono
   i crawler.
4. **Pesante**: kill istantanea del normale (nessun cadavere aggiunto), tank che
   sopravvive a `heavy_hits−1` colpi e gibba al `heavy_hits`-esimo; spinta
   (`apply_impulse`) misurabile sui vicini.
5. **Base/sconfitta**: orda che assedia il core (riusa lo scaffold `test_siege`);
   senza torrette `core_hp → 0` (loss flag); con torrette sufficienti il core
   sopravvive (drain dei besiegers per kill > pressione). Determinismo, no NaN.

Verifica visiva nel sandbox 3D (`vat_horde`): ventagli di fuoco, feriti che
strisciano e creano deviazioni, cadaveri che barricano i varchi, crollo di un
anello e reroute.

---

## 10. Aggiunte al core — riepilogo

**Due** sole API nuove in `sim_particles.h`, entrambe data-parallel e senza
stato persistente nuovo:

1. `simp_query_ray(...)` — hitscan/piercing sulla griglia di collisione, con
   occlusione muri (§2). Cugino di `query_circle`/`nearest`.
2. `simp_set_vpref(s, i, v)` — cambio di velocità preferita post-spawn, per il
   crawl e i debuff (§5).

Tutto il resto del gameplay difensivo si appoggia a primitivi già verificati:
`apply_impulse` (spinta/gib), `corpse_add` (cadaveri), `wall_pressure`/`wall_cell`
+ `set_wall` (assedio/crollo della base), `set_goal` (attrazione), `query_circle`
(acquisizione nell'arco), slot map (dati per-slot). Le invarianti del core
restano: nessuna allocazione in `simp_step`, SoA piatti, determinismo,
mappabilità su compute.

---

## 11. Fuori scope — economia (doc separato)

Tenuto fuori di proposito, da documentare in dettaglio a parte (prassi del
progetto). Decisioni già prese in fase di design, qui solo per memoria:

- **Proiettili gratis**; la **biomassa** serve solo per **upgrade** e **attacchi
  speciali** (mortaio, bombardamenti) → niente spirale "uccidi per poter
  sparare" (la difesa base non si può auto-strozzare).
- **Biomassa** = blob alla morte (riga 21), modellati **come i cadaveri** (pool
  fisso + TTL: urgenza di raccolta + tetto alla memoria). In v1 **niente droni
  simulati**: il reddito è un *tasso/raggio di raccolta* attorno ai punti di
  raccolta (≈ contatore di morti pesato per prossimità). I droni visibili sono
  FX successivi.
- **Basi di raccolta avanzate** = punti di raccolta esterni alla base, **goal +
  HP** (riusano §7), che aumentano il reddito ma diventano bersaglio dell'orda:
  dilemma "espandi per l'economia vs difendi" → la biomassa nasce al fronte
  (dove si uccide), quindi il punto economicamente migliore è il più esposto.
  Richiede l'**attribuzione multi-goal** del drain/contatto (§7, differita).
- **Le torrette stesse possono raccogliere biomassa?** (idea: reddito garantito
  ovunque, le basi avanzate solo *di più*) — da decidere nel doc economia,
  insieme alla scelta se forzare l'espansione lasciando il core scarso di
  raccolta.
- **Muri/strutture costruite dal giocatore** (oltre alle torrette): la collisione
  è già SDF, fisicamente funzionano oggi; il sistema di costruzione/griglia è
  gameplay successivo.

---

## 12. Questioni aperte / trappole

- **Indici densi instabili**: gli hit di `simp_query_ray` muoiono alla prima
  kill — convertire in handle o uccidere in ordine decrescente (§6, M3.4).
- **Cadaveri & line-of-sight**: i cadaveri non sono colpiti dal raggio (sono
  morti) e non sono nella nav → non occludono il fuoco né deviano il flow via
  nav (solo PBD). Coerente con M3.3. Se in futuro un muro di corpi dovesse
  bloccare la vista, sarà una scelta esplicita (oggi no).
- **Dwell vs sweep**: il rate-limit del puntamento in dwell va tarato con
  `sweep_speed` e `fire_period` perché la pesante (rateo basso) non scavalchi i
  bersagli; tuning a occhio nel sandbox (§4).
- **Heading del renderer in assedio**: gli assedianti fermi devono usare
  `-grad(SDF)` per il facing, non l'EMA della velocità (SIEGE §5, già annotato
  in TODO M6) — vale anche per i feriti fermi.
- **Versione femminile del maimed_arm**: necessaria per coerenza outfit
  (riga 13) — lavoro di asset, non blocca il codice (il renderer può fallback al
  maschile finché non c'è).
- **`simp_set_vpref` e il jitter**: rimpiazza la velocità preferita dell'agente;
  se si vuole conservare il jitter relativo, applicarlo nel valore passato
  (`V_CRAWL · (1 + jitter)`), il core scrive il valore esatto.

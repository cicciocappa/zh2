# M3 — Fisica di gioco: design tecnico

Espansione operativa del punto M3 di `TODO.md`. Ogni sezione: soluzione tecnica,
schizzo di API, dettagli d'implementazione, come verificarla. L'ordine delle
sezioni è l'ordine d'implementazione consigliato (ogni step dipende dai precedenti).

Principio guida: il core resta gameplay-agnostico. HP, tipi, denaro, torrette
vivono nel codice di gioco; il core espone solo dischi, navigazione, query e
impulsi. Tutto ciò che si aggiunge deve restare mappabile su compute shader
(niente strutture a puntatori, solo array piatti).

---

## 3.1 Layer di handle (id stabili)

> **IMPLEMENTATO** come da design (+ accessor `simp_slot_of` per il pattern
> dati-di-gioco-per-slot). `test_handles` PASS.

**Problema.** `simp_kill` fa swap-and-pop: gli indici densi `0..count-1` cambiano
sotto i piedi. Il gameplay (torretta che "punta lo zombie 4711") ha bisogno di
riferimenti che sopravvivono alle rimozioni e rilevano il riuso.

**Soluzione: slot map con generation counter.** Tre array piatti, tutto O(1):

```c
typedef uint32_t SimPHandle;          /* 0 = invalid; bits 31..20 gen, 19..0 slot */
#define SIMP_HANDLE_INVALID 0u

/* internals */
int      *slot_to_index;   /* cap : slot  -> indice denso corrente (o -1)   */
int      *index_to_slot;   /* cap : indice denso -> slot                    */
uint16_t *slot_gen;        /* cap : generazione corrente dello slot         */
int      *slot_free;       /* cap : stack LIFO degli slot liberi            */
int       slot_free_top;
```

- **spawn**: pop di uno slot dal free stack (o slot nuovo se lo stack è vuoto);
  `slot_to_index[slot] = i; index_to_slot[i] = slot;`
  handle = `(gen[slot] << 20) | (slot + 1)` (il +1 riserva 0 come invalid).
- **kill(i)**: lo slot dell'agente ucciso fa `gen++` (con wrap, va bene) e torna
  nel free stack. Lo swap dell'ultimo agente in posizione `i` aggiorna le due
  mappe: `slot_to_index[index_to_slot[last]] = i; index_to_slot[i] = index_to_slot[last];`
- **resolve(h)**: estrai slot e gen; se `slot_gen[slot] != gen` → handle morto,
  ritorna -1. Altrimenti `slot_to_index[slot]`.

API pubblica:

```c
SimPHandle simp_handle_of(const SimP *s, int index);
int        simp_index_of (const SimP *s, SimPHandle h);   /* -1 se morto */
```

`simp_spawn` continua a ritornare l'indice (comodo per setup in massa); il
gameplay chiama `simp_handle_of` subito dopo se gli serve persistenza.

**Nota fondamentale per i dati di gioco**: gli array paralleli del gameplay
(HP, type, bounty…) vanno indicizzati per **slot**, non per indice denso — lo
slot è stabile per tutta la vita dell'agente. Pattern: `hp[slot_of(h)]`.

Limiti: 20 bit di slot = 1M agenti contemporanei, 12 bit di gen = il riuso di
uno slot va a collidere solo dopo 4096 ricicli dello stesso slot — accettabile;
se mai servisse, passare a handle a 64 bit è una riga.

**Verifica** (`test_handles.c`): spawn 1000, kill casuali al 50%, controllare
che ogni handle vivo risolva alla posizione giusta (confronto con shadow map
brute-force) e che ogni handle ucciso ritorni -1, anche dopo che lo slot è
stato riusato.

---

## 3.2 z fittizia (volo balistico)

> **IMPLEMENTATO.** Deviazioni: gli atterraggi sono riportati come **handle**
> (`simp_landed`), non indici — il drain nello stesso step li invaliderebbe;
> `dormant` è confluito negli `aflags` (bit `SIMP_DORMANT` + `SIMP_FLYING`);
> il recovery di velocità salta i volanti, e all'atterraggio si piega `qx/qy`
> così il recovery restituisce la velocità smorzata (altrimenti il damp
> verrebbe sovrascritto); drain valido anche in volo (annotato). I volanti
> sono clampati ai bordi del mondo anche in quota.

**Problema.** Le esplosioni devono *scagliare* gli zombie, non solo spingerli
sul piano. Serve un volo parabolico durante il quale l'agente non collide con
la folla e non viene controllato dal flow field.

**Soluzione: terzo asse posticcio + bit di stato.** Niente fisica 3D vera:

```c
float   *z, *vz;        /* cap : quota e velocità verticale (0 a terra)    */
uint8_t *aflags;        /* cap : bit 0 = SIMP_FLYING                       */
```

Modifiche allo step, nell'ordine esistente:

1. **Steering**: `if (aflags[i] & SIMP_FLYING) continue;` — in volo niente
   steering, niente damping (vola di moto proprio).
2. **Integrazione**: per tutti `x += vx·dt`; in più, per i volanti:
   `vz -= G·dt; z += vz·dt;` con `G = 9.81`. **Atterraggio** quando `z <= 0`:
   `z = 0; vz = 0; flag cleared; vx *= landing_damp; vy *= landing_damp;`
   (default `landing_damp = 0.3` — l'impatto uccide quasi tutto il momento
   orizzontale). Il gameplay può leggere un contatore/lista di atterraggi per
   applicare danno da caduta: esporre `simp_landed_count()` + buffer di indici
   atterrati nell'ultimo step.
3. **Counting sort**: i volanti **non vengono inseriti nella griglia** di
   collisione → automaticamente nessuna coppia li tocca e loro non toccano
   nessuno (passano sopra le teste). Costo zero.
4. **Wall projection**: saltata se `z > WALL_H` (default 2.0 m): sopra quella
   quota si sorvolano i muri; sotto, lo SDF respinge come sempre (uno zombie
   lanciato raso terra sbatte contro il muro — corretto e divertente).
5. **Recovery velocità**: invariato per i volanti (non subiscono proiezioni,
   quindi la velocità recuperata coincide con quella integrata).

L'impulso guadagna la componente verticale:

```c
void simp_apply_impulse_ex(SimP *s, float x, float y, float radius,
                           float strength, float up_ratio);
/* dv_orizzontale = strength·fall·dir ; vz += strength·fall·up_ratio ;
 * se vz risultante > soglia (es. 1 m/s) setta SIMP_FLYING.
 * simp_apply_impulse esistente = wrapper con up_ratio 0. */
```

Default sensato: `up_ratio 0.5` — un'esplosione da `strength 15` lancia il
centro del cratere a ~7.5 m/s verticali ≈ 2.8 m di apice, ~1.5 s di volo.

**Verifica** (estensione di `test_impulse.c`): dopo il blast, contare i flying;
controllare che durante il volo nessun volante generi coppie (overlap diag
invariato); che tutti atterrino entro `2·vz_max/G + ε`; che il drain non
scatti mentre si sorvola una goal cell SOLO se si decide così (più semplice:
il drain vale anche in volo, irrilevante per il gioco — annotarlo).

---

## 3.3 Cadaveri come ostacoli

> **IMPLEMENTATO.** Deviazione principale: invece del branch sul range di
> indici nel PBD, i cadaveri vengono copiati come **ghost** (`invm = 0`) in
> coda agli agenti negli array SoA prima del binning — il kernel PBD resta
> senza casi speciali (unica guardia: coppia ghost-ghost, somma masse nulle).
> Pool a rimpiazzo del più-vicino-a-scadenza invece del ring puro (convive
> con lo swap-and-pop dell'expiry). I cadaveri bloccano anche `simp_free_at`.
> `test_corpses` usa una barriera deterministica invece del cull al 25% (più
> robusto); la barricata emergente si verifica nel sandbox (pennello KILL).

**Problema.** I morti devono (a) lasciare un segno visivo, (b) per una frazione,
ostacolare fisicamente i vivi → barricate emergenti ai colli di bottiglia.

**Soluzione.** Il segno visivo è un decal: responsabilità del renderer, il core
non c'entra. L'ostacolo invece è un **disco passivo a massa infinita** dentro
la stessa griglia di collisione:

```c
/* corpse pool (ring buffer, capacità fissa, es. 4096) */
int    corpse_cap, corpse_count, corpse_head;
float *cpx, *cpy, *crad;
float *cttl;                       /* time-to-live in secondi */

int  simp_corpse_add(SimP *s, float x, float y, float radius, float ttl);
int  simp_corpse_count(const SimP *s);
```

Integrazione nel passo, quasi gratis:

- **Counting sort esteso**: dopo i `count` agenti, inserisci anche i cadaveri
  nelle celle. La convenzione negli indici di `corder`:
  `id < count` → agente; `id >= count` → cadavere `id - count`.
- **PBD**: nella risoluzione della coppia, se `j` è un cadavere usa
  `wj = 0` (inverse mass nulla) e non scrivere la sua posizione: il vivo
  riceve il 100% della correzione, il morto non si muove mai. Il ramo è un
  `if` sul range dell'indice, niente strutture nuove. I cadaveri non compaiono
  mai come `i` (il loop esterno itera solo sugli agenti? No: itera sulle celle —
  basta `if (id_i >= count) continue;` in testa).
- **TTL**: decremento una volta per step; alla scadenza, rimozione
  swap-and-pop nel pool (i cadaveri non hanno handle: il gameplay non ci punta).
  Se il pool è pieno, `corpse_add` sovrascrive il più vecchio (ring): il limite
  diventa una garanzia di costo, non un errore.
- **Interazione col flow field**: NON marcare i cadaveri come muri (ricalcolo
  Dijkstra continuo e ingorghi permanenti). La deviazione del flusso emerge già
  dalla PBD; se in M3.6 si aggiunge densità→costo, i cadaveri possono contare
  come densità fissa nella loro cella — un'unica riga nel binning.

Politica di spawn (lato gioco): alla kill, con probabilità `p_corpse` (default
0.15–0.3, da tarare), `simp_corpse_add(x, y, rad*0.9, 8.0 + rand*4)`.

**Verifica** (`test_corpses.c`): corridoio largo 4 m; uccidere a metà corridoio
il 25% degli agenti in transito trasformandoli in cadaveri TTL lungo; misurare
il throughput a valle prima/dopo: deve calare sensibilmente e i frame devono
mostrare il flusso che si biforca attorno al mucchio. Sanity: cadaveri immobili
(posizione iniziale == finale), zero NaN.

---

## 3.4 Query spaziali di gameplay

> **IMPLEMENTATO.** Deviazione: la griglia viene ricostruita a FINE step
> (il binning di metà step è stantio per le correzioni PBD/wall e il drain),
> quindi le query tra uno step e l'altro sono esatte, non approssimate; il
> fallback brute force resta per le chiamate fuori sequenza (guardia
> `grid_stale`/`grid_total`, non più `cstart[nc]==count`). `test_query` PASS
> contro brute force, inclusi i due pattern d'uso (handle, kill decrescente).

**Problema.** Torrette e AoE devono chiedere "chi c'è qui intorno?" senza
strutture dati proprie e senza O(N).

**Soluzione: esporre la griglia di collisione già esistente.** Due primitive:

```c
/* riempie out[] con gli INDICI degli agenti entro r da (x,y); ritorna quanti
 * (saturando a max_out). Solo vivi a terra; flag per includere i volanti. */
int simp_query_circle(const SimP *s, float x, float y, float r,
                      int *out, int max_out, uint32_t flags);

/* l'agente vivo più vicino a (x,y) entro r_max, o -1.
 * Scansione ad anelli di celle dal centro verso fuori; appena un anello
 * produce un candidato, si completa UN anello in più (il più vicino può
 * stare nell'anello successivo) e si chiude. */
int simp_query_nearest(const SimP *s, float x, float y, float r_max);
```

Dettagli:

- Entrambe camminano le celle della griglia di collisione (`cstart`/`corder`)
  con test di distanza esatto sul candidato. Costo ∝ area interrogata, non N.
- La griglia viene ricostruita dentro `simp_step`: tra uno step e l'altro è
  valida e coerente con le posizioni correnti — le query vanno chiamate
  *dopo* lo step, nella fase di gameplay del frame. Asserire
  `cstart[ncells] == count` (stessa guardia già usata da `apply_impulse`)
  e fare fallback brute-force in caso di chiamata fuori sequenza.
- **Pattern d'uso per il danno** (documentarlo, è una trappola): le query
  ritornano indici densi, validi solo finché non si killa. Il giro corretto:
  query → converti subito in handle (`simp_handle_of`) → applica danno via
  gameplay → per i morti, risolvi handle→indice e killa. In alternativa, se
  si killa direttamente dagli indici della query, farlo in **ordine
  decrescente di indice** (lo swap-and-pop porta in posizione `i` solo
  indici > i, mai uno ancora da processare).

**Verifica** (`test_query.c`): 10k agenti random; per 100 query circle/nearest
casuali, confronto col brute force O(N). Poi un mini-scenario torretta:
nearest + kill ripetuti, controllo che non si killi mai due volte lo stesso
indice e che il count scenda del numero giusto.

---

## 3.5 Tipi di nemico

> **IMPLEMENTATO.** Deviazioni: `mass` è in **unità walker** (1.0 = agente a
> raggio default; gli spawn default restano `invm = 1/r²`, il desc usa
> `invm = 1/(mass·R0²)` — stessa scala, il PBD usa solo rapporti); `v_pref`
> riceve comunque il `v_jitter` globale (anti-lockstep), raggio e massa sono
> presi esatti; aggiunto `simp_sleep` (branchi dormienti tipizzati); uno
> spawn più grande del raggio default **ingrandisce la cella della griglia
> di collisione** (la ricerca coppie a 5 celle vede solo dischi che ci
> stanno; vale anche per `simp_corpse_add`); `cost_user` clampato a
> [-0.8, 100] in scrittura. `test_types` PASS (corsa runner/walker, pressing
> tank al varco — il goal a ridosso del bordo deve essere profondo ≥ 2 celle
> o i tank non ci arrivano col centro — e repulsione/attrazione via
> `simp_add_cost`).

**Problema.** Tank, runner, screamer — senza sporcare il core con nozioni di
gameplay.

**Soluzione.** Il core ha già tutto ciò che serve come *parametri per-agente*
(raggio, velocità preferita, massa): basta uno spawn esteso che li riceve
invece di derivarli dai default + jitter:

```c
typedef struct {
    float radius;        /* m   */
    float v_pref;        /* m/s */
    float mass;          /* kg-ish; il core usa 1/mass come invm */
} SimPAgentDesc;

int simp_spawn_desc(SimP *s, float x, float y, const SimPAgentDesc *d);
```

Tabella tipi e tutto il resto (HP, bounty, AI speciali) vivono nel gioco,
in array paralleli **indicizzati per slot** (vedi 3.1). Esempi di descrittori:

| tipo     | radius | v_pref | mass | effetto emergente via PBD                    |
|----------|--------|--------|------|----------------------------------------------|
| walker   | 0.30   | 1.4    | 1.0  | baseline                                     |
| runner   | 0.27   | 2.8    | 0.9  | filtra negli interstizi, arriva per primo    |
| tank     | 0.55   | 1.0    | 10.0 | `invm` 10× più piccolo: spinge tutti, apre corridoi nella folla che i walker riempiono dietro — gratis |
| screamer | 0.30   | 1.6    | 1.0  | vedi sotto                                   |

**Screamer / modifiche locali alla navigazione.** Servono perturbazioni del
campo di costo (urlo che attira, paura che respinge, fango che rallenta).
Soluzione: un campo di costo additivo per cella nav, già predisposto per 3.6:

```c
void simp_add_cost(SimP *s, int cx, int cy, float w);   /* w>0 evita, w<0 attira… */
void simp_clear_cost(SimP *s);
```

…dove il Dijkstra usa `edge = dist · (1 + cost[dest])` con `cost` clampato a
≥ una soglia > -1 (mai costi negativi assoluti: Dijkstra li richiede ≥ 0; per
"attirare" si abbassa il costo relativo delle celle target, clamp a es. -0.8).
Il ricalcolo resta lazy/dirty; lo screamer scrive un disco di costo e marca
dirty con throttle (vedi 3.6 per la frequenza).

**Verifica**: scena con muro e varco unico; 5% tank nel mix; verificare a
frame che i tank avanzano nel pressing senza venire deviati e che i runner
sopravanzano la massa. Niente metrica rigida qui: è tuning a occhio nel
sandbox, i test automatici controllano solo sanity (no NaN, drain > 0).

---

## 3.6 Densità → costo Dijkstra (Continuum Crowds light)

> **IMPLEMENTATO.** Deviazioni: l'istogramma densità non vive in
> `rebuild_grid` ma in una passata dedicata (`density_update`) eseguita solo
> al tick di ricalcolo (blur 3×3 ed EMA fusi in un'unica passata); niente
> `nav_commit_flow` come API separata — il throttle (`flow_period`) è
> interno a `simp_step`, e le modifiche al terreno continuano a forzare il
> commit completo immediato. Scratch del nav preallocati in `create` (heap
> Dijkstra, blur del flow, moltiplicatori): niente malloc dentro `simp_step`.
> Default: `k_density = 2.0`, `flow_period = 0.5 s`. `test_density_route`
> PASS: uso del percorso lungo 22% → 57%, drain del 90% in 5382 → 4552 step;
> in `test_particles` il drain sale 71% → 82% (l'orda si spalma sui due
> varchi da sola).

**Problema.** Con flow field puro tutta l'orda converge sullo stesso percorso
ottimo e si accoda; le folle vere (e i giochi belli) si allargano sugli
ingorghi.

**Soluzione: il costo degli archi cresce con la densità locale.**

1. **Binning della densità**: durante `rebuild_grid` si fa già il giro di tutti
   gli agenti; aggiungere un secondo istogramma sulle celle **nav** (un
   `float *rho_nav`, incremento per agente, +costo fisso per cadavere).
   Una passata di blur 3×3 (o due) per togliere il rumore.
2. **EMA temporale** per stabilità: `rho_s = rho_s·(1-α) + rho·α` con α ≈ 0.3
   per tick di ricalcolo — evita che il campo "sfarfalli" tra due percorsi.
3. **Costo**: `edge(a→b) = dist · (1 + k_d · min(rho_s[b]/rho_max, 1) + cost_user[b])`
   con `k_d` ≈ 1.5–3 (manopola) e `rho_max` = densità di impaccamento della
   cella (≈ cell²/(π r²) · 0.7). Il termine `cost_user` è quello di 3.5.
4. **Frequenza**: spezzare `nav_commit` in due:
   - `nav_commit_terrain` → SDF (+phi+flow), solo quando cambiano muri/goal;
   - `nav_commit_flow` → solo phi+flow, su un timer (`flow_period`, default
     0.5 s) quando densità o cost_user sono cambiati.
   Il Dijkstra su 320×240 costa pochi ms: a 2 Hz è invisibile nel budget. Se
   in M4 desse fastidio, si sposta su un thread (gli agenti possono campionare
   il campo vecchio per qualche frame senza alcun problema visivo).

**Effetto atteso e verifica** (`test_density_route.c`): scena con due percorsi
verso il goal, uno corto con varco stretto e uno lungo e largo. Con `k_d = 0`
tutti si accodano al varco corto; con `k_d = 2` una frazione sostanziale
(misurare: conteggio dei passaggi per cella su una linea di taglio) devia sul
percorso lungo e il tempo di drain totale DEVE scendere. Questo è anche il
test di regressione perfetto per il tuning di `k_d`.

---

## Ordine, dipendenze, stima

```
3.1 handles ──► 3.4 query ──► (gameplay: torrette/danno)
3.2 z-flight ──┐
3.3 corpses ───┼─► sandbox tuning
3.5 types ─────┤
3.6 density ───┘   (3.5 e 3.6 condividono cost_user)
```

Sessioni Claude Code suggerite: (1) 3.1+3.4 con i loro test — sono pura
contabilità, zero rischio; (2) 3.2+3.3 — toccano lo step, rifare girare TUTTI
i test esistenti; (3) 3.5+3.6 — chiudere con il sandbox per il tuning.

Ogni aggiunta mantiene le invarianti del core: nessuna allocazione in
`simp_step` (i pool si allocano in `create`), array piatti SoA, determinismo
(l'unico RNG resta xorshift), e ogni nuovo passo (binning densità, TTL
cadaveri, integrazione z) è un kernel data-parallel mappabile su compute.

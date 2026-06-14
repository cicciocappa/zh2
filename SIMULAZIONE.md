# Come funziona la simulazione

Documento esplicativo del core di simulazione particellare (`sim_particles.h` /
`sim_particles.c`). Due parti: prima la **teoria** generale (cosa stiamo
simulando e perché in questo modo), poi la **lettura del codice** funzione per
funzione, variabile per variabile.

> Nota di contesto. Il progetto ha due core complementari. Il *core continuo*
> (`sim.h`/`sim.c`, nella cartella del progetto precedente) tratta l'orda come un
> campo di densità su griglia (shallow-water + avvezione): servirà da LOD per le
> orde lontane. Questo documento riguarda il **core particellare**, quello su cui
> si lavora ora: ogni nemico è un disco 2D simulato come materiale granulare.

---

## Parte I — Teoria

### 1. Il problema

Vogliamo simulare orde massive (target 50k–100k nemici) che attraversano una
mappa verso uno o più obiettivi, in un tower defense top-down "fisico". I
requisiti che danno forma a tutto il resto:

- **Massa enorme di agenti** → ogni operazione per-agente deve essere O(1) o
  ammortizzata; niente algoritmi che scalano col numero di coppie.
- **Comportamento collettivo credibile** → l'orda deve aggirare gli ostacoli,
  ingorgarsi ai colli di bottiglia, spingere come una folla reale, accumularsi
  contro i muri (assedio). Non bastano agenti che seguono ciecamente una rotta.
- **Stabilità incondizionata** → con decine di migliaia di dischi che si
  schiacciano, un solver fisico classico (forze → integrazione) esplode. Serve
  un metodo stabile a qualsiasi densità di impacchettamento.
- **Determinismo** → stessa sequenza di chiamate ⇒ stesso risultato (test,
  replay, debugging).
- **Portabilità a GPU** → ogni fase deve poter diventare un dispatch di compute
  shader. Niente strutture dati che lo impediscano.

### 2. I tre ingredienti

La simulazione di ogni agente si scompone in tre livelli, dal globale al locale:

**(1) Navigazione globale — un flow field.**
Tutti gli agenti condividono un unico campo di direzioni calcolato una volta per
tutta la griglia, non per-agente. Si parte da una **Dijkstra** a 8 vicini dalle
celle-obiettivo, che assegna a ogni cella un potenziale `phi` = costo per
raggiungere il goal. Dal `phi` si ricava un campo di direzioni (verso il vicino a
costo minore), lo si smussa e normalizza. Ogni agente campiona questo campo in
modo bilineare e sa dove andare. Costo: O(griglia), condiviso da tutti — non
dipende dal numero di agenti.

Trucco chiave: **i muri sono attraversabili a costo enorme** (`WALL_ENTER`),
non bloccati. Dove esiste una rotta aperta vince sempre (un giro vale molto meno
del pedaggio). Ma un goal completamente murato continua ad attrarre: `phi`
decresce comunque verso di esso attraverso i muri, e l'orda va a *premere contro
le mura* invece di ignorare l'obiettivo. È così che nasce l'**assedio** senza
codice dedicato.

Gli archi della Dijkstra sono inoltre **pesati per congestione** (Continuum
Crowds "light"): una cella affollata, o piena di folla *ferma*, costa di più, e
il Dijkstra devia l'orda da sola attorno agli ingorghi. Vedi §I.4.

**(2) Dinamica locale — steering.**
Ogni agente accelera verso la direzione del flow alla propria velocità
preferita, con **accelerazione limitata** (`a_max`) e un po' di **rumore
angolare** per-step. La velocità preferita e il raggio hanno un *jitter*
per-agente. Tutto questo serve a rompere il *lockstep*: senza, migliaia di
agenti si muoverebbero in modo identico e l'orda sembrerebbe un cristallo. Con,
si ottiene il "ribollire" organico.

**(3) Non-compenetrazione — PBD (Position-Based Dynamics).**
Invece di applicare forze repulsive (instabili), si lavora direttamente sulle
**posizioni**: due dischi che si sovrappongono vengono separati spostandoli, in
proporzione alla massa inversa. Si itera qualche volta (Gauss-Seidel). Questo è
*incondizionatamente stabile* a qualsiasi densità. La cosa elegante: la
**velocità effettiva** non è quella che abbiamo integrato, ma viene *recuperata*
dallo spostamento reale, `v = (x_nuovo − x_vecchio)/dt`. Così la spinta della
folla si propaga come **momento reale**: se ti spingono, ti muovi davvero, e
quella velocità ti accompagna allo step dopo.

I muri, nel PBD, sono gestiti da un **campo di distanza con segno (SDF)**
precalcolato: ogni agente che entra nel muro viene rispinto lungo il gradiente
della distanza.

### 3. Perché PBD e non un solver a forze

Un solver a forze (molle repulsive tra dischi) ha una rigidità: troppo morbido e
gli agenti si compenetrano, troppo rigido e il passo d'integrazione esplode. Con
50k dischi impaccati al collo di bottiglia, "troppo rigido" è inevitabile. Il
PBD aggira il problema: proietta le posizioni a non-sovrapposte e ricava la
velocità a posteriori. Non c'è una costante di rigidità che possa divergere;
l'unico parametro è *quante iterazioni* fai (`pbd_iters`), che regola se la folla
è morbida/comprimibile (1–2) o un impacchettamento granulare rigido (4+).

I **guardrail** servono comunque: si limita la correzione per coppia al 30% del
raggio combinato per iterazione, e si mette un tetto `v_clamp` sulla velocità
recuperata. Senza, una sovrapposizione profonda (spawn troppo denso, uno
schiacciamento) produrrebbe spostamenti enormi in un solo step → velocità
balistiche (osservato: 160 m/s). Con i guardrail, le sovrapposizioni profonde si
rilassano in qualche step invece di esplodere.

### 4. Densità, ingorghi e l'assedio

Due raffinamenti sulla navigazione, entrambi implementati pesando gli archi
della Dijkstra per cella di destinazione:

- **Densità → costo** (`k_density`): si tiene un istogramma di quanti agenti
  stanno in ogni cella nav, lo si sfuma (blur 3×3) e lo si smorza nel tempo
  (EMA). Le celle affollate costano di più. Effetto: l'orda *si spalma* su rotte
  alternative invece di accalcarsi tutta sullo stesso varco. Satura a `k_density`
  (una cella piena non può costare infinito).

- **Ingorgo → costo** (`k_jam`): stesso istogramma, ma ogni agente contribuisce
  con la sua *fermezza* `(1 − |v|/v_pref)²` invece di 1. La densità da sola non
  sa distinguere "varco pieno ma che scorre" da "varco tappato a portata zero"
  (un cadavere/tappo che blocca tutto: costo reale in tempo = infinito, ma la
  densità lo vede solo come `k_density`). Il jam rende cara *solo la folla
  ferma*, abbastanza da far deviare il Dijkstra anche su strade molto più lunghe.
  Quando il tappo si scioglie, l'EMA decade e la rotta diretta torna a vincere.

Entrambi restano **sotto** `WALL_ENTER`: gli assedi ai goal murati non cambiano.

L'**assedio** emerge dalla combinazione: il goal murato attrae (phi attraverso i
muri) → gli agenti premono contro la parete → il PBD li blocca lì → un *sensore*
(§II.10) misura chi preme e dove → il gioco assegna HP alle strutture e le fa
crollare → al crollo la rotta si ricalcola e l'orda dilaga.

### 5. Il ciclo di vita di uno step

Ad ogni `simp_step(dt)` (timestep fisso 1/60), in ordine:

1. **Navigazione** (lazy/throttled): se il terreno è cambiato, ricalcola tutto
   (phi+flow+SDF); altrimenti, ogni `flow_period` secondi, aggiorna densità e
   ricalcola phi+flow se la congestione o i costi sono cambiati.
2. **Steering**: ogni agente accelera verso il flow (+rumore), accelerazione
   limitata, smorzamento.
3. **Integrazione**: salva la posizione pre-proiezione (`qx,qy`), avanza la
   posizione; per chi vola, integra l'asse z balistico.
4. **Vincoli iterati**: ricostruisce la griglia di collisione, poi `pbd_iters`
   volte fa una passata PBD + una proiezione contro i muri.
5. **Recupero velocità**: `v = (x − x_prev)/dt`, con clamp.
6. **Drain**: chi è su una cella-goal viene rimosso (e contato).
7. **Decadimento cadaveri** + **ricostruzione finale della griglia** (per query
   e impulsi coerenti tra uno step e l'altro).

---

## Parte II — Lettura del codice

Da qui in poi tutto fa riferimento a `sim_particles.c` e `sim_particles.h`. Lo
stato vive nella `struct SimP` (riga 41). Convenzione: **SoA** (Structure of
Arrays) — ogni attributo è un array separato indicizzato per agente, perché i
loop caldi scorrono un attributo alla volta (cache-friendly) e gli array
posizione/velocità sono pronti come *instance buffer* per il renderer.

### 0. Unità, costanti, RNG

- Unità: **metri, secondi**. Default umani: raggio `0.30` m, velocità `1.4` m/s
  (impostati in `simp_create`, righe 402–415).
- Costanti (righe 30–37): `PHI_INF` (1e30, "irraggiungibile"), `GRAV` (9.81),
  `CORPSE_CAP` (4096, pool fisso cadaveri), `TAKEOFF_VZ` (1.0, soglia di
  decollo), `COST_MIN/MAX` (clamp del costo utente), `RHO_EMA` (0.3, guadagno
  EMA densità), `CORPSE_RHO` (2.0, un cadavere "pesa" come 2 agenti nella
  densità).
- **RNG deterministico**: `rng_next` (riga 18) è uno xorshift32; `rng_f01` dà
  [0,1), `rng_fsym` dà [−1,1). Niente `rand()` di libc. C'è un RNG a livello di
  sim (`s->rng`, per il jitter di spawn) e uno **per-agente** (`s->seed[i]`, per
  rumore e jitter): così l'ordine di chiamata determina il risultato.

### 1. Lo stato: `struct SimP` (riga 41)

Raggruppato per ruolo:

**Griglia nav** (righe 42–53): `gw,gh` (dimensioni in celle), `cell`/`inv_cell`
(lato cella e reciproco), `world_w/world_h` (estensione in metri). Poi gli array
`gw*gh`:
- `solid` — 1 = muro;
- `goal` — 1 = cella drenante;
- `phi` — costo-verso-goal (output Dijkstra);
- `flow_x/flow_y` — campo di direzioni normalizzato (0,0 nei muri);
- `sdf` — distanza con segno dai muri (>0 = spazio libero).
- `nav_dirty` — flag: il terreno è cambiato, serve ricommittare.

**Costi di navigazione** (righe 55–64): `cost_user` (costo additivo del
giocatore, clampato in scrittura), `rho_raw`/`rho_s` (istogramma densità grezzo /
smussato), `jam_raw`/`jam_s` (idem ma pesato per fermezza), `cost_mult`
(moltiplicatore d'arco finale, riempito a ogni ricalcolo), `cost_dirty`,
`rho_active` (la densità ha ancora massa non trascurabile), `flow_timer` (per il
throttle).

**Scratch nav preallocato** (righe 66–68): `heap_nodes` (lo heap della Dijkstra,
dimensionato `gw*gh*8`), `flow_tx/flow_ty` (buffer per la passata di
smoothing). Preallocati una volta **perché in `simp_step` non si deve mai fare
`malloc`**.

**Agenti (SoA)** (righe 70–80):
- `px,py` — posizioni; `qx,qy` — posizioni *precedenti* (pre-proiezione), il
  cuore del recupero velocità;
- `vx,vy` — velocità;
- `rad` — raggio per-agente; `vpref` — velocità preferita per-agente;
- `invm` — **massa inversa** (≈ 1/r²): il PBD usa solo rapporti di massa;
- `seed` — stato RNG per-agente;
- `aflags` — byte di flag comportamentali (`SIMP_DORMANT`, `SIMP_FLYING`);
- `z,vz` — asse fittizio per il volo balistico.

**Altri buffer**: `landed`/`landed_n` (chi è atterrato in questo step, come
*handle*), `wall_pressure`/`wall_cell` (sensore d'assedio), il **pool cadaveri**
(`cpx,cpy,crad,cttl` + `corpse_count`, `corpse_rmax`), la **slot map** per gli
handle (`slot_to_index`, `index_to_slot`, `slot_gen`, `slot_free`,
`slot_free_top`), e la **griglia di collisione** (`cgw,cgh`, `ccell`, `ccount`,
`cstart`, `corder`, `grid_total`, `grid_ghosts`, `grid_stale`). Infine `params`,
`rng`, e due contatori diagnostici per l'overlap medio.

> **Dettaglio importante** (righe 438–451 in `simp_create`): gli array
> `px,py,rad,invm,seed,corder` sono allocati con `CORPSE_CAP` slot **in più**
> oltre `max_agents`. I cadaveri vengono appesi come "ghost" in coda agli agenti
> prima del binning, con `invm = 0`. Così il kernel PBD non ha casi speciali per
> i cadaveri: sono dischi a massa infinita come gli altri.

### 2. Lifecycle: `simp_create` / `simp_destroy`

`simp_create` (riga 394) fa `calloc` della struct, imposta i **default dei
parametri** (righe 402–415, vedi tabella sotto), alloca tutti gli array, prefila
la slot map (tutti gli slot liberi, in modo che le prime estrazioni diano
0,1,2…), e dimensiona la **griglia di collisione**: lato cella = `2 × raggio
massimo plausibile × 1.05` (riga 483). La scelta del lato è cruciale per il PBD
(vedi §II.7). `nav_dirty = true` forza il primo commit. `simp_destroy` libera
tutto.

I parametri (`SimPParams`, header righe 115–130) con i default:

| Campo | Default | Ruolo |
|---|---|---|
| `v_max` | 1.4 | velocità di punta desiderata (m/s) |
| `v_jitter` | 0.25 | dispersione velocità per-agente (frazione) |
| `a_max` | 8.0 | accelerazione di steering massima (m/s²) |
| `radius` | 0.30 | raggio base agente (m) |
| `r_jitter` | 0.15 | dispersione raggio per-agente (frazione) |
| `noise_ang` | 0.06 | semi-angolo del rumore di steering (rad) |
| `damping` | 0.10 | smorzamento velocità al secondo |
| `v_clamp` | 20.0 | tetto sulla velocità recuperata (m/s) |
| `pbd_iters` | 3 | iterazioni PBD per step (rigidità folla) |
| `landing_damp` | 0.30 | momento orizzontale tenuto all'atterraggio |
| `wall_h` | 2.0 | quota di volo che scavalca i muri (m) |
| `k_density` | 2.0 | guadagno densità→costo (0 = off) |
| `k_jam` | 8.0 | guadagno folla-ferma→costo (0 = off) |
| `flow_period` | 0.5 | secondi minimi tra due ricalcoli del flow |

`simp_params` (riga 520) restituisce il puntatore: i parametri sono
modificabili a runtime.

### 3. Terreno e costi

- `simp_set_wall` / `simp_set_goal` (righe 524, 529) scrivono `solid`/`goal` e
  alzano `nav_dirty`.
- `simp_is_wall` (riga 534): fuori griglia = muro (comodo per i bordi).
- `simp_terrain_commit` (riga 538) chiama `nav_commit` subito: usato quando una
  modifica al terreno deve avere effetto immediato (senza aspettare il throttle).
- `simp_add_cost` (riga 540) accumula sul `cost_user` di una cella, clampando a
  `[COST_MIN, COST_MAX]` = `[−0.8, 100]`: `w>0` respinge (paura, fango), `w<0`
  attrae (richiamo screamer). Mai negativo (archi sempre positivi), mai
  competitivo con `WALL_ENTER`. Alza `cost_dirty`.
- `simp_clear_cost` azzera tutto.

### 4. Navigazione: la Dijkstra (`recompute_phi`, riga 163)

È il cuore della navigazione globale.

1. Inizializza tutti i `phi` a `PHI_INF`.
2. **Costruisce il moltiplicatore d'arco per cella** (righe 167–185) in
   `cost_mult[i]`:
   ```
   m = 1 + cost_user[i]
       + k_density · min(rho_s[i]/rho_max, 1)     // se k_density > 0
       + k_jam     · min(jam_s[i]/rho_max, 1)     // se k_jam > 0
   cost_mult[i] = max(m, 0.2)                       // mai sotto 0.2
   ```
   `inv_rho_max` (riga 171) è il reciproco della densità di impacchettamento di
   una cella: `(π·r0²)/(cell²·0.7)`, cioè quanti agenti di raggio default ci
   stanno (con fattore di riempimento 0.7).
3. **Heap min-binario** (`heap_push`/`heap_pop`, righe 129–152) sullo scratch
   preallocato `heap_nodes`. Inietta tutte le celle-goal non-muro a costo 0.
4. **Rilassamento a 8 vicini** (righe 197–212): estrae il minimo, salta le
   *entry stantie* (riga 199: `nd.c > phi[nd.i]`), e per ogni vicino calcola
   `nc = costo_corrente + DC[k]·cost_mult[j]`, dove `DC[k]` è 1 per gli ortogonali
   e √2 per i diagonali.
   - Se il vicino è muro: `nc += WALL_ENTER` (5000). I muri sono attraversati,
     non bloccati.
   - **Anti-corner-cutting** (righe 208–209): un movimento diagonale che taglia
     l'angolo di due muri paga anch'esso il pedaggio.

Il risultato è `phi`: ogni cella libera ha il costo minimo per arrivare al goal,
e ogni cella murata ha un costo altissimo ma finito (→ assedio).

`WALL_ENTER` (riga 161) è dimensionato perché qualsiasi deviazione su mappa
(phi è in unità-cella, la griglia è poche centinaia di celle) costi molto meno
di un singolo attraversamento di muro.

### 5. Dal potenziale alle direzioni (`recompute_flow`, riga 215)

1. Per ogni cella libera con `phi` finito, guarda gli 8 vicini e prende la
   direzione verso quello a `phi` **minimo** (righe 218–242). I vicini-muro
   restano "in gara": vincono solo quando il nostro stesso `phi` è passato
   *attraverso* quel muro (tasca sigillata) → il flow punta dentro il muro →
   assedio. Sulle rotte aperte il phi del muro è più alto di ≥`WALL_ENTER`, quindi
   non vince mai. Anche qui c'è l'anti-corner-cutting (riga 235).
2. **Una passata di smoothing** (righe 244–263): media le direzioni dei vicini
   non-muro nei buffer `flow_tx/flow_ty`, rinormalizza, e ricopia in
   `flow_x/flow_y`. Smussare evita gli scatti a 45° tipici di una griglia a 8
   vicini, dando traiettorie più morbide.

### 6. Densità e jam (`density_update`, riga 272)

Chiamata a throttle (vedi §II.13). Ricostruisce gli istogrammi **da zero** ogni
volta:

1. Azzera `rho_raw` e `jam_raw`.
2. Per ogni agente **a terra** (salta i volanti, riga 277): incrementa
   `rho_raw[cella] += 1` e accumula in `jam_raw` la **fermezza al quadrato**
   `(1 − |v|/vpref)²` (righe 286–288). La velocità usata è quella *recuperata*,
   quindi la pressione della folla conta. Al quadrato perché una folla
   lenta-ma-che-scorre quasi non pesa, mentre una coda ferma pesa pieno.
3. I **cadaveri** (righe 290–295) contano `CORPSE_RHO` (2) sia in densità che in
   jam: i cadaveri leggono come "fermi a peso doppio", e il flow devia attorno ai
   mucchi.
4. **Blur 3×3 + EMA** (righe 296–311): per ogni cella media i 9 vicini e fonde
   col valore vecchio: `rho_s = rho_s·(1−RHO_EMA) + media·RHO_EMA`. L'EMA dà
   ~2 periodi di ritardo, ed è esattamente ciò che impedisce al flow di
   *sfarfallare* tra due rotte.
5. `rho_active` (riga 314) resta vero finché c'è massa residua: tiene vivo il
   ricalcolo a throttle finché una mappa svuotata non si è smorzata del tutto.

### 7. Campo di distanza dai muri (`recompute_sdf`, riga 318)

Un **chamfer distance transform** a due passate (forward + backward), metrica 3-4
scalata in metri (ortogonale = `cell`, diagonale = `cell·√2`). Risultato in
`sdf`: distanza dal muro più vicino. Le celle-muro ricevono `−0.5·cell` e le
libere `distanza − 0.5·cell` (righe 343–345): lo shift di mezza cella approssima
la distanza dalla *faccia* del muro, dato che il muro occupa tutta la sua cella.

`nav_commit` (riga 348) incatena `recompute_phi → recompute_flow →
recompute_sdf` e azzera `nav_dirty`.

### 8. Campionamento bilineare

- `bilinear` (riga 357): interpolazione bilineare di un campo griglia in un punto
  mondo, con offset di −0.5 cella (i valori stanno al centro delle celle).
- `simp_sample_flow` (riga 370): campiona `flow_x/flow_y` e rinormalizza (0,0 nei
  muri).
- `simp_sample_sdf` (riga 378): la distanza interpolata.
- `sdf_grad` (riga 383): gradiente dell'SDF a differenze centrali → la normale
  che punta *fuori* dal muro. È la direzione di reiezione del PBD-muro.

### 9. Spawn, kill, e gli handle stabili

**Spawn.** `spawn_common` (riga 560) fa il lavoro condiviso: controlla capacità
e che non sia dentro un muro, inizializza posizione (con `qx=qy=px=py`),
velocità nulla, deriva il seed per-agente, e **prende uno slot** dalla pila dei
liberi aggiornando `slot_to_index`/`index_to_slot`. Poi:
- `simp_spawn` (riga 580): applica `r_jitter` e `v_jitter`, e imposta
  `invm = 1/r²` (massa ≈ r²).
- `simp_spawn_dormant` (riga 591): come sopra + flag `SIMP_DORMANT`.
- `simp_spawn_desc` (riga 597): spawn **tipizzato** (M3.5) con raggio, `v_pref` e
  massa espliciti. La massa è in "unità walker" (1.0 = agente default):
  `invm = 1/(massa·r0²)`. Il PBD usa solo rapporti → un tank massa 10 spinge i
  walker e quasi non viene deviato. Se il raggio supera quello per cui la griglia
  è dimensionata (`grid_rmax`), **ingrandisce la cella** della griglia (righe
  612–619): corretto ma più lento per tutti, quindi boss entro ~2× il default.

**Kill.** `simp_kill` (riga 629) è **swap-and-pop**: incrementa la *generation*
dello slot (così gli handle vecchi diventano invalidi), libera lo slot, e copia
l'**ultimo** agente sopra l'indice rimosso (tutti gli array SoA, incluso il
sensore d'assedio, righe 644–645). Marca `grid_stale`. **Conseguenza: gli indici
densi NON sono stabili.** Per riferimenti persistenti servono gli handle.

**Handle** (header righe 200–215, impl. righe 715–732). Una *slot map* con
generation counter sopra lo swap-and-pop:
- `simp_handle_of` (riga 715): impacchetta `(generation 12 bit << 20) | (slot+1)`.
  0 resta invalido.
- `simp_slot_of` (riga 721): lo slot, **stabile per tutta la vita dell'agente** →
  i dati di gioco per-agente (HP, tipo…) vanno indicizzati per slot.
- `simp_index_of` (riga 726): risolve un handle all'indice denso corrente, o −1
  se l'agente è morto (anche dopo il riciclo dello slot, grazie alla
  generation).

`simp_free_at` (riga 653): dice se un disco di raggio `r` in `(x,y)` ci sta senza
toccare agenti, cadaveri o muri (SDF). Usa la griglia se attuale, altrimenti
brute force. Spawnare *solo* dove questo è vero elimina l'espulsione PBD allo
spawn e fa auto-strozzare gli emitter alla portata dell'uscita (flusso da
tunnel).

**Sveglia dormienti**: `simp_wake_radius` (riga 699) toglie il flag a chi è nel
raggio; `simp_wake_all` (riga 708) a tutti. `simp_sleep` (riga 623) è la
controparte.

### 10. La griglia di collisione (`rebuild_grid`, riga 903)

Griglia uniforme ricostruita **ogni step** con un **counting sort** (due passate
sugli agenti, niente malloc):

1. **Appende i cadaveri come ghost** (righe 906–914) in coda agli agenti negli
   array SoA, con `invm = 0` e un seed deterministico.
2. Conta gli agenti per cella in `ccount` (salta i **volanti**, riga 919: non
   binnati).
3. Prefix sum → `cstart` (inizio di ogni cella in `corder`).
4. Riempie `corder` (entry ordinate per cella) usando `ccount` come cursore.
5. Aggiorna `grid_total` (= numero agenti a quel rebuild) e `grid_stale = false`.

Il **lato cella** (`ccell`) è `2 × r_max × 1.05`: così tutte le coppie che si
possono sovrapporre stanno nella stessa cella o in una adiacente, e il PBD può
limitarsi a guardare poche celle vicine.

### 11. Il PBD (`pbd_iteration`, riga 939)

Gauss-Seidel sulle celle della griglia di collisione. Per ogni agente guarda la
**propria cella + 4 vicine in avanti** (E, SW, S, SE — array `NX/NY` righe
943–944): così ogni coppia è visitata **una sola volta** (riga 957: nella
propria cella parte da `a+1`). Per ogni coppia sovrapposta:

1. Calcola sovrapposizione e normale (con fallback casuale deterministico se i
   centri coincidono, righe 969–975).
2. **Guardrail**: la correzione per coppia per iterazione è cappata al 30% del
   raggio combinato (`capd`, righe 981–982).
3. **Sposta in proporzione alla massa inversa** (righe 983–987):
   `ci = overlap·wi/(wi+wj)`, `cj = overlap·wj/(wi+wj)`. Un disco a `invm=0`
   (cadavere) non si muove: l'altro assorbe tutta la correzione. La guardia alla
   riga 966 salta le coppie ghost-ghost (`wi+wj ≤ 0`).
4. Accumula la diagnostica di overlap (per `simp_mean_overlap`).

### 12. Muri e sensore d'assedio (`wall_projection`, riga 1004)

Eseguita dopo ogni iterazione PBD. Per ogni agente a terra (o volante sotto
`wall_h`): se l'SDF locale è minore del raggio, è dentro il muro → lo respinge di
`push = r − d` lungo il gradiente SDF (righe 1010–1047). Poi clampa ai **bordi
del mondo** (righe 1051–1052).

Sull'**ultima** iterazione (`record == true`) campiona il **sensore d'assedio**
(`SIEGE_DESIGN.md`), *prima* che la proiezione cancelli la penetrazione (è
l'unico momento in cui `push` è ancora significativo):
- `into_wall = −(flow · normale_SDF)` (riga 1019): >0 = l'agente *sterza dentro*
  il muro (assedio frontale); ≤0 = lo sfiora di tangente (grazing, futura
  meccanica hazard).
- Se `into_wall > 0`, cerca in una finestrella la **cella muro** meglio allineata
  col `−gradiente` (righe 1026–1039) e scrive `wall_pressure[i] = push·into_wall`
  e `wall_cell[i] = cella` (righe 1040–1043).

Il sensore espone *chi* preme un muro per arrivare al goal oltre, e *dove*. Il
gioco ci costruisce sopra HP delle strutture + attacchi discreti, e al crollo
chiama `simp_set_wall(false)` → reroute automatico.

### 13. Lo step completo (`simp_step`, riga 1056)

L'orchestratore. In ordine:

**Navigazione** (righe 1061–1078). Se `nav_dirty` (terreno cambiato):
`nav_commit` completo subito. Altrimenti, **throttle**: accumula `flow_timer`; al
superamento di `flow_period`, se la densità è attiva o i costi sono cambiati,
fa `density_update + recompute_phi + recompute_flow` (non l'SDF, che cambia solo
col terreno). Così la Dijkstra resta fuori dall'hot path: gli agenti leggono un
campo leggermente stantio tra un ricalcolo e l'altro, invisibile in pratica.

**(1) Steering** (righe 1091–1110). Per ogni agente non-volante: se non
dormiente, campiona il flow e gli aggiunge il rumore angolare; calcola
`dv = flow·vpref − v`, lo limita ad `a_max·dt`, aggiorna `v` e applica lo
smorzamento. I **dormienti** sterzano verso velocità zero (flow nullo): stanno
fermi ma collidono e assorbono spinte. I **volanti** sono saltati (balistici).

**(2) Integrazione** (righe 1113–1132). Salva `qx,qy` (posizione pre-proiezione),
avanza `px,py`. Per i volanti integra l'asse z: `vz −= GRAV·dt`, `z += vz·dt`;
all'atterraggio (`z ≤ 0`) toglie `SIMP_FLYING`, smorza il momento orizzontale di
`landing_damp`, **piega `qx,qy`** perché il recupero velocità (passo 4) restituisca
la velocità smorzata invece di sovrascriverla, e registra l'handle in `landed`.

**(3) Vincoli** (righe 1136–1144). Azzera il sensore d'assedio, ricostruisce la
griglia, poi `pbd_iters` volte: `pbd_iteration` + `wall_projection` (con `record`
solo all'ultima).

**(4) Recupero velocità** (righe 1149–1162). Per i non-volanti:
`v = (px − qx)/dt`, clampato a `v_clamp`. Qui la spinta della folla diventa
momento reale. I volanti tengono la velocità integrata (nessuna proiezione li
tocca).

**(5) Drain** (righe 1167–1173). Scorre **all'indietro** (per via dello
swap-and-pop): chi è su una cella-goal viene rimosso e contato. I dormienti sono
esenti (un dormiente spinto su un goal non sta "arrivando" da nessuna parte).

**(5b) Decadimento cadaveri** (righe 1176–1184): scala il TTL, swap-and-pop alla
scadenza.

**(6) Ricostruzione finale della griglia** (riga 1190). La griglia di metà step
è ormai stantia (correzioni PBD/muro + drain): ricostruirla a fine step rende
**esatte** le query spaziali e gli impulsi *tra* uno step e l'altro.

Ritorna il numero di drenati.

### 14. Stati comportamentali

Flag in `aflags` (header righe 236–242):
- `SIMP_DORMANT`: fermo finché svegliato, ma collide, assorbe spinte e reagisce
  agli impulsi; non drenato dai goal. Per branchi dormienti piazzati sulla mappa.
- `SIMP_FLYING`: volo balistico su z fittizia. Niente steering né collisioni
  (escluso dalla griglia), scavalca i muri sopra `wall_h`. Gravità riporta z a 0;
  all'atterraggio il momento orizzontale è quasi azzerato e l'agente finisce in
  `landed` (per il danno da caduta, compito del gioco).

`simp_z_arr`, `simp_landed`/`simp_landed_count` (righe 245–247 header) espongono
quota e atterraggi.

### 15. Impulsi (`simp_apply_impulse_ex`, riga 842)

Impulso radiale (esplosione): `dv = strength·(1 − r/raggio)` in allontanamento da
`(x,y)`, per ogni agente nel raggio (`impulse_one`, riga 823). Con `up_ratio > 0`
aggiunge un calcio verticale `vz`; se `vz > TAKEOFF_VZ` l'agente decolla
(`SIMP_FLYING`). Usa la griglia se aggiornata (salta i ghost cadavere e i già in
volo, non binnati), altrimenti brute force (che potenzia anche i volanti).
`simp_apply_impulse` (riga 865) è il caso `up_ratio = 0`.

### 16. Cadaveri (`simp_corpse_add`, riga 871)

Dischi statici a massa infinita nel pool fisso (`CORPSE_CAP`). Pool pieno =
rimpiazza il più vicino a scadenza (capacità = garanzia di costo, non errore). Se
un cadavere è più grande della cella di collisione, la ingrandisce (come per i
tank). Marca `grid_stale`: sarà binnato come ghost al prossimo rebuild. I
cadaveri bloccano `simp_free_at` ma sono invisibili a query e impulsi, e **mai**
marcati nel nav grid: la deviazione del flusso attorno ai mucchi emerge dal solo
PBD (barricate ai varchi). Il decadimento TTL avviene nello step (§II.13, 5b).

### 17. Query spaziali

- `simp_query_circle` (riga 744): indici degli agenti nel raggio (satura a
  `max_out`). Usa la griglia se *current* (`grid_current`, riga 740: non stantia e
  `grid_total == count`), altrimenti brute force. I volanti sono esclusi a meno di
  `SIMP_QUERY_FLYING`; i cadaveri mai restituiti.
- `simp_query_nearest` (riga 777): il più vicino entro `r_max`, con una **scansione
  ad anelli** (Chebyshev) dal centro verso fuori e un *lower bound* per fermarsi
  presto (riga 798).

> **Trappola documentata** (header righe 226–229): gli indici restituiti muoiono
> al primo `simp_kill` (swap-and-pop). Convertirli subito in handle, oppure
> killare per indice **decrescente**.

### 18. Diagnostica e accesso in lettura

- `simp_px/py/vx/vy/radius_arr` (righe 1196–1200): gli array SoA, pronti come
  instance buffer.
- `simp_wall_pressure`/`simp_wall_cell` (righe 1201–1202): il sensore d'assedio.
- `simp_mean_overlap` (riga 1208): sovrapposizione media dell'ultimo step
  (metrica di qualità: tipicamente 1–2 cm).

---

## Parte III — Invarianti e vincoli (perché certe scelte sono intoccabili)

- **SoA rigoroso nei loop caldi, zero `malloc` in `simp_step`.** Tutti gli
  scratch (heap Dijkstra, buffer flow, ghost cadaveri) sono preallocati. È il
  prerequisito per la portabilità a compute shader: ogni fase (counting sort con
  atomics, PBD a tile colorati) è un dispatch.
- **Determinismo.** RNG xorshift interni (sim-level + per-agente), niente
  `rand()`. Stesso ordine di chiamate ⇒ stesso risultato.
- **Indici non stabili** (swap-and-pop): usare gli handle per i riferimenti
  persistenti, lo slot per i dati per-agente.
- **Timestep fisso 1/60**; il renderer interpola tra i tick.
- **Guardrail di robustezza** (cap 30% per coppia, `v_clamp`): non rimuoverli,
  evitano le velocità balistiche da sovrapposizioni profonde.
- **Throughput ai varchi ≈ 10 agenti/(m·s)**: dimensionare test e livelli di
  conseguenza.
- **Densità di spawn**: non spawnare oltre la capacità geometrica
  (impacchettamento esagonale con passo ≈ diametro max); usare `simp_free_at`.

## Appendice — Mappa rapida funzione → riga

| Funzione | Riga | Ruolo |
|---|---|---|
| `rng_next` / `rng_f01` / `rng_fsym` | 18–28 | RNG deterministico |
| `recompute_phi` | 163 | Dijkstra 8-vicini → `phi` + `cost_mult` |
| `recompute_flow` | 215 | `phi` → campo direzioni + smoothing |
| `density_update` | 272 | istogrammi densità/jam + blur + EMA |
| `recompute_sdf` | 318 | chamfer distance transform → `sdf` |
| `nav_commit` | 348 | phi + flow + sdf |
| `bilinear` / `simp_sample_flow` / `simp_sample_sdf` / `sdf_grad` | 357–390 | campionamento campi |
| `simp_create` / `simp_destroy` | 394 / 501 | lifecycle |
| `simp_set_wall` / `simp_set_goal` / `simp_add_cost` | 524–540 | terreno e costi |
| `spawn_common` / `simp_spawn` / `simp_spawn_desc` | 560–597 | spawn |
| `simp_kill` | 629 | rimozione swap-and-pop |
| `simp_free_at` | 653 | test di spazio libero |
| `simp_handle_of` / `simp_slot_of` / `simp_index_of` | 715–726 | handle stabili |
| `simp_query_circle` / `simp_query_nearest` | 744 / 777 | query spaziali |
| `simp_apply_impulse_ex` | 842 | impulso radiale + volo |
| `simp_corpse_add` | 871 | cadaveri-ostacolo |
| `rebuild_grid` | 903 | counting sort griglia collisione |
| `pbd_iteration` | 939 | separazione dischi (PBD) |
| `wall_projection` | 1004 | reiezione muri + sensore d'assedio |
| `simp_step` | 1056 | orchestratore dello step |

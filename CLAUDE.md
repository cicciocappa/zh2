# Horde game — project memory

Progetto: tower defense top-down "physics-based" con orde massive (target: 50k–100k
nemici simulati), riferimento creativo *Sir, We Have an Orc Problem* + *Creeper World*.
Stack: **C + SDL3** per il sandbox; i core di simulazione sono **renderer-agnostici,
C puro, zero dipendenze** e portano su OpenGL (instancing + compute shader in futuro).

Conversazioni in **italiano**. Codice e commenti in **inglese**. Si lavora da terminale
(compila e testa sempre prima di consegnare). Vedi `TODO.md` per l'agenda operativa.

## I due core

Il progetto ha DUE modelli di simulazione complementari:

1. **`sim.h` / `sim.c` — core continuo** (POC precedente, verificato). L'orda è un
   campo di densità `rho` su griglia. Due forze: goal forcing (avvezione lungo
   `-grad(phi)`, Dijkstra 8-vicini) e surface pressure (equalizzazione shallow-water
   su `bed`, traboccamento weir oltre i muri). Niente momento, niente Poisson;
   stabile incondizionatamente. Manopola: `relax_iters` (granulare↔acquoso).
   Ruolo futuro: **LOD lontano** (orde fuori dalla zona attiva simulate come fluido,
   condensate in particelle quando si avvicinano) e densità→costo per il Dijkstra.

2. **`sim_particles.h` / `sim_particles.c` — core particellare** (attuale focus,
   verificato). Ogni nemico è un disco 2D. Tre ingredienti:
   - **Flow field**: Dijkstra 8-vicini dai goal → `phi`; campo direzioni normalizzato
     e smussato (1 passata), campionato bilineare dagli agenti. Lazy via `nav_dirty`.
     I muri sono attraversabili a costo enorme (`WALL_ENTER`, come nel core continuo):
     le rotte aperte vincono sempre, ma un goal completamente murato continua ad
     attrarre l'orda, che va a premere contro le mura (assedio).
   - **Steering**: verso il flow a velocità preferita per-agente, accelerazione
     limitata (`a_max`), rumore angolare per-step (`noise_ang`) + jitter di velocità
     e raggio per-agente (anti-lockstep).
   - **PBD**: separazione dei dischi sovrapposti per proiezione di posizione pesata
     per massa inversa (massa ~ r²), Gauss-Seidel su griglia uniforme ricostruita
     ogni step con counting sort (cella = 2·r_max; coppie visitate una volta via
     5 celle forward E/SW/S/SE). Muri tramite SDF chamfer (3-4) + gradiente a
     differenze centrali. Velocità effettiva recuperata: `v = (x − x_prev)/dt`
     → la spinta della folla si propaga come momento reale.
   - **Guardrail di robustezza** (necessari, non toglierli): cap del 30% di `r_i+r_j`
     sulla correzione per coppia per iterazione, e `v_clamp` (default 20 m/s) sulla
     velocità recuperata. Senza, sovrapposizioni profonde (spawn troppo denso,
     schiacciamenti) producono velocità balistiche (osservato: 160 m/s).
   - **Impulso radiale** (`simp_apply_impulse`): esplosioni come dv con falloff
     lineare; usa la griglia se aggiornata, altrimenti brute force.
   - **Stato comportamentale**: per-agente `dormant` (sveglio segue il flow,
     dormiente frena verso velocità zero ma collide e assorbe spinte; non viene
     drenato dai goal). `simp_spawn_dormant` per i branchi piazzati sulla mappa,
     `simp_wake_radius` / `simp_wake_all` per i risvegli (esplosioni, alba).
   - **Spawn senza burst**: `simp_free_at(x,y,r)` dice se un disco ci sta senza
     sovrapporsi ad agenti o muri (griglia se attuale, altrimenti brute force).
     Emettere solo dove è libero elimina l'espulsione PBD allo spawn e fa
     auto-strozzare gli emitter alla portata dell'uscita (flusso da tunnel).
   - **Handle stabili** (M3.1): slot map con generation counter sopra lo
     swap-and-pop. `simp_handle_of` / `simp_index_of` / `simp_slot_of`; i dati
     di gioco per-agente (HP, tipo…) vanno indicizzati per SLOT, stabile per
     tutta la vita dell'agente. 20 bit slot + 12 bit gen, 0 = invalid.
   - **Query spaziali** (M3.4): `simp_query_circle` (indici nel raggio, satura
     a max_out) e `simp_query_nearest` (scansione ad anelli con lower bound).
     Esatte tra uno step e l'altro: la griglia viene RICOSTRUITA a fine step
     (le correzioni PBD/wall e il drain rendono stantio il binning di metà
     step). Trappola documentata nell'header: gli indici ritornati muoiono al
     primo kill — convertirli subito in handle, o killare per indice decrescente.

## Stato verificato (core particellare)

- Compila pulito (`-O2 -Wall -Wextra -std=c11`), zero dipendenze oltre libm.
- `test_particles`: griglia 320×240 (cella 0.5 m), ~13k agenti, muro con due varchi
  da 4 m, 9000 step a 60 Hz. Zero NaN/out-of-bounds, overlap residuo medio ~1–2 cm,
  71% drenato al goal (throughput limitato dai varchi: congestione realistica —
  cunei granulari contro il muro, getti a ventaglio oltre i varchi, verificato
  visivamente sui frame PPM). ~3.5 ms/step single-thread.
- `test_impulse`: esplosione in folla impaccata → picco 14 m/s, rientro sotto
  4 m/s in pochi secondi, zero NaN.
- `test_handles`: 1000 spawn + kill casuali + 3 round di riuso slot contro
  shadow map brute force: ogni handle vivo risolve alla posizione giusta, ogni
  handle morto dà -1 anche dopo il riciclo dello slot.
- `test_query`: 10k agenti, 200 query circle e 200 nearest contro brute force
  O(N) (zero mismatch), saturazione del buffer, scenario torretta
  (nearest→handle→kill, esercita anche il fallback a griglia stantia) e AoE
  con kill per indice decrescente. Il secondo counting sort di fine step costa
  ~0.4 ms a 13k (3.41→3.85 ms/step in `test_particles`).
- `test_dormant`: branco piazzato via `simp_free_at` (zero coppie sovrapposte),
  fermo immobile per 300 step con goal attivo, `wake_radius` sveglia solo il
  sottoinsieme (che marcia e drena), `wake_all` svuota la mappa. Nota: svegliare
  il lato lontano del branco è lento — i marciatori devono spingersi attraverso
  i dormienti (ostacolo mobile, comportamento voluto).

## File

- `sim_particles.h` / `.c` — core particellare (vedi sopra). API `simp_*`.
  SoA pubblici (`simp_px/py/vx/vy/radius_arr`) pronti come instance buffer.
- `test_particles.c` — verifica headless: scena chokepoint, metriche, frame PPM
  in `frames/`.
- `test_impulse.c` — smoke test esplosione (picco + assestamento).
- `test_dormant.c` — verifica stato dormiente, `simp_free_at`, risvegli.
- `test_handles.c` / `test_query.c` — verifica handle (M3.1) e query (M3.4).
- `M3_DESIGN.md` — design tecnico di M3 (handle, volo, cadaveri, query, tipi,
  densità→costo): API, dettagli, piani di verifica.
- `sandbox_particles.c` — sandbox interattivo SDL3 (pennelli muro/spawner/goal/
  pack, RMB = esplosione che sveglia anche i dormienti, W = sveglia tutti,
  manopole live, pausa/step, overlay flow field). Gli spawner sono stato del
  sandbox, non del core; il pennello PACK piazza dormienti one-shot (ridipingere
  riempie i buchi, idempotente). Controlli nell'header del file.
- `Makefile` — `make test` (entrambi, no deps) · `make sandbox` (SDL3) ·
  `make clean`. SDL3 compilato dai sorgenti sta in `~/.local`: il Makefile
  imposta `PKG_CONFIG_PATH` da solo, e `sdl3.pc` porta già l'rpath giusto.
- `sim.h` / `sim.c` / `test_dump.c` / `sandbox_sdl3.c` — core continuo e relativo
  sandbox (progetto precedente, nella sua cartella).

## Convenzioni e vincoli

- SoA rigoroso nei loop caldi; nessuna allocazione dentro `simp_step`.
- Determinismo: RNG xorshift interni (sim-level + per-agente), niente `rand()`
  nel core. Stesso ordine di chiamate ⇒ stesso risultato.
- **Indici agente NON stabili** (rimozione swap-and-pop). Per riferimenti
  persistenti (targeting torrette) serve un layer di handle (in TODO).
- Fixed timestep (1/60); il renderer interpola tra tick.
- Ogni passo dello step è pensato per diventare un dispatch di compute shader
  (counting sort con atomics, PBD a tile colorati): non introdurre stato che
  rompa questa mappatura.
- Unità: metri, secondi. Default umani: r=0.30 m, v=1.4 m/s.

## Trappole note

- `gcc -std=c11` nasconde POSIX: i test che usano `clock_gettime` richiedono
  `#define _POSIX_C_SOURCE 199309L` prima degli include.
- Densità di spawn: non spawnare sopra la capacità geometrica (reticolo esagonale
  con passo ≈ diametro max). I guardrail evitano l'esplosione ma il transitorio
  resta brutto.
- Il throughput ai varchi è ~10 agenti/(m·s): dimensionare i test (e i livelli!)
  di conseguenza.

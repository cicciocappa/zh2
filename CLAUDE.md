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

## Stato verificato (core particellare)

- Compila pulito (`-O2 -Wall -Wextra -std=c11`), zero dipendenze oltre libm.
- `test_particles`: griglia 320×240 (cella 0.5 m), ~13k agenti, muro con due varchi
  da 4 m, 9000 step a 60 Hz. Zero NaN/out-of-bounds, overlap residuo medio ~1–2 cm,
  71% drenato al goal (throughput limitato dai varchi: congestione realistica —
  cunei granulari contro il muro, getti a ventaglio oltre i varchi, verificato
  visivamente sui frame PPM). ~3.5 ms/step single-thread.
- `test_impulse`: esplosione in folla impaccata → picco 14 m/s, rientro sotto
  4 m/s in pochi secondi, zero NaN.

## File

- `sim_particles.h` / `.c` — core particellare (vedi sopra). API `simp_*`.
  SoA pubblici (`simp_px/py/vx/vy/radius_arr`) pronti come instance buffer.
- `test_particles.c` — verifica headless: scena chokepoint, metriche, frame PPM
  in `frames/`.
- `test_impulse.c` — smoke test esplosione (picco + assestamento).
- `sandbox_particles.c` — sandbox interattivo SDL3 (pennelli muro/spawner/goal,
  RMB = esplosione, manopole live, pausa/step, overlay flow field). Gli spawner
  sono stato del sandbox, non del core. Controlli nell'header del file.
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

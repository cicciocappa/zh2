# M4 — Scala: multithreading del core (design)

Obiettivo: portare `simp_step` da single-thread a multi-core mantenendo
**determinismo** e l'architettura "ogni passo è un dispatch" (mappa ai compute
shader futuri, M5). Niente lock nel cammino caldo.

## Thread pool (`sim_threads.h`)

Header-only (incluso solo da `sim_particles.c`), pthreads + `<stdatomic.h>`,
zero dipendenze esterne. Pool persistente: i worker NON si ricreano ogni step.
Un `simpool_parallel_for(pool, n, chunk, fn, arg)` spezza `[0,n)` in chunk
tirati dai worker via un cursore atomico (load-balance dinamico), bloccante fino
a fine range. Il chiamante partecipa come worker 0. `fn(arg, worker, begin,
end)`: l'indice worker serve solo per accumulare diagnostiche in slot per-worker
(ridotte in ordine fisso → deterministiche). Default thread = CPU online
(`SIMP_THREADS` override, `simp_set_threads`).

## Cosa è parallelo e cosa no

| fase | stato | nota |
|------|-------|------|
| steering | ✅ parallel | per-agente, RNG per-slot indipendente |
| integrate | seriale | cheap; il touchdown dei flyer appende a `landed[]` (cursore condiviso) |
| rebuild_grid (counting sort) | seriale | O(N), candidato successivo |
| **PBD (Gauss-Seidel)** | ✅ parallel | **tile colorate**, vedi sotto |
| wall_projection | ✅ parallel | per-agente, sensore d'assedio scrive solo il proprio slot |
| velocity recovery | ✅ parallel | per-agente |
| nav: density blur + flow | ✅ parallel | per-cella (lo scatter dell'istogramma resta seriale) |
| nav: Dijkstra (phi) | seriale **a budget** | drenato su ~6 step, vedi sotto |
| drain / corpse decay | seriale | swap-and-pop |

## PBD a tile colorate (il cuore)

Ogni cella di collisione corregge i dischi della propria cella + 4 vicine
forward (E, SW, S, SE). Il **write footprint** di una cella è la cella espansa
di 1 a sinistra/destra/sotto. Quindi con **tile di T≥2 celle** (`PBD_TILE=4`) in
una scacchiera **2×2 (4 colori)**, due tile dello stesso colore (distanti 2T)
hanno footprint **disgiunti** → si processano in parallelo senza lock. 4 passate
(una per colore), barriera tra colori (= il `parallel_for` che ritorna).

**Determinismo**: le tile dello stesso colore sono indipendenti (scritture
disgiunte) → il risultato NON dipende dall'ordine/numero dei thread, solo
dall'ordine fisso dei colori. Verificato: `bench_sim` stampa un checksum delle
posizioni, IDENTICO su 1/2/4/8/16/28 thread. (Cambia rispetto al vecchio sweep
row-major bit-exact, ma resta un PBD valido — unico ritocco test: il floor
d'attacco in `test_siege` da 0.004→0.006, il leak tangente è salito 0.0037→
0.0043, sempre 8× sotto l'head-on 0.0316.)

## Risultati (bench_sim, scenes/stress.scn, 50k)

i7-14700 (28 thr): 10.3 ms (1t) → 3.6 (8t) → 2.5 (28t) = **4.2×**, ginocchio
~16 thr. Ryzen 7 3750H (4c/8t, APU): 1.46× (banda di memoria + APU). Il `max`
per-step (~7-8 ms) non scala: è il ricalcolo nav seriale.

## Nav spike: Dijkstra incrementale a budget (FATTO)

Lo spike nav (~7.6 ms ogni `flow_period`) si scomponeva: `density_update` 1.3 ms,
`recompute_phi` (Dijkstra) 4.6 ms, `recompute_flow` 1.7 ms. Solo il Dijkstra è
intrinsecamente seriale (label-setting). Soluzione in due parti:

- **density blur + flow paralleli** (`job_density_blur`, `job_flow_dir/smooth`):
  per-cella, scritture disgiunte → deterministici a qualunque thread count. Lo
  scatter dell'istogramma resta seriale (cheap; un scatter parallelo vorrebbe
  atomics/bin per-thread per restare deterministico). Il `peak` per `rho_active`
  è una riduzione seriale.
- **Dijkstra a budget** (`nav_phi_begin` + `nav_phi_drain(budget)`): l'heap è
  persistente tra step (`nav_heap_n`), si drenano `nav_budget = gw·gh/6` pop per
  step. phi viene costruito IN-PLACE su più step — ma niente nei loop caldi legge
  phi (solo `recompute_flow`, a heap vuoto), quindi gli agenti continuano a
  leggere il flow COMMITTATO finché il build non finisce e si committa il nuovo
  flow. Inputs congelati (rho_s/cost_mult al `begin`), schedule a budget fisso +
  heap seriale → il risultato NON dipende da thread/timing: **phi bit-identico**
  al monolitico, solo committato ~6 step dopo (latenza << `flow_period` 30 step).
  Una terrain edit (`nav_dirty`) abortisce il build in corso e fa il commit
  sincrono pieno. Determinismo verificato: checksum `bench_sim` identico su 1/4/8
  thr. **Risultato 50k/8core (i7→Ryzen-3750H): max 15.5→9.4 ms, avg invariato
  6.6 ms** (lo spike non domina più il frame peggiore). 11/11 test PASS.

## Riordino per località di cache (FATTO)

`rebuild_grid` è ~0.63 ms/chiamata (×2/step) ed è MEMORY-bound: count 0.23 +
scatter 0.30 ms (scritture sparse in `corder`), prefix 0.08, memset 0.02. La
griglia di collisione è SPARSA (ccell≈0.72 m → nc≈77k celle per ~46k agenti,
0.6/cella).

**Istogrammi per-thread: scartati.** Solo il count si parallelizza in modo
deterministico (somma di interi via atomics). Lo scatter no, senza ricostruire
gli offset per-chunk (O(nc·C)). Su griglia sparsa l'array istogramma C×nc è
~2.4 MB (C=8): azzerarlo + rileggerlo in ricombinazione costa ~0.5 ms di sola
banda, ≥ del count+scatter (0.53 ms) che parallelizzerebbe. Bandwidth-bound a
vuoto: net neutro/negativo.

**Riordino periodico (`reorder_agents`).** La vera causa è la località: dopo il
churn di spawn/kill (swap-and-pop) l'ordine degli indici si scorrela dallo
spazio → scatter di `corder` e letture vicini `px[corder[b]]` nel PBD vanno a
caso in memoria. Ogni `REORDER_PERIOD` (60) step un counting sort seriale per
cella permuta tutti gli array SoA in ordine spaziale e ricostruisce la slot map.
Deterministico (seriale, indipendente dai thread); cambia l'ordine PBD
intra-cella (checksum shift) ma resta identico cross-thread. Costo amortizzato
trascurabile. **Misura (`bench_sim`, 50k/8thr): layout SCRAMBLED (= stato reale)
avg 6.78→6.41 ms (~5%); prefill ordinato neutro.** Manopola `SIMP_REORDER`;
`BENCH_SCRAMBLE=1` simula il caso scorrelato. NB: rende gli indici instabili
ANCHE senza kill — il contratto già lo vieta (dati persistenti per slot/handle);
`test_dormant` correlava per indice ed è stato corretto a correlare per slot.

## Prossimi passi
1. SIMD su steering/integrate/recovery (verificare prima l'autovettorizzazione).
2. wall_projection: saltare il sample SDF se la cella nav è lontana dai muri.

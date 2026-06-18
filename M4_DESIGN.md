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
| nav (Dijkstra phi/flow) | seriale | spike ogni `flow_period`; **prossimo collo di bottiglia** |
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

## Prossimi passi
1. **Nav spike**: il Dijkstra seriale è ora il frame peggiore. Opzioni:
   ricalcolo incrementale, async su un worker, o parallelizzazione del flow.
2. rebuild_grid parallelo (istogrammi per-thread) se il profiling lo chiede.
3. SIMD su steering/integrate/recovery (verificare prima l'autovettorizzazione).
4. Riordino periodico degli agenti per località di cache (~ogni 60 step).

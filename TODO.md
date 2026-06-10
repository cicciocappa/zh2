# TODO — Horde game

Agenda operativa. Riferimento architetturale completo in `CLAUDE.md`.
Ordine consigliato: prima rendere visibile e tastabile (sandbox), poi fisica,
poi scala, poi gioco.

## Subito (prossima sessione)

- [x] **Sandbox SDL3 per il core particellare** (`sandbox_particles.c`):
      rendering a punti/cerchi, pennelli muro/goal/spawner, click destro =
      esplosione (`simp_apply_impulse`), manopole live (`pbd_iters`, `noise_ang`,
      `v_max`, raggio impulso), pausa/step. FATTO: `make sandbox`, compila pulito
      e gira contro l'SDL3 in `~/.local` (rpath via sdl3.pc). Scena di default:
      muro con due varchi da 3 m, blocco pre-spawnato a sinistra, goal a destra.
      Controlli documentati nell'header del file.
- [ ] Verifica visiva interattiva: imbuti ai varchi, onde di pressione dopo
      un'esplosione, assenza di lockstep. Tarare i default a occhio.

## M3 — Fisica di gioco

- [ ] **Layer di handle** (id stabili ↔ indici swap-and-pop) — prerequisito per
      targeting e effetti. Generation counter per evitare handle pendenti.
- [ ] **z fittizia per il volo balistico**: per-agente `z, vz`; durante il volo
      (z > 0) niente collisioni né steering, gravità semplice; all'atterraggio
      rientro nella simulazione. Impulsi forti assegnano anche vz.
- [ ] **Cadaveri**: alla morte → decal (default, costo zero) + per una frazione
      configurabile → ostacolo passivo temporaneo (disco statico nella griglia
      di collisione, TTL qualche secondo). Obiettivo: barricate di corpi ai
      colli di bottiglia che deviano il flusso.
- [ ] **Danno ad area / query spaziali di gameplay** sulla stessa griglia di
      collisione: `simp_query_circle(x, y, r, callback)` o riempimento di un
      buffer di indici.
- [ ] **Tipi di nemico** come soli parametri: tank (massa ×10, raggio ×1.5),
      runner (v_max ×2), screamer (modifica locale temporanea del costo Dijkstra).
- [x] **Stato dormiente + spawn senza burst** (`dormant`, `simp_free_at`,
      `simp_spawn_dormant`, `simp_wake_radius/all`): emitter auto-strozzati,
      branchi one-shot che dormono finché non svegliati (esplosione, alba).
      FATTO: `test_dormant` PASS, pennello PACK + tasto W nel sandbox.
- [ ] **Estensioni dello stato comportamentale**: wake per contagio (un sveglio
      a contatto con un dormiente lo sveglia → onda di risveglio nel branco),
      wake per prossimità del giocatore/rumore; più avanti stato "attacking"
      (vedi questione aperta sul drain). Valutare anche recinzioni distruttibili
      come contenimento alternativo dei branchi (muri con HP).
- [ ] Densità → costo Dijkstra (Continuum Crowds): accumulare densità per cella
      nav durante il rebuild della griglia, ricalcolo flow a bassa frequenza
      (~0.5 s). L'orda aggira gli ingorghi da sola.

## M4 — Scala (target 50–100k a 60 Hz su CPU)

- [ ] Profilare prima di ottimizzare (perf baseline attuale: ~3.5 ms/step a 13k,
      single-thread).
- [ ] **Multithreading PBD a tile colorati**: partizionare la griglia di collisione
      in tile a scacchiera (4 colori in 2D), processare in parallelo i tile dello
      stesso colore, barriera tra colori. Zero lock. Steering/integrazione/recovery
      sono embarrassingly parallel.
- [ ] SIMD sui loop di steering e integrazione (SoA già pronto); verificare che
      l'autovettorizzazione faccia già il lavoro prima di scrivere intrinsics.
- [ ] Riordino periodico degli agenti in memoria secondo l'ordine della griglia
      (ogni ~60 step) per località di cache.
- [ ] Ottimizzare `wall_projection`: saltare il sample SDF se la cella nav è
      lontana dai muri (flag per cella "sdf > r_max + margine").

## M5 — GPU (solo se serve oltre ~100k o per liberare la CPU)

- [ ] Port dei passi a compute shader GL 4.3 (counting sort con atomics, PBD a
      colori, integrazione). Rendering instanced direttamente dai buffer GPU,
      zero roundtrip CPU. La struttura attuale mappa 1:1 sui dispatch.

## M6 — Gioco

- [ ] Rendering vero: instanced sprites (vec4 x,y,angolo,frame per istanza),
      variazione tinta/scala per hash dell'indice; valutare VAT per il vicino.
      Scelta dello sprite/animazione in base alla velocità della particella:
      sopra una soglia → zombie che rotola/cade (così ogni residuo schizzo di
      fisica diventa leggibile come azione invece che come glitch).
- [ ] Torrette: piazzamento (= muri + sorgente di danno), targeting via query
      spaziali, proiettili/raycast sulla griglia.
- [ ] Wave/spawner, economia, HP/danno, condizioni di vittoria/sconfitta.
- [ ] Integrazione del core continuo come LOD lontano: orde off-screen come campo
      `rho`, condensazione in particelle al bordo della zona attiva (e viceversa).
      Conservazione della massa al passaggio di rappresentazione.

## Questioni aperte

- Attrito tangenziale tra dischi in contatto? (Ora solo separazione normale:
  la folla "scivola" — probabilmente ok per zombie, da valutare a occhio.)
- Il drain attuale uccide al primo contatto col goal cell: per il TD servirà
  invece "attacca la struttura" (stato attacking + DPS sul bersaglio).
- Politica multi-goal: un solo campo Dijkstra multi-sorgente o campi pesati
  per appetibilità (base vs torrette vs giocatore)?
- Dimensione cella nav vs collisione: ora indipendenti (0.5 m vs ~0.65 m);
  unificarle simplificherebbe il port GPU?

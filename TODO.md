# TODO — Horde game

Agenda operativa. Riferimento architetturale completo in `CLAUDE.md`.
Ordine consigliato: prima rendere visibile e tastabile (sandbox), poi fisica,
poi scala, poi gioco.

## M7 — Scena vettoriale + ostacoli (in corso, giugno 2026)

- [x] **Formato scena vettoriale** (`scene.h/.c` riscritti): entità in METRI
      rasterizzate sulla griglia all'instantiate. Ostacoli = poligoni convessi
      con `height` (estrusione 3D) + effetto nav (`solid`→muro / `cost W`→costo
      Dijkstra). Rect per goal/spawn/pack/cost. `test_scene` riscritto e PASS
      (roundtrip + raster muro-da-poligono + cost + packs + determinismo).
      Scanline even-odd (gestisce anche concavi); render fan = solo convessi.
- [x] **Ostacoli renderizzati in `vat_horde`**: carica una `.scn` (default
      `scenes/obstacles.scn`), estrude i poligoni (top fan + pareti) + suolo in
      una mesh statica, pass flat-shaded (`vat/flat.vs/.fs`, key NW). Spawn dalle
      rect di scena. Verificato headless: l'orda imbuta nei varchi del muro
      spezzato e si spacca attorno al diamante; fango (cost) aggirato.
- [x] **HUD prestazioni** nel titolo: ms sim vs ms render separati (medie mobili).
      MAXA alzato 8000→16000. A ~3900 agenti: sim ~9 ms / render ~9 ms.
- [x] **Stress test** (M7b): modalità benchmark in `vat_horde`
      (`VAT_HORDE_FILL=N VAT_HORDE_BENCH="warmup,measure"`, prefill a lattice
      senza burst, scene `scenes/stress.scn` 200×200 m). Timer separati
      core_sim / vat_layer / render. Misurato su **AMD Vega 10 iGPU** (debole;
      la workstation ha la RTX A2000 → render lì molto più basso):
        N      core_sim  vat_layer  render   tot     cap
        ~4.9k   4.3 ms    0.4 ms    10.2 ms  14.9    67 fps
        ~9.8k   7.3 ms    0.7 ms    16.6 ms  24.6    41 fps
        ~19.5k 14.3 ms    1.2 ms    35.1 ms  50.6    20 fps
        ~34k   27.7 ms    2.0 ms    72.3 ms 102.0    10 fps
        ~46k   33.9 ms    2.3 ms    84.3 ms 120.6     8 fps
      LETTURE: core_sim LINEARE ~0.75 ms/1000 (M4 multithread/SIMD lo taglia
      ~3-4×); vat_layer trascurabile; render DOMINA ma è puro fill GPU su iGPU
      debole → su GPU discreta crolla. Il limite ARCHITETTURALE è core_sim:
      **20k già sta nei 16.6 ms a 60 Hz single-thread (14.3 ms)** — il collo
      di bottiglia OGGI è il render sull'iGPU. 50k stabile (nessuna esplosione
      PBD) ma 34 ms sim → serve M4 per 60 Hz. 20k è ampiamente alla portata.
- [ ] **Portare il sandbox 2D** (`sandbox_particles.c`) al formato vettoriale:
      oggi NON compila (usava `.wall`/`scene_alloc`/save-celle-dipinte). Non è
      nei target di default. Decidere: pennelli che editano poligoni, o ritirarlo
      in favore di `vat_horde`.
- [ ] Editor di ostacoli nel sandbox 3D: piazzare/spostare poligoni a runtime,
      salvare la `.scn`.

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

- [x] **Layer di handle** (id stabili ↔ indici swap-and-pop) — prerequisito per
      targeting e effetti. Generation counter per evitare handle pendenti.
      FATTO: slot map per design M3.1, `test_handles` PASS.
- [x] **z fittizia per il volo balistico**: per-agente `z, vz`; durante il volo
      (z > 0) niente collisioni né steering, gravità semplice; all'atterraggio
      rientro nella simulazione. Impulsi forti assegnano anche vz.
      FATTO per design M3.2 (atterraggi riportati come HANDLE, non indici);
      fase volo in `test_impulse` PASS.
- [x] **Cadaveri**: alla morte → decal (default, costo zero) + per una frazione
      configurabile → ostacolo passivo temporaneo (disco statico nella griglia
      di collisione, TTL qualche secondo). Obiettivo: barricate di corpi ai
      colli di bottiglia che deviano il flusso.
      FATTO per design M3.3 ma con implementazione "ghost" (invm=0 in coda
      agli array SoA, niente casi speciali nel PBD); `test_corpses` PASS,
      pennello KILL nel sandbox per la verifica visiva delle barricate.
- [x] **Danno ad area / query spaziali di gameplay** sulla stessa griglia di
      collisione: `simp_query_circle` (buffer di indici) + `simp_query_nearest`.
      FATTO per design M3.4 (+ rebuild della griglia a fine step per query
      esatte), `test_query` PASS contro brute force.
- [x] **Tipi di nemico** come soli parametri: tank (massa ×10, raggio ×1.5),
      runner (v_max ×2), screamer (modifica locale temporanea del costo Dijkstra).
      FATTO per design M3.5: `simp_spawn_desc` (massa in unità walker) +
      `simp_add_cost`/`simp_clear_cost` per le perturbazioni locali del campo;
      `test_types` PASS; nel sandbox tasto M (tipo spawner/pack) e pennelli
      G/H (costo ±), X per azzerare.
- [x] **Stato dormiente + spawn senza burst** (`dormant`, `simp_free_at`,
      `simp_spawn_dormant`, `simp_wake_radius/all`): emitter auto-strozzati,
      branchi one-shot che dormono finché non svegliati (esplosione, alba).
      FATTO: `test_dormant` PASS, pennello PACK + tasto W nel sandbox.
- [ ] **Estensioni dello stato comportamentale**: wake per contagio (un sveglio
      a contatto con un dormiente lo sveglia → onda di risveglio nel branco),
      wake per prossimità del giocatore/rumore; più avanti stato "attacking"
      (vedi questione aperta sul drain). Valutare anche recinzioni distruttibili
      come contenimento alternativo dei branchi (muri con HP).
- [x] Densità → costo Dijkstra (Continuum Crowds): accumulare densità per cella
      nav durante il rebuild della griglia, ricalcolo flow a bassa frequenza
      (~0.5 s). L'orda aggira gli ingorghi da sola.
      FATTO per design M3.6: `density_update` (istogramma + blur + EMA) e
      ricalcolo phi/flow throttled (`flow_period`); `k_density` manopola
      (K/L nel sandbox, O = overlay densità); `test_density_route` PASS
- [x] **Ingorgo → costo (M3.7)**: la densità misura quanta gente c'è, non se
      si muove — un varco fisicamente bloccato (throughput zero) non diventa
      mai abbastanza caro e l'orda ci fa la coda per sempre. Secondo campo
      `jam` = istogramma pesato per fermezza `(1−|v|/v_pref)²` (stesso blur +
      EMA, zero passate extra), termine `k_jam·min(jam/rho_max,1)` negli
      archi: solo la folla FERMA diventa cara, il flusso devia su strade
      alternative e torna quando il tappo si scioglie. `test_jam` PASS
      (A/S + overlay O nel sandbox, `set k_jam` nei file di scena)
      (split 22%→57%, drain più veloce). M3 COMPLETO.

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
      DIREZIONE ARTISTICA DECISA: vedi `GFX_DESIGN.md` (ortografico 3/4,
      prerender 16 dir da Blender, tier di zoom, gore come sistema, heading
      = EMA per slot nel renderer). Prossimo passo: scelta dello stack
      (SDL_GPU vs GL 4.3, decide il lato compute di M5) e upgrade del
      sandbox a rendering instanced — che è anche il banco di prova di M4.
- [ ] **Zombie bloccati contro un ostacolo → animazione attack, non idle**
      (renderer-side, prossima sessione). I pezzi esistono già tutti:
      lo stato "stuck" con isteresi è nel layer (sprite_layer.c), la
      prossimità al muro è `simp_sample_sdf(x,y) < r + margine`, il facing
      verso l'ostacolo è il gradiente dell'SDF (campionamento a differenze
      centrali, come fa il core per le collisioni), la sheet attack è già
      renderizzata e il playback a tempo (ZSP3 `duration_s`) già supportato.
      Da decidere: ri-renderizzare attack a più frame (ora 4); i CADAVERI
      non stanno nell'SDF, quindi gli ammassati contro una barricata di
      corpi resterebbero in idle (accettabile? altrimenti check sulla
      lista corpse). Più avanti si fonde con lo stato "attacking" vero
      del gameplay (muri con HP, drain→DPS, vedi questioni aperte): il
      sensore è già pronto — `simp_wall_pressure[i] > 0` dice quali
      slot stanno premendo un muro per davvero (verso il goal), più
      pulito del check SDF generico perché esclude lo sfioramento
      tangente. Vedi `SIEGE_DESIGN.md` §5.
- [ ] **Idle poco animata → effetto copia-incolla nei gruppi bloccati**
      (prossima sessione). La clip idle Mixamo attuale è quasi statica:
      i frame sono indistinguibili, quindi la desincronizzazione di fase
      (che ESISTE già: fase seedata per slot + continuità dal walk) non
      basta a rompere l'uniformità. Opzioni: (a) clip idle più pronunciata,
      (b) più idle corte come varianti (es. 4 clip da 4 frame — il
      meccanismo multi-variante del walk si riusa pari pari per le stuck
      sheet), (c) in più, velocità di playback per-agente ±20% da hash
      (una riga, desincronizza anche a parità di clip). Candidate nel
      pack: scream/biting come "struggle" alternativi.
- [ ] **Zombie bloccati nel mucchio che ruotano all'impazzata** (prossima
      sessione). Causa: dentro l'ingorgo il PBD produce velocità di
      assestamento (0.1–0.3 m/s) con direzione che gira vorticosamente;
      sopra la soglia di 0.05 m/s l'EMA dell'heading le insegue. Due fix
      complementari: (1) congelare l'aggiornamento dell'heading in stato
      stuck (lo stato c'è già, è la fix più pulita), (2) rate limit sul
      delta di heading per frame (max °/s sensato, proposta utente), che
      migliora anche i camminatori — uno zombie non piroetta. Da tarare
      insieme nella stessa sessione.
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
  → **SENSORE PRONTO** (`SIEGE_DESIGN.md`): `simp_wall_pressure`/`simp_wall_cell`
    espongono chi preme un muro per raggiungere il goal e quale cella; il modello
    ad attacchi discreti (timer per-slot, HP della struttura, crollo→reroute) è
    dimostrato in `test_siege` ma vive lato gioco. Da agganciare quando arrivano
    le strutture del giocatore e l'animazione attacco (M6). Lo sfioramento
    tangente (into_wall ≤ 0) resta fuori → futura meccanica hazard ambientali.
- Politica multi-goal: un solo campo Dijkstra multi-sorgente o campi pesati
  per appetibilità (base vs torrette vs giocatore)?
- Dimensione cella nav vs collisione: ora indipendenti (0.5 m vs ~0.65 m);
  unificarle simplificherebbe il port GPU?

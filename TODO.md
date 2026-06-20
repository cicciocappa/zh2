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
- [x] **Sandbox 2D ritirato** in favore di `vat_horde` (l'unico tool visivo):
      `sandbox_particles.c` resta non compilato/non nei target. L'editing si fa
      nell'editor 3D (sotto), non nel vecchio sandbox 2D.
- [ ] **EDITOR DI LIVELLI** (design completo in `EDITOR_DESIGN.md`): modalità
      EDIT dentro `vat_horde`, edita+playtesta nello stesso tool. `Scene` =
      unica verità (re-instantiate su Play, niente editing live); il formato
      `.scn` cresce per ospitare le entità gameplay (struttura distruttibile =
      `poly solid hp [core]`, `exit` con script director, `turret`, `budget`),
      retro-compatibile. UI pure-C in fase 1 (font bitmap + tastiera), cimgui in
      fase 2. **Fase 1 (terreno)**: picking sul piano y=0, tool rect/poly,
      save/load, `test_scene` esteso (roundtrip del formato nuovo) + unit del
      picking. **Fase 2 (gameplay)**: layer `level.c` che deriva DefGame/director
      dalla Scene. **Fase 3**: undo, snap toggle, validazione, playtest in-editor.
  - [x] **Terreno slice 1 — bake + sampling** (`EDITOR_DESIGN.md §9`): formato
        `.zhm` (heightmap render-only, header ZHM1 alla `.zspr`), modulo C
        `terrain.h/.c` (load/save + `terrain_z(x,y)` bilineare clampato, zero
        deps) e `gfx/terrain_bake.py` (Blender headless: raycast giù sull'AABB
        → `.zhm`, backfill dai vicini per i miss sui bordi MAX). `test_terrain`
        PASS (roundtrip, piano esatto, gradino, clamp). Verificato end-to-end:
        glb con gradino baked in Blender 5.1 → `.zhm` letto da `terrain.c` con
        quote giuste (rampa + plateau ai bordi).
  - [x] **Terreno slice 2 — loader glTF + seating** (`EDITOR_DESIGN.md §9`):
        cgltf (`vat/cgltf_impl.c`, `-w`) + shader `vat/ground.vs/.fs` (texture
        + key NW). Carica tutte le primitive/nodi con matrice mondo, mapping
        glTF y-up → mondo `(x,y,-z)` coerente col bake; texture esterna E
        embedded (`stbi_load_from_memory`). Gli agenti si posano sulla quota
        (post-process del buffer instance, `vat_layer` resta agnostico),
        ostacoli/torrette/base sollevati al `terrain_z`; quad-suolo flat
        saltato col terreno glb. Campo `terrain` nel `.scn` (parse/save +
        env `VAT_HORDE_TERRAIN`), `test_scene` esteso. Verificato headless:
        gradino 3 m con sprite/torrette seduti, texture a scacchi mappata,
        `.zhm` mancante = fallback z=0. Demo `scenes/terrain.scn` +
        `gfx/terrain_demo_make.py`.
  - [x] **Ombre + tracer a quota**: tracer di fuoco sollevati al `terrain_z`;
        **ombre a terra aggiunte** al path VAT (non c'erano) — disco unitario
        instanziato sotto ogni agente alla quota reale del terreno (blob
        morbido blended, `vat/shadow.vs/.fs`), ground via `terrain_z` anche
        sotto chi vola. Verificato su terreno e flat (no z-fight, no regress).
  - [x] **Volo balistico height-aware**: il render del volo abbraccia il terreno
        (`za + terrain_z`, ground corrente = niente galleggiamenti); ombra del
        flyer a TERRA e rimpicciolita con `za` (segnale d'altezza). Sorgente di
        volo in `vat_horde`: tasto `E` (esplosione+lancio al centro camera) +
        headless `VAT_HORDE_BLAST="frame,x,y[,str,up]"`. Verificato: pack
        lanciato a cavallo del gradino, agenti in aria con ombre a terra
        staccate/ridotte, rientro a quota. MANCA: sorgente terreno
        urbana/procedurale (§9 fase 2).

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

- [x] Profilare prima di ottimizzare. Tool: `bench_sim` (zero deps, niente
      SDL/GL/asset — `make bench_sim`, `./bench_sim scenes/stress.scn N w m`).
      Baseline CPU PULITO single-thread (ms/step), lineare ~0.2-0.4 ms/1000:
        N      Ryzen7-3750H(local)   i7-14700(workstation)
        5k       1.97                  0.92
        10k      3.14                  1.61
        20k      5.69                  3.37
        35k     11.5                   6.81
        50k     15.96                10.23
      → 50k sta nei 16.6 ms a 60 Hz single-thread su ENTRAMBE le CPU. I `max`
      per-step = ricalcoli Dijkstra del flow. ATTENZIONE: il `core_sim` misurato
      dentro `vat_horde` sul minipc è gonfiato ~2.5× dalla contesa GPU↔CPU
      dell'APU (Vega integrata); usare `bench_sim` per i numeri CPU veri.
      Il vero margine M4 serve per >50k e per liberare la CPU dal lavoro di
      gioco, non perché 20-50k non ci stiano.
- [x] **Multithreading PBD a tile colorati** (vedi `M4_DESIGN.md`). Thread pool
      pthreads header-only (`sim_threads.h`), parallel_for a cursore atomico.
      Parallelizzati: steering, PBD (tile 2×2 a 4 colori, `PBD_TILE=4`),
      wall_projection, velocity recovery. Seriali: integrate, rebuild_grid, nav,
      drain. DETERMINISTICO a qualunque thread count (checksum identico su
      1..28 thr). `SIMP_THREADS`/`simp_set_threads`. Risultati 50k: i7-14700
      4.2× (10.3→2.5 ms, ginocchio ~16 thr); Ryzen-3750H 1.46× (APU
      bandwidth-bound). Unico ritocco test: floor `test_siege` 0.004→0.006.
- [x] **Spike nav (Dijkstra) ora è il frame peggiore** (`max` ~7-8 ms non scala
      coi thread): ricalcolo incrementale / async su worker / flow parallelo.
      FATTO: (a) `density_update` (blur) e `recompute_flow` parallelizzati col
      pool (per-cella, deterministici); (b) il Dijkstra throttled è DRENATO A
      BUDGET su ~6 step (`nav_phi_begin`/`nav_phi_drain`, heap persistente,
      `NAV_BUDGET_DIV`): phi costruito in un buffer mentre gli agenti leggono il
      flow COMMITTATO, commit del flow solo a heap vuoto. Schedule a budget fisso
      + heap seriale ⇒ DETERMINISTICO a qualunque thread count (checksum
      bench_sim identico su 1/4/8 thr). 50k/8core i7→ Ryzen-3750H: max
      15.5→9.4 ms (avg invariato 6.6); 11/11 test PASS, drain 81.5%.
- [ ] SIMD sui loop di steering e integrazione (SoA già pronto); verificare che
      l'autovettorizzazione faccia già il lavoro prima di scrivere intrinsics.
      VERIFICATO (2026-06-19): gcc 13 NON autovettorizza i job caldi. A -O2 0 loop;
      a -O3 -march=native ne vettorizza 8 ma sono density/blur/SDF/reorder, NON
      `job_steering`/`job_recover` (gather bilineare del flow + branch dormant/
      flying + sqrtf li bloccano). Quindi servirebbe SIMD a mano. MA il profilo
      (8thr/50k, breakdown per-fase) dice steering 9% GIÀ PARALLELO (~0.66 ms):
      molto lavoro per <1 ms reale → ROI BASSO, deprioritizzato. Il vero limite
      di scaling MT sono ora i due `rebuild_grid` SERIALI (~20% combinato); la
      parallelizzazione è già scartata (sopra), l'unica strada residua è rendere
      LAZY l'ultimo dei due (la griglia stale ha già fallback brute-force nelle
      query) — ma il guadagno svanisce se il gioco fa query ogni frame (torrette),
      valore reale incerto. Prossimo candidato solo se serve margine oltre 50k.
- [x] Riordino periodico degli agenti in memoria secondo l'ordine della griglia
      (ogni ~60 step) per località di cache.
      FATTO: `reorder_agents` — counting sort seriale per cella di collisione
      costruisce una permutazione, permuta tutti gli array SoA per-agente e
      ricostruisce la slot map (`index_to_slot` permutato → `slot_to_index`;
      `slot_gen`/`slot_free` per-slot intatti; `landed` sono handle → intatti).
      Deterministico (seriale), cambia l'ordine PBD intra-cella (checksum shift,
      come il tile-coloring) ma IDENTICO cross-thread (verificato 1/4/8/16).
      Manopola `SIMP_REORDER` (default `REORDER_PERIOD`=60, <=0 disabilita).
      NB: gli istogrammi per-thread per `rebuild_grid` (prima idea) sono stati
      SCARTATI: griglia sparsa (nc≈77k > total≈46k), l'array C×nc da azzerare+
      rileggere costa più banda del count/scatter che parallelizzerebbe. Il
      reorder attacca la vera causa (memory-bound) e velocizza rebuild+PBD
      insieme. Misura (`bench_sim`, 50k/8thr): su layout SCRAMBLED (indici
      scorrelati dallo spazio = stato reale dopo churn spawn/kill) avg
      6.78→6.41 ms (~5%); su prefill ordinato è neutro. `BENCH_SCRAMBLE=1`
      aggiunto al bench per misurare il caso realistico.
- [x] Ottimizzare `wall_projection`: saltare il sample SDF se la cella nav è
      lontana dai muri (flag per cella "sdf > r_max + margine").
      FATTO (2026-06-19): maschera `wall_near` (byte per cella nav) = min SDF sul
      blocco 3×3 ≤ `grid_rmax + cell`. Il 3×3 copre la footprint 2×2 del sample
      bilineare per qualunque punto della cella → cella spenta ⇒ SDF(agente) >
      r_max ovunque, lo skip non manca MAI un contatto reale (CONSERVATIVO).
      `job_wall` calcola la cella nav dell'agente e salta del tutto il sample
      bilineare dove la maschera è 0 (campo aperto). Costruita a fine
      `recompute_sdf` (edit muri) e quando `grid_rmax` cresce (spawn più grossi).
      Determinismo: checksum bench_sim BIT-IDENTICO a 1/8 thr (salta solo calcoli
      che darebbero d≥r). 11/11 test PASS, drain 81.8%. Misura per-fase (timer
      temporanei in simp_step, poi rimossi): fase wall 0.78→0.41 ms @8thr (−47%),
      2.20→0.92 ms @1thr (−58%). Il TOTALE su questa APU (Ryzen-3750H) è
      bandwidth/thermal-bound sul PBD e maschera il guadagno; su CPU non
      bandwidth-bound (i7-14700) il ~1.3 ms/step single-thread emerge netto.

## M5 — Gioco difensivo (vedi `M5_DESIGN.md`)

Fase difesa: torrette, danno, ferite, base. Economia (biomassa/droni) = doc
separato futuro (M5b). Slice 1 = loop giocabile con budget di piazzamento
statico. SOLO 2 API nuove al core (`simp_query_ray`, `simp_set_vpref`).

- [x] **`simp_query_ray`** (core, M5_DESIGN §2): hitscan + piercing sulla
      griglia di collisione (DDA Amanatides-Woo + alone 3×3, dedup nearest-
      max_out), occlusione muri (line-of-sight via DDA nav). FATTO:
      `test_turret.c` parte 1 PASS (vs brute force O(N), gridded + fallback
      stantio, piercing/non-pierce, LoS). 12/12 suite verde.
- [x] **`simp_set_vpref`** (core, §5): cambio velocità preferita post-spawn
      (crawl dei maimed_legs, debuff). FATTO: `test_turret.c` D PASS.
- [x] **Stato per-slot** (§3): HP/body/wound/heavy-hits + tabella EnemyDef
      (HP decrescenti obesi→bambini), indicizzato per slot (M3.1). FATTO in
      `defense.c` (modulo di gioco riusabile, core-agnostico).
- [x] **Torrette** (§4): sweep + dwell sul bersaglio, fuoco solo-con-bersaglio,
      leggera (logoramento) vs pesante (gib + `apply_impulse`). FATTO
      (`DefTurret`, `def_update`); piercing via `simp_query_ray` max_out.
- [x] **Ferite a 3 vie** (§5): outfit insanguinato / maimed_arm /
      maimed_legs→crawl; i crawler lenti alzano il jam M3.7 → deviano l'orda.
      FATTO: roll seedato per slot (deterministico), crawl via `simp_set_vpref`.
- [x] **Morte** (§6): cadavere M3.3 (leggera) / gib senza cadavere (pesante) /
      biomassa-stub; ordinamento kill (trappola indici M3.4). FATTO: hit→handle
      prima di ogni kill; tank a N colpi pesanti. `test_defense.c` PASS (13/13).
- [x] **Base + sconfitta** (§7): il core = struttura assediabile più interna
      (riusa SIEGE per intero), goal centrale, game over a `core_hp <= 0`.
      FATTO: strutture distruttibili in `defense.c` (`def_add_structure`/
      `def_struct_cell`/`def_lost`, assedio per-slot in `def_update`, crollo→
      `simp_set_wall(false)`+commit→reroute; il core non reroute, si perde).
      `test_base.c` PASS (reroute esterno→core cade→lost; difesa che regge;
      determinismo; no-NaN). Aggancio visivo in `vat_horde` (`scenes/base.scn`,
      `VAT_HORDE_BASE=1`, `VAT_HORDE_TURRETS=N`): anelli renderizzati dallo stato
      vivo (crollo = spariscono), HUD core/ring HP + BASE PERSA. 2 accessor core
      nuovi (`simp_grid_w`/`_h`/`simp_cell_size`) per decodificare `wall_cell`.
- [x] **Spawn director + budget** (§8): ondate dalle rect di scena (no burst
      via `simp_free_at`), budget statico di piazzamento. FATTO: `DefDirector`
      in `defense.c` (disaccoppiato da scene.h, prende `DefRect`), emissione
      burst-free auto-strozzata, rampa rate+mix per ondata (tank 2→15%),
      callback on_spawn per la variante VAT, RNG seedato deterministico.
      Budget: `def_set_budget`/`def_budget`/`def_spend`. `test_director.c` PASS
      (rate 14.6→43/s, tank% 0.7→15.6, overlap 0.000, budget, determinismo,
      no-NaN). Agganciato a `vat_horde` (sostituisce il pump fisso, HUD
      ondata+budget). **M5 slice 1 COMPLETO** (resta solo M5b economia).

## M5b — Economia (doc separato, futuro)

- [ ] Biomassa: blob alla morte (pool fisso + TTL, come i cadaveri), raccolta
      come tasso/raggio attorno ai punti di raccolta (droni NON simulati in v1).
- [ ] Basi di raccolta avanzate: punti esterni alla base, goal + HP (riusano
      §7), reddito maggiore ma bersaglio dell'orda → dilemma espandi/difendi.
      Richiede l'attribuzione multi-goal del drain (questione aperta).
- [ ] Attacchi speciali (mortaio, bombardamenti) + upgrade torrette, pagati in
      biomassa. Proiettili delle torrette GRATIS (niente spirale).
- [ ] Muri/strutture costruite dal giocatore (collisione già SDF: funziona oggi).
      Barricate runtime che chiudono le strade e instradano l'orda sotto le
      torrette. Richiede il **costo di sfondamento per-cella** (sotto).
- [ ] **[CORE NAV] Costo di sfondamento PER-CELLA** (oggi `WALL_ENTER` è una
      costante globale → tutti i muri allo stesso peso). Promuoverlo a campo
      per-cella per gerarchizzare i muri per appetibilità di sfondamento:
      `palazzo` altissimo ≫ `barricata` alto-ma-minore. La Dijkstra instrada la
      pressione d'assedio sul muro PIÙ ECONOMICO del percorso → l'orda sfonda le
      barricate e NON preme i palazzi indistruttibili (entrare nel palazzo costa
      più che deviare → `-grad(phi)` punta via). Unifica la nav in un costo
      continuo per cella (`strada<fango<barricata≪palazzo`). Collisione separata
      (SDF binaria: palazzo e barricata entrambi solidi; il peso decide *dove* si
      preme, gli HP *se* cade). Abilita lo scenario urbano e le barricate del
      giocatore (sopra). Dettaglio: `SIMULAZIONE.md` §I.4, `M5_DESIGN.md` §7.
- [ ] **Accumulo cadaveri nella NAV (anti-imbuto) — GEMELLO del costo per-cella**
      (design completo in `CORPSE_DESIGN.md`, perno anti-degenere in §7-bis).
      Oggi i cadaveri pesano su `rho`/`jam` (`CORPSE_RHO`) ma restano sotto
      `WALL_ENTER` → re-instradano solo fra strade APERTE. Per il PERNO
      anti-imbuto ("barrico una strada e ammasso le torrette sull'altra": il
      kill-zone si intasa di corpi → reroute → l'orda sfonda la barricata
      scoperta) serve che una pila DENSA salga a costo **scala-muro** (un "muro
      di cadaveri" nella nav) così da competere col costo-di-sfondamento
      per-cella della barricata. Le due feature si progettano INSIEME. Crea
      ritmo/gioco adattivo (decay → fronte oscillante). Leve secondarie lato
      gioco: costo nav sotto le torrette (nudge mite), torrette meno efficaci se
      ravvicinate (safety-net), screamer "intelligenti" (counterplay, dopo).
- [ ] **Spawn event SCRIPTATI (director-per-uscita)** — estensione del director
      §8 per lo scenario urbano (uscite metro/cancelli/portoni con flussi
      propri). Soluzione: UN `DefDirector` per uscita (oggetto già indirizzabile;
      le uscite sono sempre poche). Tre campi per-director: (a) `start_delay` o
      attivazione su EVENTO (prossimità giocatore / crollo struttura / fine
      ondata, lega allo stato `dormant`); (b) `pool` = totale da emettere, scala
      solo sugli spawn riusciti (`emitted` già lo fa → cap/congestione non
      bruciano il pool) e ferma a 0; (c) `base_rate` esistente (`rate_ramp=0` =
      flusso costante). Strozzamento fisico `simp_free_at` gratis sopra (varco
      stretto = coda). Es: "t=180s cancello apre, 300 zombie a 5/s poi finisce"
      = `{start_delay:180, base_rate:5, rate_ramp:0, pool:300}`. Livello = LISTA
      di config (data-driven, si dipingono nell'editor M7+). Dettaglio in
      `M5_DESIGN.md` §8. NB: alzare `DIR_RECT_CAP` (ora 16) se servono >16 rect
      per un singolo director.

## M6 — Rendering & animazione

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
- [ ] **Heightmap VISIVA del terreno** (dislivelli LIEVI, solo estetica, NESSUN
      impatto gameplay — vedi `GFX_DESIGN.md` §9). Core intoccato (sim 2D pura
      su z=0): costo simulazione zero, vive tutto nel renderer. Lavoro piccolo:
      (1) suolo da quad piatto a griglia tassellata displaced da H(x,y) (mesh
      statica, ~16k tri/100×80 m), normali da differenze finite; (2) sorgente H
      procedurale (agreste) o derivata dai bordi strada (marciapiedi urbani);
      (3) agenti sollevati di H via texture nel vertex shader (CPU 0) o sample in
      vat_layer_fill. Gotcha: muri/torrette/ombre vanno campionati a H o
      fluttuano (v1 = solo suolo+agenti); volo balistico su z=0 piatto si scolla
      sul ripido → tenere LIEVE. Fase 1 = una sessione (90% del look); fase 2 =
      sorgente urbana + height-aware degli altri elementi.
## M8 — Scala estrema / GPU (solo se serve oltre ~100k o per liberare la CPU)

- [ ] Port dei passi a compute shader GL 4.3 (counting sort con atomics, PBD a
      colori, integrazione). Rendering instanced direttamente dai buffer GPU,
      zero roundtrip CPU. La struttura attuale mappa 1:1 sui dispatch.
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

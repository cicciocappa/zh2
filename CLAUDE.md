# Horde game — project memory

Progetto: tower defense top-down "physics-based" con orde massive (target: 50k–100k
nemici simulati), riferimento creativo *Sir, We Have an Orc Problem* + *Creeper World*.
Stack: **C + SDL3** per il sandbox; i core di simulazione sono **renderer-agnostici,
C puro, zero dipendenze** e portano su OpenGL (instancing + compute shader in futuro).

Conversazioni in **italiano**. Codice e commenti in **inglese**. Si lavora da terminale
(compila e testa sempre prima di consegnare). Vedi `TODO.md` per l'agenda operativa.

## Preferenze di lavoro (l'utente)

- **Verifica visiva all'utente prima dell'analisi con screenshot.** Per le
  modifiche **visive/di rendering**: fare la modifica, compilare, dire in una riga
  cosa guardare, poi **fermarsi e lasciar verificare a occhio all'utente** — non
  partire subito con build headless + screenshot + diff. Lui spesso vede al volo
  cosa non va e si perde meno tempo. Screenshot headless solo se l'utente non può
  guardare interattivamente, se li chiede, o per test di CORRETTEZZA (es. unit
  test testuali), non come prima validazione estetica.
- **Niente probing dell'ambiente; download/installazioni li fa l'utente.** Non
  andare a cercare cosa è installato sul sistema (no `find /`, no scan di
  toolchain/pacchetti/rete): **chiedere sempre all'utente**. Se serve qualcosa da
  internet, **dirlo all'utente** che lo scarica e indica dove l'ha messo — non
  assumere accesso di rete.
- **Preferire librerie leggere** a quelle pesanti (es. UI sandbox: nuklear/microui,
  single-header C puro, invece di Dear ImGui/cimgui che vuole il toolchain C++).
  Coerente con l'etica "zero/poche dipendenze": i core sono zero-dep; i tool
  (`vat_horde`/`fxlab`) ammettono SDL3/glad/stb/cgltf/nuklear.

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
     attrarre l'orda, che va a premere contro le mura (assedio). Il costo
     d'ingresso è PER-CELLA (`wall_cost[]`, default `WALL_ENTER`;
     `simp_set_wall_cost`): tier alto = palazzo/roccia (l'orda devia via), tier
     basso-ma-≫aperto = barricata (bersaglio d'assedio preferito). La collisione
     SDF è indipendente: entrambi i tier sono solidi, il costo decide solo DOVE
     si preme. Vedi `SIMULAZIONE.md` §I.4 e `test_breakthrough.c`.
     Gli archi sono pesati per cella destinazione (M3.5+3.6+3.7):
     `dist · clamp(1 + k_density·min(rho/rho_max,1) + k_jam·min(jam/rho_max,1)
     + k_danger·min(danger/danger_ref,1) + cost_user, ≥0.2)`.
     `cost_user` (`simp_add_cost`, clamp [-0.8,100]) = paura/fango (w>0) o
     richiamo screamer (w<0), PERMANENTE; `danger` (`simp_add_danger`, perno
     anti-imbuto/anti-killbox 2026-06-25) = paura DA SANGUE, campo per-cella che
     DECADE (`danger_hl`, ~30 s = vita del decal di sangue): ogni morte timbra
     la cella (`defense.c` ai punti morte, accanto alla decal), l'orda evita per
     istinto le killzone insanguinate. **GRADUATO** come il vecchio termine
     cadaveri: `k_danger·min(danger/danger_ref,1)`, `k_danger`=tetto a
     SCALA-MURO (default 400), `danger_ref`=sangue per saturare (default 8).
     Sangue sparso (danger≪ref) → nudge SOFT (dirotta fra strade aperte, e il
     PBD shova comunque il fronte → la massa inonda lo stesso); killbox tenuto
     (danger satura) → costo SCALA-MURO → l'orda sfonda le BARRICATE del
     giocatore invece di alimentare l'imbuto (la strategia "tutte le torrette
     in un punto + muri attorno" si auto-sabota). Resta < `WALL_ENTER` per
     cella (palazzi indistruttibili e assedi a goal murati intatti); vince il
     muro più economico → cadono prima le barricate dei palazzi. SOSTITUISCE il
     costo-nav-cadaveri §7-bis (`k_corpse` ora default 0, macchina dormiente);
     `rho` = densità per cella nav (istogramma +
     blur 3×3 + EMA, cadaveri pesati ×2) → l'orda aggira gli ingorghi da sola
     (Continuum Crowds light); `jam` (M3.7) = stesso istogramma ma pesato per
     FERMEZZA, `(1−min(|v|/v_pref,1))²` per agente (cadaveri e dormienti a
     peso pieno) → la densità da sola satura a k_density e non può esprimere
     un varco a throughput zero (costo reale in tempo: infinito), il jam
     rende cara solo la folla FERMA e il Dijkstra devia su strade alternative
     anche molto più lunghe; quando il tappo si scioglie l'EMA decade e la
     rotta diretta torna a vincere. Resta sotto WALL_ENTER: gli assedi a goal
     murati non cambiano. phi/flow si ricalcolano su throttle
     (`flow_period`, default 0.5 s) quando densità o costi cambiano; le
     modifiche al terreno forzano il commit completo subito. Scratch nav
     preallocati: niente malloc in `simp_step`. M4: il Dijkstra throttled è
     DRENATO A BUDGET su ~6 step (`nav_phi_begin`/`nav_phi_drain`, heap
     persistente, `NAV_BUDGET_DIV`) per non spikare un singolo frame — phi
     costruito in-place mentre gli agenti leggono il flow COMMITTATO, commit
     del nuovo flow solo a heap vuoto; deterministico (budget fisso + heap
     seriale, phi bit-identico al monolitico). density blur e flow sono
     parallelizzati col pool (per-cella).
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
   - **Stato comportamentale** (`simp_flags_arr`, byte di flag per agente):
     `SIMP_DORMANT` = frena verso velocità zero ma collide e assorbe spinte,
     non drenato dai goal (`simp_spawn_dormant`, `simp_wake_radius/all`);
     `SIMP_FLYING` = volo balistico su z fittizia (M3.2): niente steering né
     collisioni (escluso dalla griglia), sorvola i muri sopra `wall_h` (2 m),
     gravità 9.81, all'atterraggio `landing_damp` (0.3) ammazza il momento
     orizzontale (via bend di `qx`: il recovery di velocità lo rispetti) e
     l'agente finisce nel buffer `simp_landed` come HANDLE (drain-safe), per
     il danno da caduta. Lancio con `simp_apply_impulse_ex(..., up_ratio)`.
     Il drain vale anche in volo (semplificazione annotata).
   - **Cadaveri** (M3.3 + §3): dischi nella stessa griglia di collisione
     (`simp_corpse_add`, pool fisso 4096, TTL; pieno = rimpiazza il più vicino
     a scadenza). Implementati come "ghost" appesi in coda agli agenti negli
     array SoA prima del binning; il kernel PBD non ha casi speciali (solo
     guardia coppia ghost-ghost, per indice `>= count`). **Massa finita (§3,
     `corpse_weight` unità walker, default 40, `<=0` = sigillo infinito legacy):
     `invm = 1/(weight·r0²)` → l'orda che spinge SHOVA la pila a valle e sfonda
     ("rallentano ma non fermano"); le posizioni spinte dal PBD sono riscritte
     nel pool a fine PBD (step 3b di `simp_step`).** Bloccano `simp_free_at`,
     invisibili a query e impulsi, MAI marcati nel nav grid: la deviazione del
     flusso attorno ai mucchi emerge da PBD + costo nav cadaveri (sotto).
   - **Accumulo cadaveri → costo nav** (CORPSE_DESIGN.md §7-bis) —
     **PENSIONATO (2026-06-25): `k_corpse` default 0, sostituito da
     sangue→paura (`danger`, sopra).** L'anti-imbuto ora è il campo di
     pericolo del sangue, non l'altezza della pila; i mound 3D di render sono
     stati ELIMINATI (erano brutti). La macchina sotto resta nel core, dormiente
     (riattivabile settando `k_corpse`>0, es. `test_corpse_pile`):
     tre campi per CELLA nav accanto a `rho`/`jam`.
     `corpse_mass` += π·r² a ogni `simp_corpse_add` (decay lento `corpse_mass_hl`,
     indipendente dal TTL del pool: il volume marcisce per conto suo);
     `corpse_pack` += `pack_inc` per agente a terra che calpesta una cella con
     massa (decay più rapido → le pile non calpestate si rigonfiano); derivato
     `corpse_height = k_h·mass/(1+k_pack·pack)` (m, `simp_corpse_height`). Termine
     d'arco `k_corpse·min(height/wall_h,1)` aggiunto a `cost_mult` in
     `nav_phi_begin`: una pila DENSA sale a costo **scala-muro** (tetto
     `k_corpse`=300, satura a `wall_h`) → batte il `wall_cost` di una barricata e
     il Dijkstra devia l'orda a sfondarla (il varco-killzone intasato si scopre
     da solo). Resta SOTTO `WALL_ENTER`: palazzi e assedi a goal murati intatti.
     `simp_corpse_clear(x,y,r)` = fuoco/acido (rimuove corpi fisici + azzera i
     campi). Default ON. Con la **massa finita §3** ora attiva, il traffico che
     sfonda una pila la calpesta → `pack` sale e abbassa height/costo nav; il
     decay resta la garanzia di non-permanenza. Lo scaling `pack`→`invm`
     (cedimento fisico progressivo) è stato SCARTATO (23 giu 2026): inerte
     (`corpse_pack` non si accumula — il PBD tiene i centri agente fuori dalle
     celle cadavere) e ridondante (l'anti-imbuto è già il costo nav §7-bis).
     RESTA: la tabella armi §6. La RAMPA
     §4 è TAGLIATA (decisione 23 giu 2026: due vettori di sconfitta sullo stesso
     muro = troppo carico di leggibilità, l'anti-degenere è già dato da
     reroute+decay, lo stallo su muro indistruttibile lo rompono i flyer;
     "scavalcano i cadaveri" resta solo fiction visiva). Vedi CORPSE_DESIGN.
   - **Spawn senza burst**: `simp_free_at(x,y,r)` dice se un disco ci sta senza
     sovrapporsi ad agenti o muri (griglia se attuale, altrimenti brute force).
     Emettere solo dove è libero elimina l'espulsione PBD allo spawn e fa
     auto-strozzare gli emitter alla portata dell'uscita (flusso da tunnel).
   - **Handle stabili** (M3.1): slot map con generation counter sopra lo
     swap-and-pop. `simp_handle_of` / `simp_index_of` / `simp_slot_of`; i dati
     di gioco per-agente (HP, tipo…) vanno indicizzati per SLOT, stabile per
     tutta la vita dell'agente. 20 bit slot + 12 bit gen, 0 = invalid.
   - **Tipi di nemico** (M3.5): `simp_spawn_desc(x, y, desc)` con raggio,
     v_pref e massa espliciti (massa in unità walker: 1.0 = agente default;
     il PBD usa solo rapporti di massa → il tank massa 10 sposta i walker e
     non viene deviato). `v_pref` riceve comunque il `v_jitter` globale.
     La tabella tipi (HP, bounty…) vive nel gioco, indicizzata per slot.
     Uno spawn (o cadavere) più grande del default ingrandisce la cella della
     griglia di collisione: corretto ma più lento per tutti — boss entro ~2×
     il raggio default. `simp_sleep` = controparte dei wake per branchi
     dormienti tipizzati.
   - **Query spaziali** (M3.4): `simp_query_circle` (indici nel raggio, satura
     a max_out) e `simp_query_nearest` (scansione ad anelli con lower bound).
     Esatte tra uno step e l'altro: la griglia viene RICOSTRUITA a fine step
     (le correzioni PBD/wall e il drain rendono stantio il binning di metà
     step). Trappola documentata nell'header: gli indici ritornati muoiono al
     primo kill — convertirli subito in handle, o killare per indice decrescente.
   - **Sensore d'assedio** (`SIEGE_DESIGN.md`): `simp_wall_pressure[i]` =
     `push · max(into_wall,0)` e `simp_wall_cell[i]` = cella muro assediata,
     per agente a terra, fotografati a fine `wall_projection`. Espongono CHI
     preme un muro per raggiungere il goal oltre (e dove): `into_wall =
     -(flow·normale)` separa l'assedio frontale (>0) dallo sfioramento
     tangente (≤0, futura meccanica hazard). Il gioco ci costruisce sopra HP
     delle strutture + attacchi discreti (timer per-slot, soglia di contatto)
     → crollo via `simp_set_wall(false)` → reroute automatico.

## Stato verificato (core particellare)

- Compila pulito (`-O2 -Wall -Wextra -std=c11`), zero dipendenze oltre libm.
- `test_particles`: griglia 320×240 (cella 0.5 m), ~13k agenti, muro con due varchi
  da 4 m, 9000 step a 60 Hz. Zero NaN/out-of-bounds, overlap residuo medio ~1–2 cm,
  82% drenato al goal (71% prima di M3.6: con densità→costo l'orda si spalma
  sui due varchi da sola; congestione comunque realistica — cunei granulari
  contro il muro, getti a ventaglio oltre i varchi, verificato visivamente
  sui frame PPM). ~4.0 ms/step single-thread (densità attiva).
- `test_impulse`: esplosione in folla impaccata → picco 14 m/s, rientro sotto
  4 m/s in pochi secondi, zero NaN. Fase volo (M3.2): 356 lanciati con
  `up_ratio` 0.5 → tutti atterrati entro il tempo balistico (step 87 ≈
  2·vz/g), vx ESATTAMENTE costante in volo, `landed` = lanciati, conteggio
  conservato.
- `test_corpses` (M3.3): corridoio 4 m, colonna di 200; aperto drena 145 in
  45 s, sigillato da 9 cadaveri drena 0; cadaveri immobili al bit sotto piena
  pressione della folla, penetrazione residua peggiore 1.8 cm; il TTL svuota
  il pool puntuale. La barricata parziale emergente (pennello KILL) si
  verifica a occhio nel sandbox.
- `test_handles`: 1000 spawn + kill casuali + 3 round di riuso slot contro
  shadow map brute force: ogni handle vivo risolve alla posizione giusta, ogni
  handle morto dà -1 anche dopo il riciclo dello slot.
- `test_query`: 10k agenti, 200 query circle e 200 nearest contro brute force
  O(N) (zero mismatch), saturazione del buffer, scenario torretta
  (nearest→handle→kill, esercita anche il fallback a griglia stantia) e AoE
  con kill per indice decrescente. Il secondo counting sort di fine step costa
  ~0.4 ms a 13k (3.41→3.85 ms/step in `test_particles`; 4.09 dopo M3.2+3.3,
  i flag nei loop caldi — primo candidato se M4 chiede margine).
- `test_dormant`: branco piazzato via `simp_free_at` (zero coppie sovrapposte),
  fermo immobile per 300 step con goal attivo, `wake_radius` sveglia solo il
  sottoinsieme (che marcia e drena), `wake_all` svuota la mappa. Nota: svegliare
  il lato lontano del branco è lento — i marciatori devono spingersi attraverso
  i dormienti (ostacolo mobile, comportamento voluto).
- `test_types` (M3.5): corsa in campo aperto (runner doppiano i walker: mean x
  30.4 vs 18.5 m a 10 s), pressing con 5% tank al varco (720/720 drenati, tank
  inclusi — ATTENZIONE: goal a ridosso del bordo profondi ≥ 2 celle, o il
  centro del tank non li raggiunge e i tank parcheggiati barricano il drain),
  costo utente su due corridoi simmetrici (repulsione +5 → 100% sull'altro,
  attrazione −0.5 → 98%).
- `test_density_route` (M3.6): varco stretto diretto vs varco largo in deviazione;
  con `k_density` 0→2.5 l'uso del percorso lungo sale 22%→57% e il drain del
  90% scende da 5382 a 4552 step. Zero NaN. (Il test pinna `k_jam = 0` per
  isolare il termine M3.6.)
- `test_jam` (M3.7): varco diretto nav-aperto ma sigillato da un cadaverone
  a massa infinita (l'ingorgo idealizzato), deviazione larga ~46 m più lunga
  (oltre la portata della densità satura). Con solo densità il branco (440)
  resta in coda al varco morto (29% drenato al cap, solo la frangia sud
  devia); con `k_jam` 8 la coda si prezza fuori mercato e il 90% drena dalla
  deviazione in 6884 step. Sweep: 4→14 monotono (più alto = ribaltamento
  più rapido). Zero NaN. Attenzione misurata: con branchi GRANDI (800+) la
  sola densità satura su un'area enorme e finisce per deviare anche lei
  (lentamente) — il jam serve per code piccole rispetto alla deviazione e
  per la reattività.
- `test_siege` (`SIEGE_DESIGN.md`): selettività del sensore (micro-test
  deterministico: stesso muro + blocco, flow into-wall 0.033 vs tangente
  0.0037 sotto il floor d'attacco) e meccanica completa (orda ~1365 preme
  una banda distruttibile → 400 colpi → crollo step ~2721 → reroute → >50%
  drenato; controllo indistruttibile = 0 drain; determinismo; zero NaN).
  Nota: l'assedio frontale sostenuto richiede una folla che spinge — un
  agente singolo nel muro viene espulso come velocità e non resta a premere.

## File

### Struttura cartelle asset (2026-07-02)

Gli asset RUNTIME (caricati dagli eseguibili) vivono tutti sotto `assets/`,
separati dai file di LAVORO Blender/FBX in `blend/`:

- `assets/models/` — mesh glb generiche: `gibs.glb` (gore), `road_test.glb`,
  `light_turret.glb` / `heavy_turret.glb` (torrette: due nodi `base` fisso +
  `gun` che ruota sull'angolo di mira e rincula al colpo).
- `assets/zombies/` — bake VAT degli agenti (`*_mesh.bin/_pos.raw/_norm.raw/
  _meta.txt/_diffuse.png`) + `outfits/` + le texture di riferimento.
- `assets/terrain/` — terreni glb + `.zhm` bakati (gitignored).
- `assets/scenes/` — file di scena (`.scn` vettoriali + `.txt` legacy).
- `assets/props/` — `catalog.txt` (catalogo prop di decoro).
- `assets/shaders/` — tutti i GLSL (`.vs`/`.fs`); il Makefile li raccoglie in
  `$(SHADERS) = assets/shaders/*.vs *.fs` come dipendenza.
- `blend/` — file SORGENTE non caricati a runtime: `.blend`, `.blend1`, gli FBX
  dei modelli riggati; `blend/mixamo/` = gli FBX delle clip d'animazione Mixamo
  (era `vat/source/`, input di `vat/bake_zombie.sh`).

I path sono hardcoded nei sorgenti (niente asset-root configurabile): gli
eseguibili si lanciano dalla root del progetto.

- `sim_particles.h` / `.c` — core particellare (vedi sopra). API `simp_*`.
  SoA pubblici (`simp_px/py/vx/vy/radius_arr`) pronti come instance buffer.
- `fx_particles.h` / `.c` — particle system grafico GENERALE (FX_LAB step 2),
  zero-dep come i core (solo libm), RNG xorshift deterministico, niente malloc
  nello step. Simula particelle billboard 3D (gravità/drag/vento, colore+scala
  start→end, blend alpha/additivo, emitter burst/cono/continui data-driven
  `FxEmitterDef`) e ne emette i dati istanza via `fx_collect`; il disegno GL sta
  nel tool (come i pool gib/decal). Callback quota terreno → stop a `ter_z`.
  Preset `BLOOD_DEF` = schizzo sangue; base per fuoco/fumo/esplosioni. Vedi
  `FX_LAB.md`. Gli FX sono solo visivi → niente test deterministico (scelta 24
  giu 2026). Shader `assets/shaders/particle.*`. Il **mesh-gib** (gore con mesh 3D vere:
  braccio reciso + frammenti, `assets/models/gibs.glb`, shader `assets/shaders/mesh.*`) vive nel
  pool condiviso di `vat_layer` (`vat_layer_maim_arm`/`fill_mesh_gibs`), reso da
  `fxlab` (`load_gib_meshes`). Migrazione in `vat_horde` = accendere il rendering.
- `test_particles.c` — verifica headless: scena chokepoint, metriche, frame PPM
  in `frames/`.
- `test_impulse.c` — smoke test esplosione (picco + assestamento).
- `test_dormant.c` — verifica stato dormiente, `simp_free_at`, risvegli.
- `test_handles.c` / `test_query.c` — verifica handle (M3.1) e query (M3.4).
- `test_corpses.c` — verifica cadaveri-ostacolo (M3.3) + massa finita (§3):
  sigillo infinito (`corpse_weight=0`) vs rallentamento §3 (default 40, la pila
  leaka e i corpi vengono shovati); il volo (M3.2) è in `test_impulse.c`.
- `test_corpse_pile.c` — verifica del perno accumulo cadaveri→costo nav
  (CORPSE_DESIGN §7-bis): pivot anti-imbuto (pressione barricata 0→11 jam→114
  cadaveri: il jam da solo non batte una barricata, il termine cadaveri sì),
  decay→non permanenza, `simp_corpse_clear` riapre il varco, determinismo+no-NaN.
- `test_types.c` / `test_density_route.c` / `test_jam.c` — verifica tipi +
  costo utente (M3.5), densità→costo (M3.6) e ingorgo→costo (M3.7).
- `test_blood_fear.c` — verifica sangue→paura (`simp_add_danger`, perno
  anti-imbuto/anti-killbox 2026-06-25): (1) reroute SOFT — killzone insanguinata
  davanti al varco diretto → `k_danger` 0 va dritta (90%), `k_danger` 6 dirotta
  sulla deviazione aperta (96%), entrambi drenano (dirotta, non sigilla);
  (2) decay esatto a mezza-vita (0.500 su `danger_hl`); (3) determinismo + no-NaN;
  (4) SCALA-MURO — varco aperto insanguinato vs barricata a basso costo: col
  default wall-scale (400) la pressione sulla barricata salta 0.06→90.9 (l'orda
  la preme invece di alimentare l'imbuto), il drain cala solo in parte (il varco
  resta aperto, il PBD ne shova una frazione). Pinna `k_density=k_jam=0`.
- `test_breakthrough.c` — verifica del costo di sfondamento PER-CELLA
  (`simp_set_wall_cost`): l'orda concentra l'assedio sul tier barricata (basso)
  invece dei palazzi (alto), 53% vs ~0% col tier uniforme.
- `test_siege.c` — verifica del sensore d'assedio (`SIEGE_DESIGN.md`):
  selettività into-wall vs tangente + la meccanica completa lato gioco
  (attacchi discreti → crollo → reroute → drain).
- `test_cover.c` — verifica dell'asse C opacità ai proiettili
  (`ENTITY_DESIGN.md` §4+§7, 2026-07-03): `simp_set_opacity` per-cella
  (default: specchia `solid`, 1/0 — bit-compatibile; override DOPO
  `simp_set_wall`) + `simp_ray_transmit` (prodotto `(1-op)^(len/cella)` sul
  DDA, fast-path binario su mappe senza celle semi-trasparenti via flag
  `has_opacity`). `simp_wall_ray`/`simp_query_ray` occludono solo sull'opaco
  pieno (op≥0.999): la CANCELLATA (solida, op 0.3) si spara attraverso, il
  muro no. In `defense.c`: acquisizione a soglia `T_ACQ` 0.05, danno leggero
  `dmg·transmit` (moltiplicatore deterministico, zero RNG), colpo pesante
  `hheat += transmit` (float per slot) → dietro cancellata servono
  ceil(1/transmit) colpi. Test: trasmittanze a mano (diagonale, mezza cella),
  DPS ×0.7 dietro cancellata / zero+non-ingaggio dietro muro (regression LoS),
  preferenza d'acquisizione, crollo cancellata assediata → opacità 0 → DPS
  pieno, determinismo. NOTA misurata: l'assedio frontale richiede orda densa
  hex-packed stile `test_base` (~380), un pack rado resta sotto
  `ATTACK_MIN_P`. Catalogo §6 parsato in `props.c`
  (colonne opzionali `[solid H [WxD]] [hp mult] [opac] [mass]`, `inf` ok,
  esercitate in `test_props`); applicazione host FATTA (2026-07-04): vedi
  `prop_world.c` sotto.
- `prop_world.h` / `.c` — applicazione host degli assi catalogo §6+§8.5
  (2026-07-04): `prop_world_apply` (chiamata da `build_world` in `vat_horde`
  PRIMA del prefill) rasterizza il footprint `WxD` di ogni prop `solid`
  (rettangolo ruotato dello yaw, scanline `scene_raster_cells` resa pubblica
  da `scene.c`; omesso = 1 cella nav, lati clampati a ≥ cella) in celle muro:
  hp finiti → `def_add_structure` assediabile a tier `cost_mult` (crollo →
  celle libere + reroute; il render scurisce col danno e sparisce al crollo);
  hp `inf`/0 → muro permanente a tier ≥ palazzo (10×: un bersaglio
  insfondabile non deve mai vincere l'asta d'assedio); opacità per-cella
  (asse C, default 1 sui solidi). Asse D (massa finita → corpo draggable)
  NON applicato in v1. `test_prop_world` = raster vs brute force (0°/90°/31°),
  tier, trasmittanza, WxD omesso, determinismo. In `vat_horde`:
  `load_prop_models` carica la colonna mesh del catalogo come glb flat-color
  (triangle soup pos+nrm+baseColorFactor, niente texture, fallback
  placeholder procedurale); libreria placeholder `blend/props.blend`
  (`gfx/props_library_make.py`) esportata con `gfx/props_export_glb.py` in
  `assets/models/props/*.glb`; banco visivo `assets/scenes/props_demo.scn`
  (bus/building indistruttibili + cancellate assediabili davanti a due
  torrette). STRUCT_CAP di defense alzato a 192 (mura + torrette + prop).
- `scene.h` / `scene.c` — file di scena VETTORIALI (M7, giugno 2026; sostituisce
  il vecchio formato ASCII a griglia di char). Entità in METRI rasterizzate
  sulla griglia all'instantiate: `cell`/`world W H`/`set <param>`, rect
  `goal`/`spawn`/`pack`/`cost x y w h [w]`, e OSTACOLI `poly <height>
  solid|cost <w> x0 y0 x1 y1 …` = poligoni convessi con un'altezza di
  render (estrusione 3D) e un effetto nav (muro o costo Dijkstra).
  GAME-SIDE (memorizzati in scena ma APPLICATI dall'host `build_world`, non da
  `scene_instantiate` — come i prop): `wall <hp> <cost_mult> x y w h` = muro
  DISTRUTTIBILE (rect di celle = una struttura `def_add_structure`, costo di
  sfondamento per-cella = `cost_mult·base`; breccia = gap fra segmenti, sezione
  debole = segmento a hp+mult bassi) e `turret x y [range] [heavy]` = torretta
  fissa. Rasterizzazione scanline even-odd (gestisce anche concavi; il render fan
  vuole convessi). `scene_instantiate` è deterministico (stessa scena ⇒
  stessi agenti). Modulo separato, il core non lo conosce; gli spawner
  (rect) restano del chiamante. `test_scene.c` = roundtrip + raster +
  determinismo. Esempi in `assets/scenes/` (`obstacles.scn`); MAPPE-BANCO (mura
  distruttibili, 2026-06-25): `arena4` (4 mura uguali), `arena_weak` (3 forti
  +1 debole), `arena_breach` (doppia cinta + breccia difesa da torrette vs
  sezione debole = banco sangue→paura). `VAT_HORDE_RATE` accelera lo spawn per
  osservare i crolli. NOTA: il vecchio
  sandbox 2D `sandbox_particles.c` usa ancora l'API a griglia e NON compila
  finché non è portato (TODO M7); non è nei target di default. I `.txt`
  legacy (`chokepoint`/`fortress`) sono nel vecchio formato, non più caricabili.
- `BLENDER_LEVEL.md` — convenzione di authoring livelli in BLENDER → `.scn`
  (decisione 3 lug 2026, supera in parte EDITOR_PLAN): prefissi nome oggetto
  = tipo entità, custom properties = parametri, collection `terrain`/`statics`
  → glb + `.zhm` + poly solid dai footprint, prop = collection instances
  dalla libreria di catalogo. Export: `gfx/export_scn.py` (FATTO, gira in
  Blender headless; riusa `bake_zhm()` fattorizzato in `terrain_bake.py` su
  motore BVH — regressione bit-identica), validazioni BLOCCANTI (prefisso
  ignoto, chiave prop fuori catalogo, fuori-mondo, limiti formato: exit 1 e
  niente output). Fixture: `levels/test_level.blend` (generato da
  `gfx/test_level_make.py`, esercita ogni regola) → `scenes/test_level.scn`
  verificato con `scene_load`+`scene_instantiate`. La modalità EDIT in-game
  resta come tuning/debug; il `.blend` è la fonte di verità del livello.
- **Richiamo da fuoco** (`def_set_fire_lure`, 2026-07-04): una torretta che
  SPARA attira l'orda — cono di `cost_user` negativo (peso al centro, falloff
  lineare al bordo; il minimo DEVE stare al centro: un plateau piatto fa solo
  sfiorare le rotte, misurato) applicato finché ha sparato negli ultimi
  `linger` s, rimosso ESATTAMENTE al silenzio/crollo (delta per-cella letti
  di ritorno da `simp_user_cost`: il clamp a −0.8 rende sbagliata la
  sottrazione cieca sotto overlap). Default gioco (vat_horde): −0.8, r 8 m,
  linger 2.5 s (`VAT_HORDE_LURE`, w≥0 = off; default defense = off) + reach
  di contatto 2.0 m. Le torrette nel flusso vanno DIFESE (barricate davanti
  = fase B). `test_lure`: impunita senza (250 HP), sbranata con (109),
  interazione col sangue misurata (k_danger 400 → 174: la paura attenua ma
  non azzera, e continua a dirottare la massa — perno anti-killbox intatto),
  rimozione bit-esatta, determinismo.
- `mission.h` / `.c` — macchina a stati di missione (GAME_PLAN fase A, FATTA
  2026-07-04): PREP (exit muti, piazzamento aperto; `prep 0` = illimitata,
  si esce con `mission_go`/INVIO) → ASSAULT (un director per `exit` di scena,
  `start_delay`+`pool` finiti aggiunti a defense §8) → WON/LOST. SURVIVE
  vince al timer, CLEAR a pool esauriti e zero vivi non dormienti; `def_lost`
  perde ovunque. Formato `.scn` esteso: `exit x y w h rate [delay] [pool]`,
  `lz x y` (in vat_horde → core assediabile 5×5 + goal, `build_lz_core`, HP
  `VAT_HORDE_LZ_HP` def 1500), `mission survive|clear s [prep N] [budget N]`,
  `budget N`, `wave n exit_idx count rate [tank pct] [obese pct]`
  (LOOP_DESIGN A 2026-07-16: assalto a ondate annunciate — stesso n =
  partono insieme, con le `wave` le exit sono puri rect di spawn; in
  ASSALTO pausa-annuncio `wave.pause` → ondata → pausa; `mission_call_next`
  salta la pausa e ritorna i secondi risparmiati → bonus bio `wave.bonus`;
  API HUD `mission_wave_total/current/pending`). Con GAME_SHELL comanda la
  shell (in APP_PREP missione congelata, START_ASSAULT → `mission_go`);
  senza, INVIO. `def_struct_damage`
  = danno diretto pubblico (nato qui, serve a EXPLOSION_DESIGN §8).
  `test_mission` = vinta/persa/CLEAR/pool esatti/ondate/determinismo; banchi
  `assets/scenes/mission_demo.scn` e `wave_demo.scn`.
- `bio.h` / `bio.c` — economia biomassa **v2** (BIOMASS_DESIGN.md, riscritta
  2026-07-14; la v1 col convertitore a output selezionabile è stata smontata,
  UX bocciata). Modulo game-side zero-dep: UN serbatoio di UNA valuta.
  Ogni kill frutta biomassa (resa per body, host) fino a `cap` (default 500);
  oltre è SPRECATA (`bio_add` ritorna lo sprecato → flash HUD, `bio_full`).
  Le azioni si pagano a domanda dal serbatoio (`bio_take(cost)`, rifiuta se
  corto): niente item, niente store, niente output da selezionare.
  Lato defense: caricatori torretta (`mag_size`/`mag`/`reload_s`/`reload_t`
  in DefTurret, mag_size 0 = infinito legacy bit-identico; flame/acid scalano
  a tick), `def_turret_reload_now`/`_reloading`, `def_struct_repair` (mai su
  crollate) e **`def_turret_set_facing`** (REGOLA: trasla l'arco, ampiezza
  conservata, la torretta ri-acquisisce da sé).
  Host (vat_horde, GAME_SHELL) — la barra d'ASSALTO mostra SOLO i verbi del
  giocatore (§4) più il serbatoio: **MORTAIO** (M, 40 bio a colpo, aiming
  come prima), **RIPARA** (R, LMB TENUTO su una struttura = flusso
  biomassa→HP a 100 HP/s, 1 bio = 1 HP, si ferma a hp_max/serbatoio vuoto/
  rilascio), **REGOLA** (V, drag da una torretta = nuovo facing; gratis;
  disponibile anche in PREP, pulsante accanto ad ANNULLA). Fuori modalità,
  click su una torretta in ricarica = ricarica istantanea a costo per kind
  (25/35/30/30). La torretta in ricarica porta una **barra di reload
  world-space** (transitoria e actionable — le barre HP permanenti restano
  bandite) e reload default **12 s**: la finestra muta deve far male.
  Costi/mag/resa ora in **`assets/balance.cfg`** (modulo `balance.h/.c`,
  2026-07-15: chiavi `mortar.cost`, `bio.*`, `turret.*.mag/reload_cost`);
  mag default per kind 40/12/30/25 (`VAT_HORDE_MAG`/`VAT_HORDE_RELOAD`
  vincono sul file; sandbox non-shell = infiniti salvo env). Scene: `biotank [start] [cap]`
  (`biostock` v1 = parsato, ignorato, warning). Budget $ (PREP) e biomassa
  (ASSALTO) NON si convertono. Gli **upgrade escono dall'assalto**: pannello
  al debrief (§7) DA FARE col Blocco 3. `test_bio` (add/cap/spreco esatto,
  take/rifiuto, cap knob, determinismo) + `test_turret_mag` (guardia legacy,
  ciclo colpi→silenzio→ripresa, reload_now, tick flame, lure spento in
  ricarica, REGOLA, determinismo).
- `balance.h` / `balance.c` — **tabella di bilanciamento runtime**
  (Blocco 3, 2026-07-15): TUTTI i numeri attacco/difesa in una struct
  `Balance` caricata da **`assets/balance.cfg`** (`chiave = valore`, `#`
  commenti, chiavi mancanti = default compilati, ignote = warning e skip;
  `VAT_HORDE_BALANCE` cambia il path, le env `VAT_HORDE_*` vincono sul
  file). Riletta a OGNI `build_world` → si tara live cambiando livello o
  col rebuild, senza ricompilare. Embedda `DefTuning` (nuova, in
  defense.h: tabella nemici, DoT fuoco/acido, assedio strutture,
  sangue→paura, mix del director — default via `def_tuning_defaults`,
  installati da `def_create`, override con `*def_tuning(g)=…`; i test
  storici non cambiano di un bit) + manopole host: torrette per kind
  (range/fp/danno/arco/hp/costo/mag/reload_cost — UNIFICATE piazzamento e
  scena: la light di scena era 0.10s/55HP, ora `turret.light.*` 0.12/40),
  barricata/cancellata/mina (patch di `PL_CAT_GAME`, ora non-const),
  mortaio (costo/volo/gittate/cooldown/blast), biomassa (cap/resa/ripara),
  lure, contact-siege, director legacy, budget/LZ/danno da caduta.
  `test_balance` = default deterministici, parser (override/commenti/righe
  rotte), roundtrip save→load bit-identico, file assente intoccato, e il
  cfg SHIPPED deve parsare a zero errori. I rate/pool delle exit e la
  missione restano nel `.scn` (per-livello, non global balance).
- `LOOP_DESIGN.md` — **salvataggio del loop d'assalto** (DECISO 2026-07-15,
  dal playtest "assalto noioso"): diagnosi (manca il circolo TD
  minaccia↑/potere↑, cecità informativa, mortaio slot-machine) + pacchetto
  D→B→C→A→playtest→E/F. **D FATTA** (mortaio: `mortar.bio_yield` 0.1 — i
  kill da mortaio non alimentano più il farm; cratere timbra sangue-paura
  `mortar.fear` 16 / `fear_radius` 8 → il mortaio NEGA UNA STRADA ~30 s,
  strumento di rotta). **B FATTA** (2026-07-16: marker exit + streamline a
  trattini animati exit→goal via `simp_sample_flow`, solo PREP, live sul
  commit nav; verificata a occhio). **C FATTA** (2026-07-16: costruzione in
  assalto a biomassa — `mission_placement_open` aperta anche in ASSAULT,
  hook `PlWallet` in place.c (`pl_set_wallet`, NULL = budget legacy),
  wallet bio agganciato solo in APP_ASSAULT con `bio.build_markup` 1.5,
  card COSTRUISCI light+barricata nella barra d'assalto esclusive coi
  verbi; `test_place` §17). **A FATTA** (2026-07-16: ondate annunciate —
  righe `wave n exit count rate [tank pct] [obese pct]` nel `.scn`,
  mission.c a sequenza pausa-annuncio→ondata (`wave.pause`/`wave.bonus`
  in balance), `mission_call_next` = INVIO in assalto → bonus bio sui
  secondi risparmiati, banner + bordi exit lampeggianti; banco
  `wave_demo.scn`, test in `test_mission`). Restano: E screamer
  lampeggiante + click-priorità, F soldato (SOLDIER_DESIGN), entrambi
  DOPO il playtest di A.
- `OUTFIT_DESIGN.md` — pipeline texture outfit zombie a BAKE DI PROIEZIONE
  (DECISO 2026-07-10, da implementare): basta dipingere a mano nello spazio
  UV scomodo — collage fronte/retro su sagoma + `gfx/outfit_bake.py`
  (Blender headless, Emit-bake su UV vere, bersaglio = FBX rigged del bake
  VAT) → stesso design su tutti i corpi in batch; maimed = stessi design
  stessi indici + layer sangue (continuità di colore nello swap); grading
  finale a script. L'utente sta facendo l'audit UV dei file. Piano e
  decisioni nel doc.
- `EXPLOSION_DESIGN.md` — esplosioni × mondo (DECISO 2026-07-04, da
  implementare): primitiva condivisa `def_blast` (agenti+strutture+cadaveri
  con falloff unico) + fiction host (burst/incendi/scorch/decal); risposta
  per ARCHETIPO dagli assi ENTITY_DESIGN + 2 colonne catalogo (`resist`,
  `burn`); scorch direzionale v1 senza asset, convenzione nodi `glass_*`/
  `dmg_*` per il v2; chiude il buco danno-da-caduta (`simp_landed` mai
  consumato). Prerequisito di mine (fase D) e mortaio (fase F).
- `M3_DESIGN.md` — design tecnico di M3 (handle, volo, cadaveri, query, tipi,
  densità→costo): API, dettagli, piani di verifica.
- `GFX_DESIGN.md` — direzione artistica e design grafico (DECISO, giugno 2026):
  ortografico 3/4 alla WC2 (niente prospettiva), prerender realistico-caricato
  da Blender a 16 direzioni, zoom a tier discreti ×2 (32→16→8 px/m → campo di
  densità = minimappa), viewport scorrevole, livelli S/M/L fino a 512×384 m,
  gore come sistema (gib balistici + decal persistenti + cadaveri), costruzioni
  del giocatore a griglia + decoro obliquo a forma libera rasterizzato nella
  nav (la collisione è già SDF: fisicamente funziona oggi). Input per la
  scelta dello stack di rendering (SDL_GPU vs GL, ancora aperta).
- `sandbox_particles.c` — sandbox interattivo SDL3 (pennelli muro/spawner/goal/
  pack/kill e costo± su G/H con X per azzerare, RMB = esplosione che sveglia
  anche i dormienti e lancia in aria (manopola `up_ratio` su 7/U), W = sveglia
  tutti, M = tipo di spawn (walker/runner/tank/mix per spawner e pack), K/L =
  `k_density`, A/S = `k_jam`, O = overlay ciclico (off → densità viola →
  jam rosso), manopole live, pausa/step, overlay flow field). Gli spawner sono stato del sandbox, non del core; il pennello PACK
  piazza dormienti one-shot (ridipingere riempie i buchi, idempotente); il
  pennello KILL uccide sotto il pennello e il ~30% lascia un cadavere
  (barricate emergenti). Scene: `./sandbox assets/scenes/file.txt` carica una
  configurazione di partenza (deve essere 160×120), F2 salva lo stato
  dipinto sul file caricato (o `scene_saved.txt`), R = reset alla scena.
  Controlli nell'header del file. Viewport scorrevole con tier di zoom
  (Z = 12/16/32 px/m ancorato al cursore, frecce = pan): a 16/32 disegna
  il layer sprite con la sheet dello zombie (B per tornare ai quad), a 12
  i quad di sempre. F3 = screenshot `sandbox_shot.bmp`; env
  `SANDBOX_SHOT="frame[,tier,camx,camy]"` = N frame, screenshot, esci —
  per verifiche visive headless (così Claude può guardarsi i frame da solo).
- `gfx/sprite_render.py` / `gfx/sheet_pack.py` — pipeline prerender sprite
  (GFX_DESIGN.md §4): il primo gira DENTRO Blender headless (`blender
  --background --python gfx/sprite_render.py -- [opzioni]`), costruisce il
  rig unico (camera orto 45°, chiave NW, rim) e renderizza 16 direzioni ×
  N frame × azioni in PNG supersampled (128 px/m) + `meta.json` (anchor a
  terra, k_z); il secondo (python3+PIL) impacchetta le sheet per azione
  (righe=direzioni, colonne=frame) e riscala al tier (64 px = tattico).
  Senza `--fbx` usa un pawn placeholder animato (valida la catena); con
  `--fbx` + `--anim nome=file.fbx[:frames]` importa personaggio e clip
  Mixamo (height auto-normalizzata; il root motion delle clip NON in place
  viene cancellato per frame misurando la XY world delle hips — verificato
  con FBX sintetico a deriva 2 m: personaggio centrato in ogni frame). Convenzione: riga k = heading
  k·22.5° ORARIO da "verso camera". Blender locale: portatile 5.1 in
  `~/Scaricati/blender-5.1.0-linux-x64/` (non nel PATH). Output in
  `gfx/out/` (gitignored). Oltre al PNG, ogni sheet esce anche come
  `.zspr` (header binario ZSP3 + RGBA raw, formato in testa a
  sheet_pack.py): è quello che carica il sandbox, SDL3 non decodifica
  PNG. L'header porta anche `stride_m` (falcata misurata sul ciclo
  INTERO f0..f1 — il walk avanza con la distanza) e `duration_s` (durata
  nativa — le clip a tempo tipo idle girano a velocità propria).
  RENDER INCREMENTALI: `meta.json` viene FUSO, non sovrascritto —
  aggiungere una clip = renderizzare solo quella nello stesso `--out` e
  rilanciare sheet_pack (rigenera tutte le sheet dai PNG su disco).
  `--gpu` (OptiX/CUDA, fallback CPU) e `--max-tex N` (downscale texture
  sorgente: la 4K del pack è sprecata su sprite da 35 px e costa RAM).
  `gfx/render_remote.sh` = batch sulla workstation via ssh (rsync,
  render --gpu, pull, pack locale).
- `sprite_view.c` — previewer standalone delle sheet `.zspr` (riproduce
  l'animazione al `duration_s` nativo: la filmstrip PNG statica non mostra
  la temporizzazione, serve per giudicare es. "l'idle è viva?"). Niente
  dipendenza dal core: parsa l'header ZSP3 e blitta i frame come
  `sprite_layer.c`. `./sprite_view a.zspr [b.zspr ...]` (Tab cicla le
  sheet); controlli nell'header (space play/pausa, `,`/`.` step frame,
  ←/→ direzione, `-`/`=` velocità, B sfondo, G anchor+terra, A montaggio
  16 direzioni, R reset). Headless: `SPRITE_VIEW_SHOT="[dir]" ./sprite_view
  s.zspr` dumpa tutti i frame di una direzione in fila in
  `sprite_view_shot.bmp` (così Claude può ispezionare l'animazione da
  solo — converti il BMP in PNG con PIL per guardarlo). `make sprite_view`.
- `sprite_layer.h` / `.c` — layer sprite SDL3 sopra il core (validazione
  del §4 di GFX_DESIGN prima dello stack GPU): heading per slot = EMA
  della velocità (τ 0.25 s) quantizzato sulle direzioni della sheet;
  fino a 8 VARIANTI di walk (sheet multiple, assegnate per hash
  dell'handle: rompe l'uniformità del passo), ognuna con la sua falcata
  dal `.zspr`; frame avanzato con la DISTANZA percorsa (fase in cicli,
  `v_jitter` desincronizza gratis); fase/heading/variante/tinta seedati
  dall'handle (slot riusato = stato pulito); y-sort su y di terra;
  anchor della sheet inchiodato a (x,y) dell'agente, scala = raggio/0.30
  (il tank è uno zombie grosso); tinta da palette curata di 8 tinte
  zombie + jitter di luminosità (il random RGB libero faceva zombie
  viola), dormienti dimezzati; volo = offset `z·k_z·ppm` + ombra
  ellittica a terra; stato STUCK (EMA velocità con isteresi 0.15/0.35
  m/s) e dormienti → sheet idle opzionale a playback nativo
  (`sprite_layer_set_stuck`) invece del freeze a metà passo. Stato
  per-slot tutto nel layer, il core non sa nulla (§7). Problemi noti
  annotati in TODO M6: idle Mixamo quasi statica (effetto copia-incolla
  nei gruppi fermi), heading che piroetta dentro gli ingorghi (serve
  freeze in stuck + rate limit), stuck contro muro dovrebbe attaccare.
- `Makefile` — `make test` (entrambi, no deps) · `make sandbox` (SDL3) ·
  `make clean`. SDL3 compilato dai sorgenti sta in `~/.local`: il Makefile
  imposta `PKG_CONFIG_PATH` da solo, e `sdl3.pc` porta già l'rpath giusto.
- `sim.h` / `sim.c` / `test_dump.c` / `sandbox_sdl3.c` — core continuo e relativo
  sandbox (progetto precedente, nella sua cartella).

## Convenzioni e vincoli

- SoA rigoroso nei loop caldi; nessuna allocazione dentro `simp_step`.
- Determinismo: RNG xorshift interni (sim-level + per-agente), niente `rand()`
  nel core. Stesso ordine di chiamate ⇒ stesso risultato.
- **Indici agente NON stabili** (rimozione swap-and-pop E riordino periodico
  per località di cache, M4: `reorder_agents` permuta gli indici ~ogni 60 step
  anche SENZA kill). Per riferimenti persistenti (targeting torrette, dati
  per-agente HP/tipo) usare gli handle / indicizzare per SLOT, mai per indice
  denso tra uno step e l'altro. Manopola `SIMP_REORDER` (0 = off).
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

# FX_LAB — laboratorio effetti visivi

> **STATO (2026-06-24):** step 0 (scheletro) FATTO e verificato headless;
> gli FX gore (hit/maim/gib/morte/decal/sagoma) sono già CABLATI sui pool
> condivisi di `vat_layer` e rendono nel lab (steps 1-3 da rifinire a occhio,
> uno alla volta). `vat/fxlab.c`, `make fxlab`. Questo doc fissa scopo,
> principi e ordine degli step così non si perde il filo.

## Build / uso (implementato)

- `make fxlab` → `./fxlab [terreno.glb]` (default `blend/road_test.glb`). Riusa
  `vat_layer.c`, gli shader `vat/*`, terrain.c, i pool gib/decal/sagoma. NON è
  nei target di default.
- DRIVER: un solo agente del core (`sim_particles`) che fa la SPOLA lungo un
  goal (locomozione reale del gioco → heading/FSM/fase veri in `vat_layer`),
  niente director/orda. Il `road_test.glb` è caricato come suolo texturizzato +
  heightmap `.zhm` a fianco (`gfx/terrain_bake.py`). Footprint del glb a coord
  negative → traslato in coord ≥0 per la sim, la quota si campiona riaggiungendo
  l'origine (`OFX/OFY`).
- TERRENO/QUOTA: ombre, decal di sangue, sagome-cadavere, gib e l'agente sono
  TUTTI posati su `terrain_z(x,y)` → un cadavere su pendenza siede alla quota
  giusta GIÀ ORA. `road_test` è quasi piatto (z 0–0.16): per stressare le quote
  serve un glb con dislivelli veri. L'accumulo cross-quota (mound che copre celle
  a quote diverse) resta da affrontare nello step 5.
- UI: pannello **nuklear** (single-header C puro `vat/nuklear.h`, backend SDL3+GL3
  `nkgl_*` in `fxlab.c`, adattato dal demo ufficiale) — combo **body** (9 modelli),
  property **outfit** base (0–15: 0–13 normali, **14=carbonizzato, 15=acido**) +
  checkbox **insanguinato** (+16), combo **Animazione** (clip walk/run/idle del
  body corrente, "auto" = FSM velocità→stato), bottoni effetti
  (**Hit+sangue**/Maim/**Maim braccio**/Schizzo/Sangue/Insanguina/Morte/Esplodi/
  Respawn), pausa, stato. **Hit+sangue** = anim hit + burst di sangue particellare
  + outfit insanguinato, in un colpo (stesso per il tasto `H`). **Maim braccio**
  (tasto `N`) = body→`zombie_maimed_arm` + sangue + braccio reciso e 2 frammenti
  (mesh-gib 3D, sotto) che volano via + outfit insanguinato. Il mouse sul
  pannello NON muove la camera (`nk_window_is_any_hovered`/`item_is_any_active`).
  **Pannello AUTOREVOLE**: body+outfit+clip scelti sono applicati all'agente vivo
  OGNI frame → lo schermo combacia sempre col pannello (anche al primo spawn e dopo
  i respawn; prima il primo spawn aveva body casuale, fuori sync). Setter aggiunti
  a `vat_layer`: `set_outfit` (make_bloody è a senso unico) e `force_clip` (override
  clip locomozione, -1 = auto/FSM; reset al cambio body / riuso slot).
- **LACUNA ASSET (texture)**: le righe insanguinate dell'atlante outfit (celle
  16–31, = outfit+16) sono dipinte solo per **zombie_man** e **zombie_man_obese**.
  Per **zombie_fem, zombie_fem_obese, zombie_child, zombie_fem_skirt** le celle
  16–31 sono **placeholder verde** (non dipinte) → selezionare "insanguinato" su
  quei body mostra il verde. Il lab è corretto (mostra ciò che c'è nell'asset):
  vanno dipinte le righe sangue di quelle 4 diffuse. Atlante = 4 col × 8 righe
  (`vat.vs` ATLAS_NX/NY), righe 0–3 = outfit 0–15, righe 4–7 = +16 insanguinati.
- CONTROLLI tastiera (oltre all'UI): `[`/`]` cambia body · `O` insanguina · `H`
  colpo+sangue+insanguina · `M` mutila→crawler · `G` schizzo (gib chunky) · `J`
  sangue particellare · `K` morte (cadavere+decal) · `B` morte esplosiva · `R`
  sangue particellare · `K` morte (cadavere+decal) · `B` morte esplosiva · `N`
  maim braccio · `R` respawn · `SPACE` pausa · `C` cam libera · `T` texture ·
  mouse/frecce camera · `ESC`.
- MESH-GIB (gore con mesh 3D vere, FX_LAB step maim): pool balistico condiviso in
  `vat_layer` (`vat_layer_maim_arm`/`vat_layer_fill_mesh_gibs`) — pos/vel/ttl +
  tumble a 3 assi + `mesh_id`; il modulo tiene SOLO la fisica, il tool mappa id→
  VAO e disegna (principio 1). Le 3 mesh stanno in `blend/gibs.glb` (nodi `arm`/
  `gib1`/`gib2` = mesh_id 0/1/2), caricate da `fxlab` (`load_gib_meshes`, centrate
  sul centroide), texture = diffuse di `zombie_man`, shader `vat/mesh.*`. Scala
  unità→metri `GIB_UNIT=0.01` (braccio ~0.53 m), tunabile in `fxlab.c`. Le mesh
  sono BOZZE: l'arte si itera, la pipeline è pronta.
- PARTICLE SYSTEM (`fx_particles.h/.c`, modulo zero-dep come i core, FX_LAB
  step 2): sim renderer-agnostic di particelle billboard (gravità/drag/vento,
  colore+scala start→end, blend alpha/additivo, emitter burst/cono/continui
  data-driven `FxEmitterDef`); il disegno GL (shader `vat/particle.*`, due
  passate) sta in `fxlab` come i pool gib/decal. RNG xorshift deterministico,
  niente `rand()`, niente malloc nello step. Callback quota terreno → le gocce
  si fermano su `ter_z`. Preset `BLOOD_DEF` = schizzo sangue. Migra in
  `vat_horde` accendendo un flag (principio 1). Generale per fuoco/fumo/
  esplosioni (preset futuri). Decal-a-terra al landing SCARTATO (24 giu 2026:
  basta la macchia di morte già esistente + i cadaveri).
- HEADLESS: `FXLAB_SHOT="<frame>"` → N step, `fxlab_shot.bmp`, esce (UI esclusa).
  `FXLAB_CAM="cx,cz,hh,az,el"`, `FXLAB_FX=<frame>` (esplode l'agente per
  verificare i pool gore da uno screenshot), `FXLAB_UI=1` (forza l'UI anche nello
  screenshot, per verificarla), `FXLAB_CLIP=<idx>` (forza una clip sull'agente,
  -1=auto), `FXLAB_BODY=<v>` / `FXLAB_OUTFIT=<o>` (pin body / outfit, per verifiche
  headless di anim/outfit/texture).

## Scopo

Un eseguibile **separato da `vat_horde`** per studiare e testare gli effetti
visivi **uno alla volta**, **senza la simulazione fisica**: niente orda, niente
director, niente nav. Solo un agente *scriptato* su cui si innescano a comando gli
effetti (cammina, ferito, mutilato, gibbato, morto, decal, accumulo…). Quando un
effetto **soddisfa a occhio**, migra in `vat_horde` e si chiude.

Perché: iterazione veloce e deterministica (premi un tasto → ferisci/uccidi
*quel* corpo *lì*, invece di aspettare il director), e scenari **scriptabili
headless** (screenshot da env, come `VAT_HORDE_SHOT`) così anche Claude può
giudicare i frame da solo.

## Principi (NON negoziabili)

1. **Condividere il codice, non forkarlo.** Il lab fa girare gli **stessi
   moduli** del gioco — `vat_layer.c`, `vat_gl.h`, gli shader, i pool decal/gib,
   il bake atlante sagome (CORPSE_DESIGN §10.7) — solo con un **driver diverso**
   (un agente scriptato al posto della sim). "Integrare" deve voler dire *accendere
   un flag in `vat_horde`*, non riscrivere l'effetto. Il rischio numero uno di una
   sandbox separata è la divergenza: il lab bello, il gioco diverso. Evitarlo per
   costruzione.
2. **Scope = RENDER.** Niente pathfinding/Dijkstra nel lab. Il confronto peso
   nav-cadaveri vs barricata resta dove già vive: `test_jam`, `test_breakthrough`,
   `test_corpse_pile` (core, headless) + `vat_horde` (in azione). Nel lab
   l'accumulo si testa come **visivo**, guidato da un `corpse_height` **dipinto a
   mano** (niente agenti che muoiono).
3. **Hot-load degli asset.** I modelli li fa l'utente: il lab carica gli asset VAT
   che gli si puntano, con **scelta body + outfit da tastiera**, così si droppa un
   modello nuovo e si vede subito hit/maim/gib/morte/decal su quello.
4. **Disciplina "uno alla volta + graduazione".** Ogni step si chiude migrando in
   `vat_horde`. Il lab resta un banco di prova, non un secondo gioco.

## Build / convenzioni

- Target `make fxlab` (nome provvisorio), un nuovo `.c` che riusa i moduli `vat/`.
  Fuori dai target di default.
- Controlli a tasti (bozza): `H` hit · `M` maim/mutilazione · `O` cambio outfit ·
  `G` gib/schizzi · `K` morte · `F` freeze→decal · `B` bake in texture cadaveri ·
  `N` toggle normal/POM · `[`/`]` body/outfit · camera con mouse (come vat_horde).
- Headless: env stile `FXLAB_SHOT="<frame>[,...]"` → N frame, screenshot, esci.

## Ordine degli step (dipendenze)

0. **Scheletro.** Livello vuoto + un VAT che cammina + selezione body/outfit.
   Tutto il resto ci si appende sopra.
1. **Ferimento.** Animazione `hit` + cambio outfit (insanguinato) **oppure**
   mutilazione (arto monco / crawler). Riusa la FSM di `vat_layer` e gli outfit
   atlas.
2. **Gib / schizzi.** Burst gore (pool gib esistente) o particelle di sangue;
   decal-macchia a terra.
3. **Morte → cadavere → decal.** Freeze dell'ultimo frame (decedente) → a TTL la
   sagoma-cadavere (CORPSE_DESIGN §10.7, già fatta) → **bake del pool in una
   texture "cadaveri sul suolo"** world-aligned (la RTT-accumulation rinviata in
   §10.7: il lab è il posto giusto per prototiparla in sicurezza).
4. **Normal map + POM.** Esperimenti di rilievo di superficie sotto la nostra
   camera (azimuth ruotante, elevazione fissa): vedere l'effetto, decidere se vale
   per cadaveri/terreno/mound (vedi nota POM in CORPSE_DESIGN).
5. **Accumulo cadaveri (ibrido).** Heightfield bulk da `corpse_height` (griglia
   mondo fissa, stabile→niente flicker) + pochi cadaveri "hero" persistenti su
   ancore a seed fisso. Guidato da un campo dipinto a mano. Decisione di design
   in CORPSE_DESIGN §10 (cupola procedurale SCARTATA: flicker strutturale).

## Vedi anche

- `CORPSE_DESIGN.md` §10 — rendering cadaveri (singoli/decal/mucchi), §10.7
  sagoma-cadavere + nota RTT-accumulation, decisione heightfield+hero.
- `GFX_DESIGN.md` — direzione artistica (realistico finale; placeholder ora).

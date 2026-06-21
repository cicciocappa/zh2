# Editor di livelli — design tecnico

> **DA IMPLEMENTARE.** Editor visivo per autorare i livelli (terreno + entità di
> gioco) come file `.scn` vettoriali. Costruito come **modalità EDIT dentro
> `vat_horde`** (l'unico tool visivo del progetto), così si **edita e si
> playtesta nello stesso strumento**. Decisioni prese con l'utente (giugno 2026):
> (1) **un solo file**: il formato `.scn` cresce per ospitare anche le entità
> gameplay; (2) **UI pure-C** per la fase 1 (font bitmap + modi a tastiera),
> Dear ImGui/cimgui adottato solo quando la fase 2 (proprietà/script) lo chiede;
> (3) **niente editing live**: in EDIT il sim è fermo, si re-instanzia su Play.

Riferimenti: `scene.h`/`scene.c` (formato attuale), `vat_horde.c` (host),
`M5_DESIGN.md` §7-§8 (base, director), `GFX_DESIGN.md` (vista ortografica,
snap-a-griglia vs decoro libero).

---

## 1. Principio portante: la `Scene` è l'unica fonte di verità

L'editor muta **solo la struct `Scene`** (in metri, risoluzione-indipendente).
Tutto ciò che gira — `SimP` + `DefGame` (strutture/torrette) + i `DefDirector`
delle uscite — è **DERIVATO** dalla `Scene` con uno step di instantiate. Non si
edita mai il mondo vivo.

```
            EDIT                              PLAY
   ┌───────────────────────┐        ┌──────────────────────────┐
   │  muta la struct Scene │  ──►   │ scene_instantiate(Scene) │
   │  (rect, poly, exit…)  │        │ + derive gameplay        │
   │  render STATICO        │  ◄──   │ orda viva, sim steppa     │
   └───────────────────────┘  drop  └──────────────────────────┘
```

- **Toggle EDIT↔PLAY** (es. TAB): EDIT non steppa il sim, disegna la scena
  statica + overlay editabili + cursore; PLAY re-instanzia dalla `Scene` e gira
  come `vat_horde` oggi; tornando in EDIT si **butta il sim** e si tiene la
  `Scene`. L'orda si azzera a ogni Play — accettabile (l'editing non ha bisogno
  di un mondo vivo, l'utente è d'accordo).
- **Save = `scene_save`** (esiste già), esteso per le righe nuove. Load idem.

### Confine architetturale (da rispettare)

`scene.c` **parsa e immagazzina** le entità gameplay nella `Scene`, ma **non le
agisce**: resta gameplay-agnostico, conosce solo `sim_particles.h`.
`scene_instantiate` continua a fare SOLO terreno (rasterizza i poly come
wall/cost, set goal/pack/param). È il **layer di gioco** (in `vat_horde` o un
piccolo modulo `level.c`) che legge la `Scene` e costruisce `DefGame`
(strutture da `poly hp`), le torrette (`turret`) e i director (`exit`). Stesso
confine di oggi: `scene.c` non conosce `defense.c`.

> Helper nuovo necessario: `scene.c` espone la rasterizzazione di un poligono in
> celle (`scene_rasterize_poly(poly, gw, gh, cell, cb, user)`, scanline even-odd
> già scritta) così il layer di gioco sa **quali celle** appartengono a una
> struttura distruttibile da passare a `def_struct_cell`, senza duplicare il
> raster.

---

## 2. Estensioni del formato `.scn` (retro-compatibili)

Tutte parole-chiave NUOVE → i `.scn` esistenti continuano a caricare invariati.

```
# --- terreno (già esistente) ---
cell 0.5
world 100 80
set k_density 2.5
goal 48 38 4 4
spawn 2 1 96 3                 # spawn semplice (un director globale, come oggi)
pack 10 10 4 4
cost 20 30 8 8 5.0
poly 4.0 solid     30 20 40 20 40 35 30 35      # muro indistruttibile
poly 0.4 cost 6.0  60 40 70 40 65 50            # hazard a costo

# --- gameplay (NUOVO) ---
budget 1000                                      # budget di piazzamento (§8)

# struttura distruttibile = poly-muro CON hp, opz. il core (§7). Elegante:
# unifica §7 col formato ostacoli — l'editor la disegna come un poly qualsiasi
# a cui assegni hp; `core` la marca come condizione di sconfitta.
poly 3.0 solid hp 800        20 60 36 60 36 64 20 64
poly 3.0 solid hp 1200 core  76 76 84 76 84 84 76 84

# uscita scriptata = spawn rect + script del director-per-uscita (§8). Campi
# opzionali, default = flusso costante infinito. Es: cancello che a t=180s
# apre, butta 300 zombie a 5/s e poi si esaurisce.
exit 12 40 2 2  rate 5 ramp 0 delay 180 pool 300

# torretta pre-piazzata (livelli a difesa fissa). Il piazzamento a budget del
# giocatore è la "fase di setup" futura, non l'editor.
turret 50 30  range 45 arc -1 1 heavy 0 pierce 1

# --- terreno mesh + props (NUOVO, §9-§10) ---
terrain meshes/level1.glb            # glb#1: suolo CON buchi; .zhm/ZHM2 baked a fianco
statics meshes/level1_statics.glb    # glb#2: mesh visiva statici indistruttibili (palazzi/rocce)
prop bus      40 22 90              # istanza di catalogo (dinamico): tipo x y rot(gradi)
prop edicola  55 30 0
# i palazzi/rocce NON sono prop: stanno in glb#1 (buco) + glb#2 (visiva), vedi §9-§10
```

### Cosa guadagna la struct `Scene`

- `ScenePoly` += `float hp` (0 = indistruttibile, >0 = struttura) e `bool core`.
- nuovo `SceneExit { float x,y,w,h, rate, ramp, delay; int pool; }` + `n_exit`.
  (Lo `spawn` semplice resta per il caso globale; `exit` è lo spawn autorato con
  script.)
- nuovo `SceneTurret { float x,y, arc_min,arc_max, range; int heavy,piercing; }`
  + `n_turret`.
- `int budget`.

`scene_save` scrive le righe nuove SOLO se presenti (file minimali restano
minimali). `scene_load` le parsa con default sensati per i campi omessi.

---

## 3. Host: modalità EDIT in `vat_horde`

Si riusa tutto l'esistente (camera ortografica 3/4, pan, tier di zoom, render
del suolo/ostacoli flat-shaded, HUD). Si aggiunge:

- **stato editor**: `Scene` in RAM come verità, tool corrente, entità
  selezionata, flag snap, poligono in costruzione.
- **toggle EDIT/PLAY**; in EDIT il loop a passo fisso non gira.
- **picking sul piano `y=0`** (sotto): cursore → coord mondo (x,y).
- **render overlay** delle entità editabili + highlight selezione + griglia/
  cursore.
- **input mouse** (oggi `vat_horde` è solo tastiera): LMB disegna/seleziona,
  drag muove/ridimensiona, RMB annulla/cancella.

### Picking: dal pixel al mondo

La VP ortografica è già calcolata ogni frame. Unproject del cursore:
NDC `(2·mx/W−1, 1−2·my/H)` a z=−1 e z=+1, trasformati per `inverse(VP)`, danno
due punti `p0,p1`; il punto a terra è l'intersezione col piano `y=0`:
`t = −p0.y/(p1.y−p0.y)`, `world = lerp(p0,p1,t)` → `(world.x, world.z)` in metri.
Serve `m_invert(VP)` (4×4) — una funzione in più nel mini-math di `vat_horde`.

---

## 4. Modello di interazione (fase 1: terreno)

Tool selezionati a tastiera, mostrati in un HUD testuale:

| Tool          | Tasto | Azione                                                    |
|---------------|-------|-----------------------------------------------------------|
| Select/Move   | `1`/Q | click = seleziona; drag = muovi; `Del` = cancella         |
| Rect goal     | `2`   | click-drag → rect (snap cella)                            |
| Rect spawn    | `3`   | come sopra                                                 |
| Rect cost     | `4`   | come sopra; `[`/`]` regola il peso                        |
| Rect pack     | `5`   | come sopra                                                 |
| Poly muro     | `6`   | click = vertice; `Invio` chiude; `Esc` annulla; `+`/`-` h |
| Poly costo    | `7`   | come sopra + peso                                         |

- **Snap**: rect e poligoni-edificio agganciano alla cella (0.5 m); decoro libero
  con `G` che disattiva lo snap (GFX_DESIGN §6: edifici a griglia, decoro libero).
- **Selezione + proprietà**: cliccata un'entità, i suoi parametri (altezza, peso,
  hp…) si editano coi tasti, valori a HUD. Niente pannelli veri in fase 1.
- **Rebuild on-edit**: a ogni modifica del terreno si ricostruisce il mesh
  ostacoli statico (cheap) e si ridisegna; il sim NON esiste in EDIT.

### Fase 2 (gameplay): tool aggiuntivi

`base`/`core` (poly con hp + flag core), `turret` (punto + arco/range), `exit`
(rect + script `rate/ramp/delay/pool`). Qui servono **pannelli di proprietà**
veri (molti campi numerici per entità) → è il trigger per **ImGui/cimgui**.

---

## 5. UI: pure-C ora, ImGui dopo

- **Fase 1**: testo via **font bitmap** (`stb_easy_font`: emette quad ASCII
  renderizzabili col flat shader, zero texture) per HUD tool/coordinate/parametri
  + modi a tastiera. Spartano ma sufficiente per il terreno.
- **Fase 2**: i pannelli di proprietà e gli editor di script (uscite con 4+
  campi, liste di entità) rendono la UI a mano insostenibile → si adotta
  **cimgui** (binding C su Dear ImGui), che resta una dipendenza del TOOL, non
  del core/sim (precedente: `vat/` usa già glad e stb, codice di terzi). Decisione
  differita finché la fase 1 non è in piedi.

---

## 6. Fasi e verifica

### Fase 1 — Terreno
Editor di: `world`/`cell`/`set`, poligoni (place/move/delete, solid/cost +
altezza), rect (goal/spawn/cost/pack), save/load. Già questo manda in pensione
l'editing a mano del `.scn`.
**Verifica**: (a) `test_scene` esteso = **roundtrip** load→save→reload con
confronto struct (il formato nuovo incluso) + determinismo del raster (già
coperto); (b) **unit del picking**: camera + pixel noti → punto mondo atteso
(math pura, headless); (c) visiva via `SANDBOX_SHOT`/screenshot.

**STATO (giugno 2026) — fase 1 prima fetta FATTA**:
- **Picking** (`vat/edit_pick.h`): `m4_invert` + `pick_y0` (unproject a y=0,
  ortho E prospettica). `test_pick` PASS — round-trip mondo→pixel→pick su griglia
  di punti + center-pixel + raggio parallelo (verifica (b)).
- **Modalità EDIT in `vat_horde`** (`VAT_HORDE_EDIT=1` o `TAB`): la sim si ferma,
  picking del cursore (VP del frame precedente), overlay flat-shaded delle rect
  color-codate + poligono in costruzione + cursore. Tool a tastiera (1-7), snap
  cella (G), `[`/`]` altezza/peso, F2 salva. Mouse: LMB drag = rect, LMB =
  vertice poly (Invio chiude), RMB = annulla vertice / cancella entità.
- **Scene = verità + re-instantiate su Play** (§1): `build_world`/`free_world`
  fattorizzati; TAB EDIT→PLAY rifà sim+gameplay dalla Scene editata (mesh
  ostacoli ri-uploadata). Editing non-live, come da design.
- **Logica di mutazione** (`vat/editor.h`, pura): `test_editor` PASS — drag→rect,
  poly close/reject, delete-at, snap, point-in-poly, save/reload round-trip
  (verifica (a), formato fase-1 invariato).
- **Finestra ridimensionabile + F11 fullscreen**; viewport/aspect dinamici.
- **RESTA (fase 1)**: select/move/drag-resize di un'entità (oggi solo
  create/delete), HUD a font bitmap (`stb_easy_font`; ora l'HUD è nel titolo
  finestra), editing dei `set`/`world`/`cell` da UI, snap visivo della griglia.

### Fase 2 — Gameplay
`poly hp`/`core` → strutture (§7), `turret` → torrette, `exit` → director-per-
uscita (§8), `budget`. Il layer `level.c` (game-side) deriva `DefGame`/director
dalla `Scene`. UI ImGui.
**Verifica**: roundtrip esteso del formato gameplay; un livello di prova caricato
in PLAY riproduce il loop §7+§8 (assedio→crollo→reroute / difesa che regge);
playtest visivo.

### Fase 3 — Polish
Undo/redo (stack di snapshot della `Scene`, è piccola), toggle snap, **validazione**
(goal raggiungibile via flood-fill della nav, spawn non dentro i muri, core
presente se ci sono strutture), playtest in-editor (PLAY → EDIT senza ricaricare).

---

## 7. Migrazione del setup programmatico esistente

Oggi `vat_horde` costruisce base/anelli (`build_base`) e l'anello di torrette in
modo PROGRAMMATICO, gated su env (`VAT_HORDE_BASE`, `VAT_HORDE_TURRETS`,
`VAT_HORDE_RATE`…). Con l'editor questi diventano **default di comodo per il
demo**; un livello vero porta la base come righe `poly hp core` e le uscite come
`exit` nel `.scn`. Il layer `level.c` legge la `Scene`; se il `.scn` non ha
entità gameplay, il fallback programmatico/env resta per le prove rapide. Niente
da buttare, solo da affiancare.

---

## 8. Decisioni prese / aperte

**Prese:** Scene = unica verità; edit↔play in `vat_horde`; un solo file `.scn`
esteso; pure-C in fase 1, cimgui in fase 2; nessun editing live (re-instantiate
on Play); struttura distruttibile = `poly solid hp [core]`.

**Prese (sessione terreno+prop, vedi §9-§10):**
- **Terreno = mesh `.glb` texturizzata** caricata as-is a runtime via **cgltf**
  (single-header C, come glad/stb già in `vat/`); heightmap `.zhm` **baked** a
  fianco, usata SOLO per il render (quota di sprite/strutture). Zero effetto su
  sim/gameplay (la sim resta 2D planare su z=0). La grid-mesh-da-heightmap è
  scartata come primo slice (si parte direttamente dal loader glTF).
- **Prop = catalogo + istanze**: tipo autorato una volta (mesh/footprint/hp/
  flag/stati danno/fx), istanze `prop <tipo> x y rot` nel `.scn`.
- **"Peso" di un prop = costo Dijkstra** (il `weight` già esistente); il
  footprint si rasterizza `solid` o `cost W`. Nessun concetto nuovo per il peso.
- **Attrito = asse SEPARATO e opzionale** (erosione tangenziale, §10), NON
  ridondante col footprint cost.

**Aperte:**
- `exit` vs estendere `spawn` con i campi script: scelto `exit` separato per non
  appesantire lo spawn semplice, ma da riconfermare quando si scrive il loader.
- Slot torrette autorati vs piazzamento libero del giocatore in setup: l'editor
  per ora mette torrette FISSE (`turret`); il piazzamento a budget è la fase di
  setup, gameplay separato.
- `m_invert` 4×4 nel mini-math di `vat_horde`, oppure formula analitica diretta
  per la sola camera ortografica (più semplice, meno generale): da decidere
  all'implementazione del picking.
- Validazione "goal raggiungibile": il flood-fill della nav lo dà gratis
  (`phi` finito dalle celle goal) — quando agganciare l'avviso in editor.

---

## 9. Terreno: mesh `.glb` + heightmap (puro render)

Ogni livello ha un **terreno** = mesh `.glb` modellata e texturizzata in Blender,
usata **as-is a runtime**. La simulazione resta **2D planare su z=0**: il terreno
è SOLO grafica, costo sim zero. La quota serve a *posare* gli elementi che la sim
muove in piano (sprite zombie, strutture, ombre) così leggono come "salgono sul
marciapiede" o "emergono dalla scalinata della metro". (Stesso intento della
heightmap M6 in `GFX_DESIGN.md §9, ma sorgente = mesh autorata anziché procedurale.)

### Tre stadi isolati (ognuno testabile da solo)

1. **Bake** — `gfx/terrain_bake.py`, Blender headless: raycast verso il basso
   sull'AABB della mesh → griglia di quote `Z(x,y)` → **`.zhm`** binario (header
   `ZHM1`: origine in metri, px/m, W×H, Z raw float; convenzione dell'header
   `.zspr`). Risoluzione render ~**4 px/m**, NON la cella nav (è render, non
   collisione). Bake = preprocessing offline, non runtime.
2. **Loader glTF** — in `vat_horde` via **cgltf** (single-header C; unica
   dipendenza nuova, sta in `vat/` accanto a glad/stb). Carica vertici/UV/
   texture del `.glb` e lo disegna come mesh statica del suolo (sostituisce il
   quad/suolo flat di oggi). I poligoni-ostacolo (`poly`) restano estrusi sopra.
3. **Sampling** — `terrain_z(x,y)` bilineare sulla `.zhm` → quota di: anchor
   sprite (`sprite_layer`), apice del volo balistico (offset visivo, la fisica
   resta su z=0), base di strutture/torrette/ombre. **Math testabile a parte**
   (sample bilineare su `.zhm` sintetica con gradino noto).

### Gotcha (da `GFX_DESIGN.md §9`)
- Muri/torrette/ombre vanno campionati a `terrain_z` o **fluttuano**; v1 può
  fare solo suolo + sprite e rimandare il resto.
- Volo balistico calcolato su z=0 piatto: su pendenze ripide l'apice visivo si
  scolla dal suolo → **tenere i dislivelli LIEVI** (marciapiedi, scalini, rampe).
- La `.zhm` è ancorata al sistema metrico della scena (stessa origine del `world`).

### Milestone slice 1
Bake di un terrain con un gradino (marciapiede + scalinata) → caricato in
`vat_horde` via cgltf → `SANDBOX_SHOT` headless con sprite che **seguono la
quota**. Test della math di sampling separato dal render.

### Statici indistruttibili vs distruttibili: due glb + maschera buchi (DECISO, 21 giu 2026)

Tutto lo scenario **statico indistruttibile** (palazzi, rocce) si modella in
**Blender**, non nell'editor; nell'editor si aggiunge SOLO il **dinamico** (muri/
barriere distruttibili, autobus, decoro). Il problema da risolvere: passare alla
nav i footprint invalicabili **senza perdere la quota del terreno dove servirà**.

**Distinzione chiave (motiva tutta la pipeline).** Dal punto di vista dell'SDF
muri e palazzi sono **identici** (planare, dal solo `solid[]`). Ma:
- di un **palazzo/roccia** la quota *sotto* non serve mai → è permanente, nessuno
  ci cammina;
- di un **muro distruttibile** la quota sotto **serve** → quando crolla gli zombie
  transitano lì e vanno posati su quella quota.

→ **i palazzi sono BUCHI nel terreno** (quota sotto irrilevante); **i muri NO**
(stanno su terreno intatto, quota nota), e si piazzano nell'editor.

**Due glb per livello (stessa scena Blender, due collection → allineamento
automatico):**
1. **glb #1 — terreno (CON buchi)**: `terrain_bake.py` lo raycasta →
   **heightmap** `Z(x,y)` **+ maschera dei buchi**. Il raycast già distingue
   colpito/mancato (array `valid[]`/`misses`); oggi i buchi vengono backfillati,
   da estendere per **emetterli**: bump `.zhm`→**`ZHM2`** (W×H byte-mask in coda,
   loader retro-compatibile con `ZHM1`). I buchi = footprint indistruttibili →
   a `scene_instantiate`: cella **`solid` + tier `palazzo`** (alto) del costo-muro
   per-cella (`simp_set_wall_cost`, già nel core), **permanente**. Z sotto = 0/
   irrilevante (coperta dalla mesh visiva).
2. **glb #2 — mesh visiva degli statici**: solo **render + riferimento
   nell'editor** (mostra dove sono i palazzi mentre piazzi il resto). Porta la sua
   Z da Blender (**self-seated**), **non** viene raycastata.

**Elementi dell'editor — entrambe le famiglie su terreno intatto (Z nota da
glb#1):**
- **barriere SDF** (muri, autobus, recinzioni): footprint `solid`/`cost`, tier
  `barricata` (basso, vedi `SIMULAZIONE.md §I.4`), distruttibili o no. Al crollo
  `simp_set_wall(false)` → reroute, gli zombie passano e si posano sulla **Z nota**;
- **decoro puro** (cartelli, carretti, tavolini, cassonetti): niente SDF, niente
  nav, solo render + seating su `terrain_z`.

**Invariante che fa quadrare tutto:** il terreno ha Z **ignota SOLO nei buchi di
glb#1**; *tutto* ciò che si piazza nell'editor sta su suolo a **quota nota** — ed
è esattamente la proprietà che permette ai distruttibili di reinstradare
correttamente al crollo.

**Da decidere all'implementazione:** regola di downsample `.zhm`
(~4 px/m = 0.25 m) → cella nav (0.5 m): cella = muro se il centro è buco, o se
≥metà/almeno-un campione lo è (conservativo per non far sfiorare gli angoli);
allineamento origine `.zhm` (AABB min) = origine `world` della scena.
**Limite noto:** raycast mono-strato → niente passaggi SOTTO (archi/ponti):
il primo colpo ombra il suolo sotto. Va bene per palazzi/rocce solidi.

### Stato (giugno 2026)
- **Slice 1 FATTO** — formato `.zhm` + `terrain.h/.c` (`terrain_z` bilineare,
  zero deps) + `gfx/terrain_bake.py` (raycast Blender → `.zhm`, backfill bordi).
  `test_terrain` PASS; bake reale verificato end-to-end.
- **Slice 2 FATTO** — loader glTF (cgltf in `vat/cgltf_impl.c`, `-w`) +
  shader `vat/ground.vs/.fs` (texture base-color + key NW). Carica TUTTE le
  primitive di TUTTI i nodi con la matrice mondo; mapping glTF y-up → mondo
  `(x,y,-z)` coerente col bake. Texture sia esterna (uri) sia EMBEDDED
  (`stbi_load_from_memory`). Gli agenti si POSANO sulla quota (post-process
  del buffer instance, il `vat_layer` resta terrain-agnostico); ostacoli/
  torrette/strutture sollevati al `terrain_z` della base; il quad-suolo flat
  si salta col terreno glb. Campo `terrain` nel `.scn` (parse/save, env
  override `VAT_HORDE_TERRAIN`). Verificato headless: gradino 3 m con sprite/
  torrette seduti sulla superficie, texture a scacchi mappata. `.zhm` mancante
  = fallback grazioso a z=0. Scena demo `scenes/terrain.scn`
  (+ `gfx/terrain_demo_make.py`).
- **Ombre + tracer a quota FATTO** — tracer di fuoco sollevati al `terrain_z`
  degli estremi; **ombre a terra aggiunte** (non c'erano nel path VAT): disco
  unitario instanziato sotto ogni agente alla quota REALE del terreno (blob
  morbido blended, `vat/shadow.vs/.fs`), ground via `terrain_z` anche sotto chi
  vola → l'ombra resta a terra. Verificato headless su terreno e su scena piatta
  (`ter_z=0`, nessun z-fight, nessuna regressione).
- **Volo balistico height-aware FATTO** — il render del volo già abbraccia il
  terreno (`za + terrain_z(x,y)`: usare il ground CORRENTE evita galleggiamenti/
  sprofondi, scelta migliore della parabola assoluta dato che il sim resta
  planare). L'ombra del flyer resta a TERRA (`terrain_z`, non la quota di volo)
  e si RIMPICCIOLISCE con `za` → segnala l'altezza. Sorgente di volo aggiunta a
  `vat_horde` (tasto `E` = esplosione+lancio al centro camera; headless
  `VAT_HORDE_BLAST="frame,x,y[,str,up]"`). Verificato: pack lanciato a cavallo
  del gradino, agenti in aria con ombre a terra staccate e ridotte, rientro a
  quota corretto.
- **Resta**: sorgente terreno procedurale/urbana (GFX_DESIGN §9 fase 2).

---

## 10. Catalogo prop: tre assi ortogonali

> **AGGIORNATO (21 giu 2026): gli statici indistruttibili NON sono più prop
> dell'editor.** Palazzi e rocce si modellano in **Blender** (vedi §9: glb#1
> buco → footprint invalicabile tier `palazzo`; glb#2 mesh visiva). Il catalogo
> prop dell'editor si restringe al **dinamico**: barriere SDF (muri/autobus/
> recinzioni, tier `barricata`, distruttibili o no) + decoro puro (no SDF/no nav).
> Tutti i prop editor stanno su **terreno intatto a quota nota** (§9 invariante).
> Gli assi qui sotto restano validi per i prop editor; la riga "Edificio
> (palazzo)" è spostata in §9 (Blender).

Un **prop** è un oggetto di scenario dinamico (autobus, edicola, semaforo,
segmento di muro/recinzione, carretto…). Modello = **catalogo** (il *tipo*,
autorato una volta) + **istanze** nel `.scn` (`prop <tipo> x y rot`). Il catalogo
NON sta nel core: è dati del gioco/editor; `scene.c` parsa solo le istanze e
rasterizza il footprint (come per i `poly`), il layer di gioco agisce hp/attrito.

Le proprietà di un tipo si scompongono in **assi indipendenti** — niente concetto
"peso/attrito" monolitico:

| Asse | Cosa | Come (cuciture esistenti) |
|------|------|----------------------------|
| **Render** | mesh `.glb` (+ stati di danno: mesh alternative a soglie di hp; death-fx a hp 0) | macchina a stati VISIVA; la sim conosce solo hp + footprint |
| **Footprint nav** | `{ solid \| cost <w> \| none }` — il **"peso" = costo Dijkstra** | `scene_rasterize_poly` → wall / cost (già fatto) |
| **Distruttibilità** | `hp` + `attackable`: assedio **diretto** (folla che preme per passare oltre) → crollo → reroute | loop SIEGE/§7, `simp_wall_pressure` con `into_wall > 0` |
| **Attrito (OPZ.)** | erosione **passiva** dal flusso che scorre ACCANTO (anche senza sbarrare): accumula → cede → death-fx + cambio nav | componente **tangenziale** del flow (`into_wall ≤ 0`), già annotata in `SIEGE_DESIGN.md §5` come "hazard ambientale" |
| **Draggable (OPZ.)** | `mass` + `drag`: il prop viene **trascinato** dal flusso, non distrutto | terza categoria di corpo PBD (massa finita, no steering/goal); UNICA vera aggiunta al core |

### L'asse "attrito" in dettaglio (idea utente, opzionale)
Prop sottili (pali, alberi, semafori) **non sbarrano** il passaggio ma il flusso
gli scorre accanto e li **logora**: si accumula lo **shear tangenziale** (la
componente del flow parallela alla superficie, `into_wall ≤ 0`) + il volume di
passanti; superata una soglia il prop **cede**, con conseguenze emergenti:
- **death-fx** che ammazza gli zombie nei paraggi;
- **cambio nav**: il prop che cade diventa ostacolo e può **bloccare un varco**
  (footprint `none → solid/cost`), instradando di colpo l'orda altrove.

È la prima destinazione concreta della componente tangenziale che `SIEGE_DESIGN`
teneva da parte: **sensore già pronto, zero roba nuova nel core**. Dà
imprevedibilità (fronte che cambia da solo). **Non fondamentale** — implementabile
dopo il resto del catalogo, o ignorabile.

### Categorie derivate (combinazioni degli assi)
- **Edificio/roccia** (statico indistruttibile): **NON un prop editor** → Blender,
  glb#1 buco (tier `palazzo`) + glb#2 visiva (§9).
- **Barricata/muro** (distruttibile): `solid` + tier `barricata` + hp medio +
  stati di danno (mesh rovinate). Su terreno intatto → al crollo reroute + zombie
  posati sulla quota nota.
- **Light prop** (tavolino, carretto hotdog): footprint `none`, **despawn al
  contatto** col fronte (flavour, ~zero nav).
- **Draggable** (cassonetto, auto): asse draggable; footprint dinamico mentre si
  sposta (o trascurato in v1).
- **Sottile erodibile** (palo, albero): footprint `none`/piccolo + asse attrito.

### Formato catalogo (da definire all'implementazione)
File-dati per tipo (mesh path, footprint poly in coord locali, hp, flag
`attackable`/`draggable`, soglie stati danno + mesh, fx, soglia attrito,
mass/drag). L'editor lo legge per offrire la palette di prop; il `.scn` referenzia
per nome. **Aperto:** un file per tipo vs un catalogo unico; coord footprint
locali ruotate da `rot` all'instantiate.

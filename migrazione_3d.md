# Migrazione a rendering 3D con VAT — note di lavoro

Documento di passaggio (15 giugno 2026). Esperimento: rendere gli zombie in
**3D full con animazioni VAT** (Vertex Animation Textures) invece degli sprite
prerenderizzati decisi in `GFX_DESIGN.md` (§4/§8). Stato: **valutazione +
preparazione del baking**. Nessun codice scritto in zh2 ancora. Si riprende
nel pomeriggio.

> ⚠️ **LEGGI PRIMA**: la sezione [Trappola della scala](#trappola-della-scala-priorità-1)
> — è il problema che in passato ha distorto tutto il baking. Va risolto per
> primo, con calma.

---

## 0. Stato aggiornato (15 giugno, sessione 2) — BAKE VALIDATO

Pipeline di baking VAT **funzionante e verificata headless**. Fatto:

- **Modello** (sez. 5/6.1): `~/Scaricati/Scary_Zombie_Pack/Walking_lowpoly_zombie.fbx`.
  Mesh `low poly guy` = **163 vert / 322 facce** (ultra-lowpoly ✓), 25 bones,
  root `mixamorig:Hips`. **NIENTE UV** → render flat/illuminato (no diffusa).
  Una sola Action `Armature|mixamo.com|Layer0`, 47 frame @ 30fps (il walk),
  **in-place** (stride=0: niente root motion → la posizione la guida il core,
  la FASE animazione va da TEMPO non da distanza). idle/attack/death = clip
  Mixamo da aggiungere dopo.
- **Trappola scala** (sez. 3) RISOLTA, ma **NON** via apply-transforms (le
  F-curve Mixamo non si riscalano: dava Y fino a −1350). Approccio robusto:
  bake in **coordinate WORLD** (la mesh valutata porta tutte le transform + il
  deform) e **normalizzazione in POST** sui numeri (altezza→1.8 m, piedi a y=0,
  centro XZ frame0 all'origine). Verifica: pos range X[-0.84,0.54] Y[0,1.80]
  Z[-0.76,0.89] — metri plausibili (Z largo = braccia protese da zombie).
- **Exporter patchato per Blender 5.1**: nuovo `vat/bake_vat.py` (headless,
  standalone — sostituisce il vecchio `tools/exporter.py` di crowd_glfw che usa
  `calc_normals_split()` rimossa in 4.1+). Lancio:
  `blender --background --python vat/bake_vat.py -- <fbx> <out_prefix> [action]`.
- **Formato asset definito DA NOI a entrambi i capi** (niente EXR/convert in due
  passi; risolve il mismatch baker↔loader della sez. 4.4):
  - `<prefix>_mesh.bin`: `int nv, int ni; nv×(3f: vertID,0,0); nv×(2f uv=0);
    ni×(ushort index)`.
  - `<prefix>_pos.raw` / `_norm.raw`: RGBA f32, `texW × (rowsPerFrame·numFrames)`.
    Per lo zombie: texW=256, rowsPerFrame=1, 163 vert/frame.
  - `<prefix>_meta.txt`: `texW,texH,rowsPerFrame,numFrames,fps,duration_s,
    stride_m,height_m`.
  - Assi GL: `to_gl(v)=(v.x, v.z, -v.y)` (z-up Blender → y-up GL).
  Output corrente in `vat/assets/zombie_*` (gitignorabile).
- **Verifica visiva**: proiezione ortografica PIL dei frame (front XY + side ZY)
  → umanoide ingobbito che cammina, animazione coerente. Bake geometricamente sano.

### Renderer GL in zh2 — FATTO (viewer standalone)

Step 6 completato come **previewer standalone** `vat/vat_view.c` (analogo 3D di
`sprite_view.c`), validato visivamente headless (fila di zombie nelle fasi del
ciclo, smooth-shaded con luce NW — `VAT_VIEW_SHOT=K ./vat_view`):

- **Context GL via SDL3** (`SDL_GL_CreateContext` + glad 0.1.36 GL3.3 core,
  `gladLoadGLLoader(SDL_GL_GetProcAddress)`). glad vendorizzato in `vat/glad*`
  + `vat/KHR/`. Build: `make vat_view` (glad.c compilato con `-w`).
- **Instance attribute PULITO** (niente hack mat4): 3×vec4 per istanza =
  `(posX,posY,posZ, heading)` · `(scala, frameA, frameB, mix)` ·
  `(outfit, tintR,G,B)`. `outfit` + tinta già previsti per gli atlanti
  multi-outfit futuri.
- **Shader** `vat/vat.vs`/`.fs`: VAT pos+norm interpolate fra 2 frame, rotazione
  per heading (attorno a Y), **luce NW finita** (chiave direzionale + ambient,
  normali bakate). Niente texture diffusa finché manca la UV.
- **Camere**: fissa orto 3/4 (GFX_DESIGN §1) + libera orbitale (tasto C).
  Math matrici inline nel .c (niente cglm, coerente zero-deps).
- **Fase animazione**: da TEMPO (clip walk in-place, stride=0). Quando
  ri-esporti il walk Mixamo CON root motion, `bake_vat.py` misura `stride_m`
  e si passa a fase-da-distanza (come `sprite_layer.c`).

### Decisioni di design (sessione 2)

- **Shading smooth, niente split normali** per gli zombie (lo split serviva sul
  soldato hard-edge). Normali per-vertice.
- **Export Mixamo**: clip di spostamento (walk/run) da esportare CON root motion
  → il baker toglie l'XZ del root (render in-place) e MISURA `stride_m`. Meglio
  che impostare lo stride a occhio.
- **Outfit via atlante**: a regime una texture con più outfit, `outfit_index`
  per-istanza → lo shader offsetta le UV nella sotto-regione. Già nel layout
  instance. Quando arriva il modello texturizzato: il baker leggerà la uv-map e
  servirà lo **split per-UV** alle cuciture (non implementato ora, niente UV).
  Previsto anche un umanoide femminile (stesso percorso).
- **Modello attuale non ottimizzato ai joint**: ok tenerlo — la distorsione
  innaturale degli arti è in-character per uno zombie. Sostituibile dopo senza
  toccare la pipeline.

### Multi-clip + UV + texture — FATTO (sessione 3)

Modello UV-mapped + texture `albedo.png` (1024²) + 13 clip Mixamo bakate:
6 walk, 2 run, 2 idle, 2 attack, 1 scream (richiamo screamer). Stato:

- **`bake_vat.py` esteso**: UV-aware con **split per-UV** alle cuciture (mesh
  163→420 vert split), **scala condivisa** fra le clip (calcolata sulla 1ª clip,
  riusata via meta → lo zombie non cambia taglia), **multi-clip incrementale**
  (`--append`: accoda i frame alle stesse texture pos/norm + merge meta, senza
  rifare le altre). Driver: bake walk senza flag, le altre con `--append`.
- **Asset stacked**: `zombie_pos/norm.raw` 256×2018 (rowsPerFrame=2), `_mesh.bin`
  con UV reali, `_meta.txt` multi-clip (`clip=<nome> startFrame numFrames
  duration_s stride_m`), `zombie_diffuse.png`. Scala 0.01071.
- **Stride MISURATO non-zero** per walk/run (0.8–2.8 m) nonostante l'export "in
  place": le clip hanno moto in avanti reale, il baker lo toglie (render
  in-place) e lo misura → fase-da-distanza possibile per walk/run. idle/attack/
  scream stride=0 (statiche, fase da tempo).
- **Renderer aggiornato** (`vat.fs` campiona la diffusa con UV; `vat_view.c`
  parsa il meta multi-clip, carica la texture, seleziona/cicla le clip N/P,
  toggle texture T). **V-flip delle UV** sul load (`stbi_set_flip_vertically_on_load`,
  Blender ha l'origine UV in basso-sx). stb_image vendorizzato in `vat/`.
- Verificato headless: `VAT_VIEW_SHOT=<clip> ./vat_view` → walk/run/attack/idle
  texturizzati e corretti.

**Trappola risolta**: nested `strtok` nel parser meta (non rientrante) →
parsing manuale riga-per-riga + `sscanf`.

### FSM + crossfade a 2 tap — FATTO (sessione 3)

In `vat_view.c` (logica CPU per-agente; nel gioco vivrà nel render layer, il core
non la tocca):
- **Stati** `IDLE/WALK/RUN/ATTACK/SCREAM`; gruppi costruiti per prefisso del nome
  clip (walk=6, run=2, idle=2, attack=2, scream=1). Variante scelta per
  `hash(id,stato)` (rompe l'uniformità). ATTACK/SCREAM sono **one-shot**: blendano
  in, riproducono una volta, blendano di nuovo allo stato precedente.
- **Crossfade a 2 tap** (`BLEND_DUR` 0.25s): durante la transizione
  `frameA`=frame clip uscente, `frameB`=frame clip entrante, `mix`=blendF 0→1 —
  niente sample extra, niente shader nuovo. `agent_transition/update/frames`.
- **Trigger** da tastiera (modalità #1): `1..5` → tutta la folla blenda allo stato.
  `N/P` = preview libera di una clip (no FSM). Fase da TEMPO (nel viewer non c'è
  movimento; col core diventerà fase-da-distanza per walk/run via `stride_m`).
- **Verifica**: `VAT_VIEW_SHOT="blend:idle:attack"` → fila del morph congelato A→B
  (interpolazione liscia confermata); griglia FSM gira senza crash.

### Outfit atlas 4×4 — FATTO (sessione 3)

Varietà di aspetto via atlante: la texture diffusa è una **griglia 4×4 = 16 outfit**,
l'`outfit_index` per-istanza (già nel layout, `iOutfitTint.x`) sceglie la cella.
- **Shader** (`vat.vs`): `vUV = (vec2(u, 1-v) + vec2(col,row)) / 4`, con
  `col=outfit%4`, `row=outfit/4`. **Convenzione: outfit 0 = cella ALTO-SINISTRA,
  ordine di lettura** (riga0=[0..3], riga1=[4..7]...). Il V-flip ora è nello
  shader (`1-v`), NON più al load (tolto `stbi_set_flip_vertically_on_load`).
  `ATLAS_N=4` costante nello shader.
- **Renderer**: `inst_push` porta l'outfit; nel viewer seedato `hash(id)%16`
  (stabile per-zombie, come tinta/variante). Nel gioco: per slot.
- **Verifica**: atlante 4×4 sintetico (outfit attuale ×16 con hue diversi, in
  `vat/assets/zombie_diffuse.png`; guida-layout in `/tmp/atlas_ref.png`) → la fila
  walk mostra 6 outfit distinti, texture allineata al corpo. L'originale
  single-outfit resta in `~/albedo.png` (NON più valido per lo shader atlante:
  campionerebbe solo la cella 0).
- **Trappole**: bleed bilineare ai bordi cella (lasciare un **gutter** nel disegno
  dell'atlante reale, o UV del modello che non toccano 0/1); outfit stabile per
  handle/slot.

### Aggancio al core sim_particles — FATTO (sessione 3)

L'orda reale del core resa in 3D VAT. Tre file nuovi:
- **`vat/vat_layer.h/.c`** — render layer (analogo 3D di `sprite_layer.c`), stato
  per-SLOT, il core non sa nulla: heading = EMA velocità (congelato se quasi fermo,
  anti-piroetta); FSM `IDLE/WALK/RUN` da |v| con isteresi → clip di stato scelte
  per `hash(slot)` (walk×6 ecc.); transizioni = crossfade a 2 tap; fase walk/run
  dalla DISTANZA/`stride_m`, idle dal tempo; outfit+tinta per-slot seedati
  dall'handle (slot riusato = pulito). `vat_layer_update` + `vat_layer_fill`
  (produce il buffer instance). ATTACK/SCREAM restano per eventi di gioco (API
  futura). Parsa il meta multi-clip da sé.
- **`vat/vat_gl.h`** — helper GL condivisi (mat4, shader, texture, BMP), static.
- **`vat/vat_horde.c`** — eseguibile `make vat_horde`: scena chokepoint (muro con
  varchi + goal in cima + spawn burst-free dal basso, ~80% walker 20% runner),
  `simp_step` + `vat_layer_update` per frame, camera orto 3/4 (pan frecce, zoom
  ±, `C` libera, `T` texture). Headless: `VAT_HORDE_SHOT=<frames> ./vat_horde`
  (+ `VAT_HORDE_CAM="cx,cz,hh,az,el"`).
- **Verificato**: ~3500 zombie multi-outfit guidati dal core marciano col heading
  corretto (spalle alla camera = verso il goal) in posa di walk, instancing a
  scala. `HEADING_OFFSET=0` allinea il forward del modello (Mixamo +Y) a +v —
  verificato in vista laterale (faccia/falcata nel verso del moto; π = moonwalk).

### Baking semplificato (rig + anim condivise) + jitter — FATTO (sessione 4)

- **`bake_vat.py` accetta `--model <rig.fbx> --anim <clip.fbx>`**: importa il
  modello riggato, importa la clip (anche SENZA skin), applica la sua Action
  all'armatura del modello (stesso scheletro `mixamorig`, 25 bone) e bake.
  `bind_action()` gestisce le **slotted actions di Blender 4.4+/5.1**
  (`action_slot`). Verificato byte-identico al bake diretto. → si scaricano le
  animazioni UNA volta (model-independent): 5 modelli + 13 anim = 18 FBX invece
  di 65. Legacy `<fbx> <prefix> <clip>` ancora supportato.
- **`vat/bake_zombie.sh <model.fbx> <out_prefix> [anim_dir]`**: bake completo
  (13 clip incrementali) di un modello col workflow sopra.
- **Jitter d'altezza** per-slot in `vat_layer` (`hmul` 0.90–1.12, seedato
  dall'handle, cosmetico) → varietà d'altezza gratis dentro un tipo di corpo.
  Nota: scala UNIFORME (anche larghezza), la collisione resta sul raggio del core.

### Multi-modello (uomo/donna/obeso/bambino) — FATTO

Tipi di corpo = MESH diverse (un bambino non è un adulto scalato): ogni tipo è un
asset VAT a sé (`zombie_man`, `zombie_woman`, `zombie_child`...), bakato con
`bake_zombie.sh` riusando le STESSE 13 anim. Render: una `glDrawElementsInstanced`
PER tipo (mesh/texture/VAT diversi → non condivisibili in una sola call); il
`vat_layer` assegna il tipo per-slot (seed o da `simp_spawn_desc`) e raggruppa le
istanze per tipo. ~5 draw call totali. L'altezza dentro un tipo resta il jitter.

**Implementato** (`vat_layer` + `vat_horde`): `vat_layer_create_multi(metas, N,
max)` carica N meta; il body di ogni agente è assegnato per-slot via
`hash(handle)%N` (cosmetico, stabile per la vita — come tinta/outfit). Chiave che
semplifica tutto: gli **indici di clip sono identici fra varianti** (stesso ORDER
di `bake_zombie.sh`), cambiano solo frame-range/stride/scale/texW per mesh → un
solo `clipA[slot]` vale per ogni meta, basta indicizzare il meta del body
dell'agente quando si avanza la fase (stride del bambino ≠ adulto).
`vat_layer_fill_variant(vl,s,v,buf,max)` emette i SOLI agenti del body `v`; il
renderer fa un draw per variante con la sua mesh/VAT-texture/diffuse (instance VBO
condiviso, ri-riempito prima di ogni draw). Verificato: 6 body (man, man_obese,
fem, fem_obese, child, fem_skirt), ~2200 agenti, 6 draw call, taglie distinte a
video. La skirt senza diffuse rende flat-shaded (texture placeholder, OK). Per
legare il body al tipo di gioco (obeso=tank) basta sostituire il seed hash con
`simp_spawn_desc` → mappa tipo, niente altro da cambiare.

### Prossimi passi

1. ~~Multi-modello (sopra) quando i 5 modelli sono pronti.~~ FATTO.
2. Stress a 10–20k (alzare MAXA; misurare ms/frame), z-sort/depth già attivo.
2. Eventi di gioco nella FSM: ATTACK sul sensore d'assedio (`SIEGE_DESIGN`),
   SCREAM per gli screamer; volo (SIMP_FLYING) già mappato su y=altitudine.
3. A/B sprite-vs-3D nel sandbox (GFX_DESIGN §8).
4. Fattorizzare un `vat_render` condiviso fra `vat_view` e `vat_horde` (oggi il
   boilerplate GL è in `vat_gl.h` ma il setup VAO/draw è duplicato).
5. Transizione a 4 tap solo se servisse l'interp intra-clip durante il blend.

---

## 1. Decisione e parametri

- **Cosa**: zombie come mesh 3D animata via VAT, sotto la stessa proiezione
  ortografica 3/4 di `GFX_DESIGN.md` §1 (niente prospettiva).
- **Perché ha senso ora** (rivaluta §8 del GFX_DESIGN con parametri nuovi):
  - Target ridotto a **10–20k** agenti (non 50–100k). Questo **elimina
    l'obiezione architetturale principale** del GFX_DESIGN §8 (la doppia
    pipeline sprite+impostor a 90k): a 20k × 322 tris = 6,4M tris/frame puoi
    renderizzare VAT su **tutti** i tier di zoom, niente impostor 2D.
  - Modello **ultra-lowpoly: 162 vertici, 322 facce**. La VAT su così pochi
    vertici è quasi gratis in memoria (~100–200 KB per tutte le clip).
  - Si guadagna: **rotazione continua** (niente quantizzazione a 16 direzioni),
    luci dinamiche, archi di volo reali, varietà a costo VRAM ~zero.
- **Costo**: forza la decisione sullo stack GPU (era rimandata in §8). Il
  sistema esistente (sotto) è **GL puro**, quindi di fatto sceglie OpenGL.
- **La scommessa residua è ESTETICA**, non tecnica: lowpoly illuminato a
  runtime ≠ prerender alla Diablo 2. Si scioglie solo con un prototipo a
  schermo (A/B sprite-vs-3D).

---

## 2. Asset di partenza riusabile: `~/Documenti/crowd_glfw`

**Decisione: riusare come base, NON ripartire da zero.** È un sistema VAT
completo e funzionante (esiste il binario `game`). Era un progetto a sé
("crowd game"/"Principessa Flo").

### 2a. Cosa è riusabile quasi tale e quale

**Renderer** — `src/soldier.c` + `shaders/vat.vs` / `vat.fs` (OpenGL 3.3 core):
- VAT corretta: **ID vertice in `aPos.x`**, posizioni per-frame in texture
  `RGB32F` con filtro `NEAREST`, **interpolazione fra 2 frame** nel vertex
  shader (`fA`/`fB`/`mix`) → animazione fluida.
- **Instancing**: una sola `glDrawElementsInstanced` per tutta la folla;
  instance buffer `GL_DYNAMIC_DRAW` riempito ogni frame, upload in un colpo
  con `glBufferSubData`. Cap `MAX_SOLDIERS` = 10000 (alzare a 20000).
- **Hack da rifare**: i dati VAT (`fA/fB/mix`) sono infilati nei buchi
  `[*][3]` di una `mat4` per istanza (vedi `soldier.c` righe ~161-167 e
  `vat.vs` righe ~31-40). Funziona ma vincola a una mat4 → in zh2 sostituire
  con un **instance attribute pulito** (a noi bastano x, z, heading, scala +
  clip-index/tinta, non una mat4 intera).
- **Luce non cablata**: `vat.fs` hardcoda `FragNormal = (0,1,0)`. Le normali
  però **sono già bakate** (`texNorm` caricata) → finire lo shader con la
  chiave NW di GFX_DESIGN §4 è frutto basso.

**Pipeline di baking** (la parte più matura e costosa da rifare):
- `tools/exporter.py` — "VAT EXPORTER V23". Gira **dentro Blender**. Bakea
  posizioni **+ normali split** (anti-artefatti shading), fa **root motion
  extraction**. Campiona la mesh **deformata** per frame (`obj.evaluated_get`)
  → funziona con qualunque mesh skeletal: lo scheletro viene "cotto" nella VAT.
- `tools/convert_assets.py` — converte l'output web (JSON/EXR) → binario C.
- `tools/mixamo_import.py` — importa FBX Mixamo, prepara per export GLB.

### 2b. Cosa si BUTTA (è un gioco a sé, non ci serve)

`soldier_update_all` (movimento + collisione terreno: è un **random-walk
CPU**, lo guida il nostro core!), `src/states/`, `src/ui/` (nuklear),
`audio`, `terrain`, `src/skeletal/`, `main.c`/GLFW.

### 2c. Cosa va ADATTATO per innestarlo in zh2

1. **Coordinate**: loro = mondo 3D y-up. Noi = 2D metrico top-down. Mappa
   `simp_px → x`, `simp_py → z`, `y = 0` (+ offset z per il volo M3.2).
   Camera **ortografica 3/4** (GFX_DESIGN §1), non prospettica.
2. **Dati di guida**: le posizioni vengono dagli SoA del core
   (`simp_px/py/...`). Il renderer diventa puro: *leggi SoA → riempi instance
   buffer*. **Heading** e **fase animazione** li abbiamo GIÀ in
   `sprite_layer.c` (EMA velocità τ 0.25 s; fase dalla distanza percorsa) →
   logica da portare, non da inventare.
3. **Context GL via SDL3**: `SDL_GL_CreateContext` + `glad`, **via GLFW**.
   Tenere `glad` + `cglm` + `stb_image`. Il **core resta zero-dipendenze e
   intoccato**: il renderer 3D è un modulo separato e swappabile (coerente
   con l'architettura renderer-agnostica di CLAUDE.md).
4. Alzare `MAX_SOLDIERS` a 20000.

---

## 3. Trappola della scala (PRIORITÀ 1)

**In passato il baking è stato difficile proprio per un problema di scala che
distorceva tutto.** Causa tecnica probabile e fix:

- `exporter.py` campiona `obj.evaluated_get(depsgraph).to_mesh()` e legge
  `v.co` → sono coordinate in **spazio oggetto (locale)**, NON world. Lo
  swizzle `blender_to_webgl(v) = (v.x, v.z, -v.y)` cambia solo l'orientamento,
  **non applica la scala dell'oggetto**.
- Gli FBX Mixamo arrivano quasi sempre con **scala non unitaria** (tipico:
  import a 0.01, o armatura scalata 100× con mesh minuscola). Se le trasformate
  non sono applicate, le posizioni bakate finiscono in scala sbagliata/distorta.
- **FIX da fare PRIMA di bakare, nel .blend**:
  1. Seleziona mesh **e** armatura → `Object > Apply > All Transforms`
     (location, rotation, **scale → 1.0**).
  2. Verifica che `obj.scale == (1,1,1)` e `armature.scale == (1,1,1)`.
  3. Conferma l'altezza del personaggio in metri sensata (~1.8 m) PRIMA del
     bake (lo zombie deve avere raggio coerente col core: default r=0.30 m,
     scala sprite = raggio/0.30 — vedi `sprite_layer.c`).
  4. Se serve, normalizzare l'altezza come fa già la pipeline sprite
     (`gfx/sprite_render.py` auto-normalizza l'altezza dei Mixamo) — replicare
     quella logica nell'exporter VAT.
- **Verifica numerica**: dopo il bake, controllare il range dei valori
  nell'EXR `_pos` (devono essere metri plausibili, es. X,Z in ±0.5, Y in 0..1.8),
  non micro-valori né migliaia.

---

## 4. Altri problemi noti (da risolvere prima di lanciare)

1. **Blender 5.1 rompe `exporter.py`**: usa `mesh.calc_normals_split()`,
   **rimossa in Blender 4.1+**. Sul 5.1 locale lo script fallisce lì. Fix: in
   4.1+ le split normals sono automatiche dopo `calc_loop_triangles()`; leggere
   `loop.normal` direttamente, togliere le chiamate a `calc_normals_split()`.
   (Blender locale: `~/Scaricati/blender-5.1.0-linux-x64/blender`, non nel PATH.
   C'è anche `blender-5.0.0` come tar.)
2. **Path hardcoded**: `OUTPUT_PREFIX = "/home/france/Scaricati/.../soldier_ok"`
   in `exporter.py` riga 30 → cambiare. `ROOT_BONE_NAME = "mixamorig:Hips"`
   (riga 32) → verificare sul rig dello zombie.
3. **`ACTIONS_TO_EXPORT`** (righe 17-27): lista di nomi di Action che devono
   esistere nel .blend. Vanno importate (Mixamo: 1 FBX = 1 clip) e **rinominate**
   le Action di conseguenza (es. `walk`, `idle`, `attack`, `death`).
4. **Mismatch di formato baker ↔ loader** (da riconciliare quando ricostruiamo
   il renderer in zh2 — controlliamo entrambi i lati):
   - `convert_assets.py` produce `.bin` **con header** (w,h,channels) a **3
     canali**, `TEXTURE_WIDTH = 512`, `rowsPerFrame` **variabile** (= righe per
     frame in base ai vertici).
   - `soldier.c` invece carica `.raw` **senza header**, **4 canali (RGBA)**,
     `1024×1024`, `ROWS_PER_FRAME = 2.0` hardcoded.
   - I `.raw` in `resources/` venivano da un percorso ancora diverso ("formato
     web"). Per ora non allineati: in zh2 definiamo NOI il formato a entrambi
     i capi.
5. **Il "viewer" NON mostra il VAT**: `tools/viewer/` è un *GLB/skeletal
   importer* (cgltf), non un renderer VAT. **Verifica del bake** = (a)
   ispezionare gli EXR `_pos`/`_norm` come immagini + i conteggi in
   `_meta.json`, oppure (b) un mini-render via `soldier.c`. Non c'è un VAT
   viewer pronto.

---

## 5. Modello zombie — candidati

- `~/Scaricati/Walking_lowpoly_zombie.fbx` ← probabile 162-vert (da
  confermare: conteggio vertici, armatura, Action contenute).
- `~/Scaricati/Scary Zombie Pack/` (cartella, da esplorare).
- Clip Mixamo sciolte utili in `~/Scaricati/`: `Walking.fbx`, ecc.
- **Da fare**: ispezione headless con Blender 5.1 (import FBX → stampa
  `len(verts)`, `len(polys)`, armatura, `bpy.data.actions`). Script di
  ispezione già abbozzato; era il prossimo comando quando ci siamo fermati.

---

## 6. Piano per riprendere (in ordine)

1. **Confermare il modello**: ispezione FBX headless (vertici=162? armatura?
   quali animazioni?). Decidere il set minimo di clip (walk, idle, attack,
   death, tumble).
2. **Risolvere la scala** (sez. 3): apply transforms, altezza in metri,
   normalizzazione. Questo PRIMA di tutto il resto.
3. **Patchare `exporter.py`** per Blender 5.1 (sez. 4.1) + path/azioni.
4. **Primo bake** di una sola clip (walk) → ispezionare EXR `_pos` (range
   valori = scala corretta) + `_meta.json`.
5. **Verifica visiva** del bake (mini-render VAT, non il viewer GLB).
6. Solo DOPO: portare il renderer (`soldier.c` → modulo zh2 su SDL3+GL),
   definire il formato asset a entrambi i capi, riempire l'instance buffer
   dagli SoA del core, camera ortografica 3/4, heading/fase da `sprite_layer`.

---

## 7. File di riferimento

In `~/Documenti/crowd_glfw`:
- `src/soldier.c` / `src/soldier.h` — renderer VAT instanced (cuore riusabile).
- `shaders/vat.vs` / `shaders/vat.fs` — shader VAT (fs: luce da finire).
- `src/gfx.c` — caricamento shader + texture (`gfx_load_texture_raw` usa
  `GL_RGB32F` + `NEAREST`, corretto per VAT).
- `tools/exporter.py` — baker VAT (Blender). **Da patchare** (Blender 5.1,
  path, scala).
- `tools/convert_assets.py` — EXR/JSON → binario C.
- `tools/mixamo_import.py` — prep FBX Mixamo.
- `tools/viewer/` — viewer **GLB** (non VAT; non serve per verificare il bake).

In `~/Documenti/zh2` (questo repo):
- `sprite_layer.c` / `.h` — logica heading (EMA velocità) + fase animazione
  (distanza) da PORTARE nel renderer VAT.
- `GFX_DESIGN.md` §1 (camera orto 3/4), §8 (analisi VAT originale, da
  aggiornare con la decisione di andare full-3D a 10–20k).
- `sim_particles.c` / `.h` — core, SoA = instance buffer (`simp_px/py/...`).

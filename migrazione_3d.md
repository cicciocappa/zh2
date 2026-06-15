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

# BLENDER_LEVEL — convenzione di authoring livelli in Blender → `.scn`

Decisione (3 lug 2026): **niente editor dedicato completo** — i livelli si
autorano in Blender e uno script Python (`gfx/export_scn.py`) li esporta nel
formato `.scn` esistente. Il `.scn` resta il CONTRATTO col
runtime e non cambia; Blender è solo un *produttore* di `.scn`, come oggi lo
è la modalità EDIT di `vat_horde`. Il gioco non sa nulla di Blender.

Questo documento definisce la convenzione (nomi, custom properties, assi,
collection) e il perimetro dello script di export. Guida pratica passo-passo
per chi autora: `TUTORIAL_LIVELLO.md` (in caso di conflitto vince questo doc). Sostituisce le fasi
E1/E3/E4/E5 di `EDITOR_PLAN.md` come strada principale di authoring; la
modalità EDIT in-game resta come strumento di tuning rapido e debug.

## 1. Principi

- **Pipeline a senso unico**: `.blend` → `export_scn.py` → `.scn` (+ glb +
  `.zhm`). Il `.blend` è la FONTE DI VERITÀ del livello. L'F2-save
  dell'editor in-game su un livello autorato in Blender va considerato
  scratch (le modifiche vanno riportate a mano nel `.blend`).
- **La validazione vive nell'export**: lo script fallisce (o warna) con
  messaggi chiari invece di produrre un `.scn` rotto. Il runtime non deve
  difendersi da livelli malformati.
- **Zero magia**: ogni riga `.scn` emessa deriva da UNA regola di questo
  documento (prefisso nome → tipo entità; custom property → parametro).
- Lo script gira DENTRO Blender headless, come `gfx/sprite_render.py` e
  `gfx/terrain_bake.py` (di cui riusa il bake `.zhm`, ora fattorizzato in
  `bake_zhm()` su motore BVH — raycasta SOLO gli oggetti terreno, il resto
  del `.blend` non fa ombra):

  ```
  blender --background levels/level1.blend \
          --python gfx/export_scn.py -- \
          --out scenes/level1.scn [--mesh-dir meshes] [--ppm 4] [--no-bake]
          [--catalog props/catalog.txt]
  ```

  Emette `meshes/<stem>.glb` (+ `.zhm`), `meshes/<stem>_st.glb` e il `.scn`
  (`<stem>` = basename di `--out`). Output deterministico (oggetti in ordine
  di nome). Livello di prova: `levels/test_level.blend`, generato da
  `gfx/test_level_make.py`, esercita ogni regola di questo documento.

## 2. Assi, unità, origine

- **1 unità Blender = 1 metro.** Terreno di gioco `(x,y)` = Blender `(X,Y)`,
  quota = Blender `Z` (stessa convenzione di `terrain_bake.py`).
- **Il livello si modella nel quadrante positivo**: mondo = `[0,W]×[0,H]`,
  origine di gioco = origine di Blender. L'AABB XY del terreno DEVE avere
  min ≈ (0,0) (tolleranza mezza cella; l'exporter warna e trasla no,
  fallisce sì — allineare è compito di chi modella, come già per la `.zhm`).
- **Rotazioni**: solo attorno a Z (yaw). L'export legge `rotation_euler.z`
  in gradi per i prop; il segno/offset esatto verso il `rot` del `.scn` si
  fissa al primo prop reale con mesh asimmetrica (annotato in §9).

## 3. Identificazione delle entità: prefisso nome + custom properties

Il TIPO di entità lo dà il **prefisso del nome oggetto**; i PARAMETRI le
**custom properties** (pannello Object Properties → Custom Properties).
Regola di parsing del nome: `tipo[_sottochiave][.qualunque]` — il suffisso
numerico automatico di Blender (`.001`, `.002`…) è ignorato, quindi
duplicare con Shift+D "funziona e basta". Maiuscole/minuscole indifferenti.

Oggetti (e collection) col nome che inizia per `_` sono IGNORATI
dall'export: reference, guide, luci di lavoro, camera.

| Nome oggetto     | Geometria attesa      | Riga `.scn` emessa                          | Custom properties (default)            |
|------------------|-----------------------|---------------------------------------------|----------------------------------------|
| `goal`           | plane / mesh → AABB XY| `goal x y w h`                              | —                                       |
| `spawn`          | plane / mesh → AABB XY| `spawn x y w h`                             | — (script arriva con `exit`, §8)        |
| `pack`           | plane / mesh → AABB XY| `pack x y w h`                              | —                                       |
| `cost`           | plane / mesh → AABB XY| `cost x y w h weight`                       | `weight` (obbligatoria; <0 = richiamo)  |
| `wall`           | plane/box → AABB XY   | `wall hp cost_mult x y w h`                 | `hp` (500), `cost_mult` (1.0)           |
| `turret`         | empty o mesh → origine| `turret x y range heavy hp`                 | `range` (30), `heavy` (0), `hp` (0=default host) |
| `poly`           | mesh → hull convesso XY| `poly h solid x0 y0 …`                     | `height` (default: estensione Z della mesh), `cost` (assente = `solid`; presente = `cost <val>`) |
| `prop_<key>`     | empty o collection instance → origine | `prop key x y rot`          | — (`key` dal nome; parametri nel catalogo) |

Note:

- **I rect sono axis-aligned per formato** (`SceneRect`): l'export prende
  l'AABB world-space in XY. Un plane ruotato produce l'AABB che lo contiene
  — l'exporter warna se un oggetto-rect ha yaw ≠ 0 oltre tolleranza. Mura
  oblique oggi NON esistono nel formato (aperture §9).
- **`poly`**: l'export proietta tutti i vertici world-space su XY e prende
  il **convex hull** (il render fan vuole convessi; la scanline del raster
  gestirebbe anche concavi, ma la convenzione è: un poly = un convesso).
  Fallisce se il hull supera `SCENE_POLY_MAX_VERTS` (16). La comodità:
  si modella il volume 3D vero (un box estruso) e footprint+altezza escono
  da soli; `height` override per casi speciali (tettoie: footprint pieno,
  altezza bassa).
- **`turret`**: un empty "plain axes" basta; se si usa una mesh segnaposto
  conta solo l'origine dell'oggetto.

## 4. Parametri di scena: custom properties della SCENA Blender

Le righe `.scn` non spaziali vengono dalle custom properties della scena
(Scene Properties → Custom Properties):

| Property scena     | Riga `.scn`            | Default / note                             |
|--------------------|------------------------|--------------------------------------------|
| `cell`             | `cell v`               | 0.5                                         |
| `world_w`,`world_h`| `world W H`            | default: derivati dall'AABB XY del terreno (arrotondato alla cella); obbligatori se non c'è terreno |
| `set_<nome>`       | `set <nome> v`         | ripetibile: `set_k_density` → `set k_density`; `<nome>` deve essere un campo `SimPParams` (l'exporter NON valida la chiave — la valida `scene_load`, che oggi ignora silenziosamente: aperture §9) |
| `mission`, `budget` | riservate            | GAME_PLAN fase A (§8)                       |

## 5. Terreno e statici: collection dedicate

Due collection top-level col nome riservato:

- **`terrain`** — la mesh del suolo (con i buchi-palazzo, EDITOR_DESIGN §9).
  L'export: (a) esporta la collection in `meshes/<level>.glb` (glb#1),
  (b) invoca il bake `.zhm`/ZHM2 (riuso della logica di `terrain_bake.py`,
  siamo già dentro Blender — niente secondo processo), (c) emette
  `terrain meshes/<level>.glb`. `--no-bake` salta (b) quando si itera solo
  sul layout.
- **`statics`** — palazzi/rocce indistruttibili e invalicabili (glb#2,
  visiva). L'export la esporta in `meshes/<level>_st.glb` ed emette
  `statics …`. In più, **per ogni mesh della collection emette anche il suo
  footprint come `poly <h> solid`** (hull convesso XY, altezza = estensione
  Z): visiva e nav nascono dallo STESSO oggetto e non possono divergere.
  Custom property `nav="none"` sull'oggetto per l'opt-out (decoro alto ma
  sorvolabile/attraversabile, es. arco, pensilina).

Tutto il resto (entità §3, prop §6) sta in collection libere a piacere di
chi modella: l'export identifica per NOME OGGETTO, non per collection.

## 6. Prop di catalogo: collection instances (o empty)

I prop (`props/catalog.txt`) si piazzano nel modo Blender-nativo:

1. **Libreria**: `blend/props.blend` (generata da
   `gfx/props_library_make.py`, placeholder alla scala vera) con UNA
   collection per chiave di catalogo (`bench`, `cart`, `trafsign`…), mesh
   alla scala giusta, origine alla base (dove tocca terra).
2. **Nel livello**: File → Link (o Append) della collection, poi Add →
   Collection Instance. Si vede la mesh vera nel viewport, si sposta/ruota
   l'empty di istanza. L'export riconosce `obj.instance_collection` e usa
   il **nome della collection** come chiave; il nome dell'istanza è libero.
3. **Fallback senza libreria**: un empty chiamato `prop_<key>` (es.
   `prop_bench.003`) — utile finché le mesh di catalogo sono placeholder.

L'export valida la chiave contro `props/catalog.txt` (chiave ignota =
errore) e rispetta `SCENE_PROP_KEY_LEN` (24).

## 7. Validazioni dell'export (fail = niente `.scn`)

Errori bloccanti:
- prefisso nome non riconosciuto e non `_`-ignorato (typo = entità persa
  in silenzio: MAI silenzioso);
- poly con hull > 16 vertici; prop con chiave fuori catalogo;
- superati i limiti del formato (`SCENE_MAX_RECT` 64 per tipo,
  `SCENE_MAX_POLY` 256, `SCENE_MAX_PROP` 256);
- entità fuori dal mondo `[0,W]×[0,H]`; `cost` senza `weight`;
- AABB min del terreno lontano da (0,0) oltre mezza cella.

Warning (il `.scn` esce comunque):
- rect con yaw ≠ 0 (esportato l'AABB);
- `wall` più sottile della cella nav (rasterizza a strisce vuote);
- spawn/goal che intersecano un `poly solid` o un buco terreno
  (check geometrico cheap in export; la validazione vera di
  raggiungibilità — flood fill su `phi` — resta rimandata come in
  EDITOR_PLAN E4, aperture §9).

## 8. Nomi riservati per GAME_PLAN fase A (non ancora implementati)

Quando `mission.c` estenderà il formato (`exit`/`lz`/`mission`/`budget`),
la convenzione è già fissata — NON usare questi nomi per altro:

- `exit` — rect (come `spawn`) con properties `rate`, `delay`, `pool` →
  `exit x y w h rate [delay] [pool]`;
- `lz` — empty puntuale (uno solo per livello) → `lz x y`;
- scena: `mission` (stringa, es. `"survive 300 prep 90"`), `budget`
  (float) → righe `mission …` / `budget …`.

Regola invariata da EDITOR_PLAN §1: il formato lo estende chi implementa
la semantica; l'exporter aggiunge l'emissione DOPO che `scene_load` +
`test_scene` la consumano.

## 9. Aperture / rimandate

1. **Segno/offset dello yaw prop** (Blender `rotation_euler.z` → `rot`
   `.scn`): si fissa empiricamente al primo prop con mesh asimmetrica.
2. **Rect ruotati** (mura oblique): il formato è axis-aligned; se il level
   design le chiede, è un'estensione del `.scn` (angolo nel formato +
   raster), non dell'exporter.
3. **Prop-izzazione dei palazzi** (idea 3 lug 2026): oggi i palazzi sono
   `statics` (glb#2 + poly solid + buco terreno). Se serviranno stati di
   danno da bombardamento (texture "danneggiata"), migreranno a prop di
   catalogo con stati visivi (DESTRUCT/EDITOR_DESIGN §10) e la collection
   `statics` si svuoterà verso istanze §6. La convenzione §6 già li copre;
   il runtime ORA anche (2026-07-04): loader glb prop (`load_prop_models`)
   + footprint nav dal catalogo (`prop_world_apply`, colonna `WxD` §8.5) —
   un palazzo-prop è una riga `solid H WxD inf` (es. `building`, già in
   catalogo). `statics` resta per i palazzi fusi nel terreno del livello.
4. **Validazione di raggiungibilità** (goal raggiungibile da ogni spawn):
   flood-fill sul `phi` del core — vive meglio in un tool C headless che
   carica il `.scn` esportato (`scene_instantiate` di prova), invocabile
   dallo script dopo l'export. Rimandata come EDITOR_PLAN E4.
5. **`set_<nome>` non validato in export**: `scene_load` oggi ignora le
   chiavi `set` ignote in silenzio; portare la lista dei campi
   `SimPParams` nell'exporter è duplicazione — meglio (un giorno) far
   warnare `scene_load`. Annotato, non bloccante.
6. **Heightmap del terreno vs `world`**: se il terreno è più grande del
   mondo dichiarato (`world_w/h` espliciti più piccoli dell'AABB), oggi
   vince la dichiarazione; l'exporter warna sulla discrepanza.

# Rig Mixamo → VAT: trappole di export e scala

Note operative sul pipeline che porta un personaggio dal modeling in Blender agli
asset VAT (`vat/assets/<nome>_*`). Nate dal debug del modello **obeso**
(`male_version_obese`), che usciva storto mentre il **normale** (`male_version`)
funzionava. Servono per non ricominciare da capo: i due bug qui sotto si ripresentano
ogni volta che si aggiunge un personaggio.

## Il pipeline

1. **Blender** → export FBX della sola mesh (T-pose) → upload su **Mixamo**.
2. **Mixamo** auto-rig → download FBX riggato (mesh + armatura `mixamorig`).
   Salvato come `blend/<nome>_rigged.fbx`.
3. **`vat/bake_zombie.sh <rigged.fbx> vat/assets/<nome>`** applica le 13 clip di
   `vat/source/` al rig e baka le VAT.

Gli FBX li si ispeziona **dall'interno** (binari) con:

```
python3 vat/fbx_inspect.py blend/male_version.fbx blend/male_version_obese.fbx
```

Stampa GlobalSettings (UpAxis/FrontAxis/CoordAxis + segni, UnitScaleFactor) e i
transform di ogni nodo Model (Lcl Scaling/Rotation/Translation, PreRotation). Il
confronto "modello buono vs modello rotto" rende ovvia la differenza.

Per la scala mesh-vs-armatura all'import serve Blender (legge `obj.matrix_world.to_scale()`
di mesh e armatura dopo `import_scene.fbx`): devono **coincidere**.

## Bug 1 — export verso Mixamo sbagliato (orientamento + scala)

**Sintomo:** dopo l'auto-rig, ossa ruotate di ~90°, e a baking lo zombie nasce
**ribaltato/tombolante** (NON raddrizzabile con una rotazione in post). All'import
del rigged: mesh a scala 1.0 e armatura a 0.01 (mismatch) → l'armature deform fa un
round-trip mesh→world→spazio-armatura con scale diverse e impazzisce.

**Causa (letta dagli FBX pre-Mixamo):** l'export Blender dell'obeso aveva
GlobalSettings/transform diversi dal normale. Tabella del confronto che ci ha guidati:

| campo (pre-Mixamo) | normale (OK) | obeso (rotto) |
|---|---|---|
| `UpAxis` | 1 = **Y-up** | 2 = **Z-up** |
| `FrontAxisSign` / `CoordAxisSign` | +1 / +1 | −1 / −1 (facing invertito) |
| mesh `Lcl Rotation` | assente | `90,0,180` (rotazione non applicata) |
| mesh `Lcl Scaling` | assente | `100,100,100` (scala non applicata) |
| `UnitScaleFactor` | 1.0 | 100 → poi 10000 (a seconda di "Apply Scalings") |

Mixamo vuole **Y-up**; caricato Z-up, l'auto-rig piazza i giunti seguendo quell'asse →
rig storto. La scala non applicata diventa il mismatch mesh/armatura.

**Fix (settaggi export FBX da Blender, da far combaciare col normale):**
- Transform **Up = Y**, **Forward = Z** (finché `FrontAxisSign`/`CoordAxisSign` = +1);
- **applicare i transform alla mesh** (Object → Apply → All Transforms): azzera `Lcl Rotation`;
- **Apply Scalings = `FBX All`** + checkbox **sperimentale "Apply Transform" = ON**:
  cuoce conversione assi e unità nella geometria → `UnitScaleFactor = 1.0` e niente
  `Lcl Scaling`/`Lcl Rotation` residui (nodi puliti come il normale).

NON serve toccare le singole ossa: Mixamo rigenera l'armatura da zero, conta solo la
mesh caricata. Verifica col parser: il nuovo upload deve leggere `UpAxis=1`,
`FrontAxisSign=+1`, `CoordAxisSign=+1`, mesh **senza** `Lcl Rotation` né `Lcl Scaling`.

**Stato:** RISOLTO. Dopo il giro corretto, `male_version_obese_rigged.fbx` importa con
mesh **e** armatura a 0.01 (come il normale) e baka uno zombie **in piedi col deform
corretto** (verificato a video con `vat_view`).

## Bug 2 — Scale sbagliato nel dialog di export FBX (stride 100×) — RISOLTO (causa nota)

**Sintomo:** l'obeso a baking dà `stride_m ≈ 131` per il walk (il normale: `≈ 1.31`).
Il **visual è corretto** (l'altezza viene normalizzata dal baker), ma lo `stride` è
esattamente 100× troppo grande su TUTTE le clip (131.85/1.31, 82.46/0.82, … = 100.x).

**Perché è un problema:** `vat_layer.c` avanza la fase del passo con
`phaseA += dist/stride` se `stride > 0.1`, altrimenti `dt/duration`. Con `stride=131`
i piedi avanzano 1/100 del dovuto → **piedi quasi congelati** mentre il corpo scivola.
Il fallback a tempo NON scatta (131 ≫ 0.1).

**Causa REALE (non quella che credevamo):** NON è la scala del modeling — i due `.blend`
sono identici (mesh ~176 unità, world_scale 1.0 entrambi). La differenza è stata fatta
**nel dialog di export FBX di Blender**: per il normale il campo *Transform → Scale* era
**1**, per l'obeso era **0.01**. Quel `0.01` rimpicciolisce la geometria caricata su
Mixamo ×100 (normale rigged ≈ 17.600 unità raw, obeso rigged ≈ 177). Mixamo emette il
root motion delle clip in unità FISSE (~119) indipendenti dalla taglia mesh: sul normale
"grande" è proporzionato → stride 1.31; sull'obeso "piccolo" è ~100× la sua taglia →
stride 131. (La vecchia teoria "obeso a scala metri / rapporto mesh-root" era sbagliata:
il rapporto nasce SOLO dal campo Scale dell'export, non dal blend.)

**Fix:** ri-esportare l'obeso da `blend/male_version_obese.blend` con **Transform →
Scale = 1** (tutto il resto come il preset Bug 1: Up=Y, Forward=Z, Apply Scalings = FBX
All, Apply Transform ON), ri-uploadare su Mixamo, ri-riggare, ri-bakare. Niente da
scalare nel blend. **FATTO il 17/6** (`male_version_obese.fbx` ore 19:48).

**Verifica (ciò che conta):** l'unico predittore dello stride è l'**altezza WORLD** che
l'upload FBX presenta a Mixamo. Il baker legge `mesh.matrix_world @ v.co` (coord world) e
calcola `scale = 1.8/altezza_world`, `stride = spostamento_world × scale` — un RAPPORTO,
invariante a scalature uniformi. Mixamo emette un root motion ASSOLUTO (~128 cm): su mesh
world ~176 → stride 128/176×1.8 ≈ 1.31; su mesh world ~1.76 → ≈ 131. NON contano le unità
raw (normale 17.640 a wscale 0.01 vs obeso 175 a wscale 1.0): conta solo l'altezza fisica.
Misurato sul ri-export: obeso upload world **175.6** ≈ normale **176.4** → atteso stride ≈ 1.3.
Conferma finale: dopo Mixamo + re-bake, `stride_m(walk)` deve uscire ≈ 1.31.

**Regola per i prossimi personaggi:** export FBX verso Mixamo SEMPRE con Scale = 1
(Apply Transform ON). NON usare Scale=0.01 per "rimpicciolire" un modello che sembra
troppo grande: cambia l'altezza world e rompe lo stride.

**Stato asset:** `vat/assets/zombie_man_obese_*` è ancora bakato col rig vecchio
(Scale 0.01) → stride 100×. Da rifare con il giro qui sopra prima di usarlo per walk/run.
`vat/assets/zombie_man_*` (normale) è a posto.

## Bug 3 — auto-rig Mixamo FALLISCE (UnitScaleFactor 100) — RISOLTO

**Sintomo:** ri-esportato l'obeso a "Scale=1" per fixare il Bug 2, Mixamo rifiuta
l'auto-rig (fallisce del tutto, non solo stride storto).

**Causa (letta dagli FBX):** l'export obeso usciva con **`UnitScaleFactor = 100`** mentre
il normale (che rigga) ha **`UnitScaleFactor = 1.0`**. Con 100, Mixamo moltiplica la
geometria raw (~175 unità) → vede un personaggio alto **~175 metri** → fuori tolleranza
auto-rig. Il normale ha invece la conversione di unità **cotta nei vertici** (raw ~17.640,
Unit 1.0). Cambiare solo il campo *Scale* nel dialog non basta: conta **Apply Scalings**.

**Fix (verificato con uno sweep di tutti i parametri, GS confrontati col normale):**
ricetta esatta in **`vat/export_mixamo_fbx.py`** (headless, riproducibile). I tre
parametri che contano:
- `axis_up='Y'`, `axis_forward='-Z'` → `FrontAxisSign/CoordAxisSign = +1` (con `'Z'`
  escono −1 = il facing invertito del Bug 1);
- `bake_space_transform=True` ("Apply Transform") → raddrizza su Y + cuoce il ×100;
- `apply_scale_options='FBX_SCALE_NONE'` → `UnitScaleFactor = 1.0` (con `'FBX_SCALE_ALL'`
  o `'FBX_SCALE_UNITS'` esce 100 → il fail). `global_scale=1.0`.

Uso: `blender --background blend/<modello>.blend --python vat/export_mixamo_fbx.py --
blend/<modello>.fbx`. Verifica con `fbx_inspect.py`: deve leggere identico al normale
(Front +1, Coord +1, Unit 1.0; al reimport geometria Y-up wscale 0.01, raw ~17.6k).

**Stato:** `blend/male_version_obese.fbx` rigenerato con lo script il 17/6 → GS e geometria
identici al normale. Pronto per l'upload Mixamo. (Backup del file rotto Unit=100:
`/tmp/obese_backup_unit100.fbx`, temporaneo.)

## Pipeline DEFINITIVA varianti (obesi/bambini/donne) — non ri-riggare su Mixamo

L'auto-rig di Mixamo sul lowpoly (297 vert) è una lotteria: dipende dalla posizione dei
marker piazzati a mano, e con così pochi vertici l'auto-detector spesso fallisce
("please place all markers" / "unknown error while generating motion"). Un modello denso
(es. `working_test.fbx`, 2237 vert) rigga senza problemi → conferma che è la geometria
rada il limite. La subdivision surface fa passare il rig ma rende la mesh densa (VAT
pesante, stile non coerente col lowpoly).

**Soluzione adottata: riggare UNA volta sola il normale su Mixamo, poi trasferire il rig
alle varianti.** Tutti i 5 modelli condividono base e scheletro (cambiano pochi vertici:
pancia per gli obesi, loop cut+seno per le donne). Verificato che Mixamo PRESERVA l'ordine
dei vertici (Procrustes `male_version.blend` vs `male_version_rigged.fbx`: rotazione
identità, scala 1, residuo ~0).

- **Obesi, bambini** (topologia IDENTICA al normale): `vat/transfer_rig.py` — importa il
  normale riggato, sposta i vertici per INDICE col delta (variante − base) dai .blend,
  pesi intatti → FBX riggato. Esatto. Es.:
  `blender --background --python vat/transfer_rig.py -- blend/male_version_rigged.fbx
  blend/male_version.blend blend/male_version_obese.blend blend/male_version_obese_rigged.fbx`
  poi `vat/bake_zombie.sh blend/male_version_obese_rigged.fbx vat/assets/zombie_man_obese`.
- **Donne** (loop cut → topologia diversa): NON usare transfer_rig.py. Serve trasferimento
  pesi per PROSSIMITÀ (modificatore Data Transfer dalla mesh riggata, stessa armatura).
  Script da scrivere quando serve.

**Stato:** `vat/assets/zombie_man_obese_*` RI-BAKATO il 17/6 via transfer_rig.py →
stride walk 1.3204 (era 131), scale 0.01103, tutte le 13 clip, in piedi e deforma
corretto (verificato con `vat_view` shot). Bug 2 CHIUSO. NB: tutta la saga
export-FBX (Bug 1/2/3 qui sopra) è stata in parte un detour — per le VARIANTI non si
passa più da Mixamo, quindi i settaggi di export contano solo per riggare un modello
NUOVO da zero (lì resta valido `vat/export_mixamo_fbx.py`).

## Quarta via — variante GIA' RIGGATA nel .blend (niente Mixamo, niente transfer)

Quando il rig vive gia' nel file di modeling (mesh + armatura `mixamorig` riggate a
mano in Blender) non serve ne' l'auto-rig Mixamo ne' un transfer di pesi: si esporta
direttamente mesh **e** armatura in FBX e si baka. E' il caso piu' semplice — usato per
`fem_version_skirt` (la gonna aggiunge geometria → topologia diversa da base e fem, ma
il rig era gia' nel blend).

1. **`vat/export_rigged_fbx.py`** — esporta mesh+armatura del .blend aperto in FBX, con
   gli assi della pipeline (`axis_up='Y'`, `axis_forward='-Z'`, `bake_anim=False`). A
   differenza di `export_mixamo_fbx.py` (sola mesh, per l'upload all'auto-rig) qui il rig
   c'e' gia' e va portato fuori intatto. Uso:
   `blender --background blend/<modello>.blend --python vat/export_rigged_fbx.py --
   blend/<modello>_rigged.fbx`
2. **`vat/bake_zombie.sh blend/<modello>_rigged.fbx vat/assets/<nome>`** → le 13 clip.

**Texture:** il baker NON esporta il diffuse (`<prefix>_diffuse.png`, caricato da
`vat_view`/`vat_layer` per UV). Al momento le texture dei modelli sono PLACEHOLDER
(`blend/texmap.png`, 256×256), quindi non si versiona nessun `_diffuse.png` per le
varianti: si copiera' la texture definitiva quando ci sara'. Senza diffuse il viewer va
in flat shading (`no png ... (flat shading)`), il deform/baking resta corretto.

**Stato:** `vat/assets/zombie_fem_skirt_*` bakato il 18/6 via questa via → stride walk
1.3337, scale 0.01114 (come la fem), 13 clip, in piedi e deforma corretto con la gonna
visibile (verificato `vat_view` shot). Ultima variante zombie.

## Manopole aggiunte al baker durante il debug

`vat/bake_vat.py` ha ora `--rotx/--roty/--rotz <gradi>` (rotazione del modello via
empty-pivot all'origine, deform-safe) per correzioni di orientamento legittime (FBX
esportato con asse up sbagliato). NON è la soluzione per i due bug qui sopra (quelli si
fixano alla sorgente), ma resta utile in generale.

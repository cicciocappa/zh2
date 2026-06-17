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

## Bug 2 — scala assoluta del modeling diversa (stride 100× sballato) — APERTO

**Sintomo:** l'obeso a baking dà `scale ≈ 1.10` e `stride_m ≈ 131` per il walk (il
normale: `scale ≈ 0.011`, `stride ≈ 1.31`). Il **visual è corretto** (l'altezza viene
normalizzata a 1.8 m dal baker), ma lo `stride` è 100× troppo grande.

**Perché è un problema:** `vat_layer.c` avanza la fase del passo con
`phaseA += dist/stride` se `stride > 0.1`, altrimenti `dt/duration`. Con `stride=131`
i piedi avanzano 1/100 del dovuto → **piedi quasi congelati** mentre il corpo scivola.
Il fallback a tempo NON scatta (131 ≫ 0.1).

**Causa precisa (misurata):** il personaggio normale è modellato a scala "cm"
(bbox mesh ~17600 unità, world span ~176), l'obeso a scala "metri" (bbox ~175,
world span ~1.76) → **100× di differenza di taglia assoluta** nel file. Mixamo però
esporta il root motion delle clip in cm (~119 unità in entrambi). Sul normale è
proporzionato; sull'obeso quel movimento è ~68× la sua taglia. È il **rapporto
taglia-mesh / movimento-root** ad essere sbagliato nel rig: uno scaling UNIFORME nel
baker non lo corregge (mesh e movimento scalano insieme, il rapporto resta).

**Due strade (DECISIONE DA PRENDERE a casa):**
1. **`--stride-scale 0.01` nel baker** (manopola da aggiungere): moltiplica solo il
   numero `stride_m` scritto nel meta (131 → 1.31), niente tocca geometria/anim già
   corrette. Asset pronto subito; resta un rig "mal-scalato" ma funzionante e documentato.
2. **Ri-upload su Mixamo a scala cm**: in Blender scalare l'obeso ×100 per matchare i
   ~176 unità del normale, ri-esportare (stesso preset del Bug 1) e ri-riggare. Rig
   pulito e coerente col normale, stride corretto nativo. Costo: un altro giro Mixamo.
   → Probabilmente la scelta giusta long-term: tenere TUTTI i modelli alla stessa scala
   (cm) evita che il bug si ripresenti su ogni nuovo personaggio.

**Stato attuale degli asset:** `vat/assets/zombie_man_obese_*` è bakato col rig BUONO
(in piedi, deform corretto) ma con lo **stride ancora 100×** (vedi i `stride_m` enormi
nel `_meta.txt`). Va sistemato con una delle due strade prima di usarlo per il walk/run.
`vat/assets/zombie_man_*` (normale) è invece a posto.

## Manopole aggiunte al baker durante il debug

`vat/bake_vat.py` ha ora `--rotx/--roty/--rotz <gradi>` (rotazione del modello via
empty-pivot all'origine, deform-safe) per correzioni di orientamento legittime (FBX
esportato con asse up sbagliato). NON è la soluzione per i due bug qui sopra (quelli si
fixano alla sorgente), ma resta utile in generale.

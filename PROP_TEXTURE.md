# Texture dei prop — pipeline a proiezione cubica (guida d'uso)

Pipeline per texturizzare i prop del catalogo dipingendo **6 immagini viste
"in fotografia"** invece che nello spazio UV: l'analogo per i prop della
pipeline outfit (OUTFIT_DESIGN.md), stessa filosofia — template → dipingi (o
generi con SDXL+ControlNet) → bake di proiezione sulle UV vere. Gli script
girano in Blender headless (portatile in `~/Scaricati/blender-5.1.0-linux-x64/`).

I 6 lati prendono il nome dall'asse mondo su cui sta la camera (Blender Z-up):
`px nx py ny pz nz` (pz = dall'alto, nz = da sotto). Il framing è ricalcolato
in modo deterministico dal bounding box, identico in template e bake: ciò che
dipingi sul template atterra sul prop al pixel.

## 1. Genera i template

```
~/Scaricati/blender-5.1.0-linux-x64/blender --background \
    --python gfx/prop_template.py -- \
    --glb assets/models/props/bus.glb \
    --out gfx/out/prop_tpl/bus [--res 1024] [--scale 1.0]
```

Output: `bus_px.png … bus_nz.png` (viste workbench con cavity/outline, sfondo
trasparente) + una riga di log con S (metri) e px/m per lato. Il framing è
**per-lato** (fittato sulla silhouette di quel lato): sul bus il muso è
inquadrato stretto, la fiancata larga — canvas sempre pieno. `--scale` = la
colonna scale del catalogo (i placeholder sono 1.0).

## 2. Dipingi (o genera) le immagini

Lavora sopra i template, salva in una cartella per prop, es.
`gfx/prop_src/bus/`, con nome `px.png` / `nx.png` / … (accettato anche
`qualcosa_px.png`). **Non servono tutte e 6**:

- lato mancante → il bake usa l'**opposto** (proiettato attraverso, quindi
  specchiato: ok per superfici generiche — fiancate di bus, muri);
- opposto mancante anche lui → grigio medio.

In pratica: una cassa vive con 1-2 immagini, un palazzo con 3-4; `nz` (da
sotto) non si vede mai in gioco.

## 3. Bake

```
~/Scaricati/blender-5.1.0-linux-x64/blender --background \
    --python gfx/prop_bake.py -- \
    --glb assets/models/props/bus.glb \
    --images gfx/prop_src/bus \
    --out gfx/out/props/bus_diffuse.png \
    [--res 512] [--supersample 2] [--sharp 8] [--scale 1.0] \
    [--preview] [--export-glb gfx/out/props/bus.glb]
```

- Le 6 proiezioni sono miscelate **pesando la normale** della superficie:
  `--sharp` alto = scelta netta per faccia (giusto per i prop squadrati),
  basso = blend morbido sugli smussi. Default 8.
- Se il glb non ha UV (i placeholder procedurali) viene generata una **Smart
  UV Project** al volo; se le ha (modello tuo), usa le tue — meglio.
- `--preview` renderizza `<out>_prev_<lato>.png`: il prop texturato visto dai
  6 lati, per il controllo a occhio.
- `--export-glb` riesporta il glb **con le UV di bake e la texture
  incorporata** (materiale Principled): è il file destinato al loader di
  gioco texturato.

## Stato runtime (attenzione)

Il gioco oggi NON mostra ancora le texture dei prop: `load_glb_soup` in
`vat_horde` legge solo posizione+normale+colore piatto (commento nel codice:
"si aggiunge il path texturato quando arrivano i prop veri"). Il pezzo
mancante: soup esteso a UV, bind della diffuse, shader tipo `mesh.vs/fs`
(esiste, lo usano i mesh-gib), draw dei prop spezzato per-def. Fino ad
allora, il giudizio visivo si fa con `--preview`.

## Verifica fatta (2026-07-12)

Bus end-to-end con 6 immagini di test etichettate (colore+scritta per lato +
marker d'origine): maschere per-lato corrette, nessuna specchiatura,
allineamento template↔bake al pixel, Smart UV + export glb funzionanti.

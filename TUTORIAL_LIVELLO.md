# Tutorial: creare un livello in Blender per zh2

> Guida passo-passo all'authoring. La convenzione normativa è
> `BLENDER_LEVEL.md` (in caso di conflitto vince quella); qui c'è il percorso
> pratico. Pipeline a senso unico: **`.blend` → `gfx/export_scn.py` → `.scn` +
> glb + `.zhm`**. Il `.blend` è la fonte di verità; l'F2-save in-game è solo
> scratch. Esempio funzionante da tenere aperto come riferimento:
> `levels/test_level.blend` (generato da `gfx/test_level_make.py`, esercita
> ogni regola).

## 0-bis. Il metodo: greybox → kit → vestizione (deciso 14 lug 2026)

La risposta alla domanda "apro Blender e ora che faccio?" — l'ordine di
lavoro, PRIMA della tecnica:

1. **Greybox giocabile lo stesso giorno.** Un livello di zh2 spogliato
   dell'arte è topologia di pressione: LZ, cinta con cancelli e sezioni
   deboli, exit, palazzi-box che incanalano. Tutto questo si costruisce con
   soli box e plane (il vincolo "hull ≤ 16 vertici, palazzi = box" dice
   proprio questo) e si playtesta subito in `vat_horde`. Si itera sul layout
   finché l'assedio è DIVERTENTE; l'arte arriva dopo. Vestire un livello
   brutto è tempo buttato.
   **Regola: il livello NON piazza difese.** Il livello fornisce terreno,
   mura, cancelli, exit — la difesa (torrette, barricate aggiuntive) è
   SEMPRE una decisione del giocatore in PREP, pagata col budget. Mai
   `turret` in un livello di gioco (nei banchi di test/demo sì). Il
   valore delle difese "che avresti regalato" va nel budget.
2. **Kit modulare per ambientazione, non diorami bespoke.** 10–20 pezzi
   riusabili (segmenti di cinta, cancello, gusci di edificio, container…)
   entrano nel catalogo prop + `blend/props.blend`; un livello nuovo =
   greybox → pezzi kit al posto dei box → 1–2 landmark unici. L'identità
   del luogo la danno il layout e i landmark, non mille mesh uniche.
3. **Luoghi inventati con stile riconoscibile; OSM solo come lucido.**
   Import di geometria reale scartato (footprint sporchi, geografia non
   pensata per l'assedio, landmark costosi). Se serve un tracciato credibile:
   curve OSM in un layer `_ref` e ci si ricalca sopra. Google Earth escluso
   (ToS + mesh fotogrammetriche inutilizzabili).
4. **Prima ambientazione: stabilimento industriale** — cinta nativa,
   capannoni-box, terreno piatto, piazzale = LZ, exit con fiction facile
   (cancelli/brecce). Favela/borgo medievale = ambientazioni successive.
5. **Mai partire dalla tela bianca**: il seed del livello si genera via
   script (`gfx/level1_make.py` → `levels/level1.blend`); si apre Blender
   su un livello già bloccato e si rimodella. Rilanciare lo script
   SOVRASCRIVE il `.blend`: dopo la prima modifica a mano, il `.blend` è la
   fonte di verità e il seed non si rigenera più.

## 0. Prerequisiti

- Blender 5.1 portatile: `~/Scaricati/blender-5.1.0-linux-x64/blender`
  (non è nel PATH).
- Si lavora **dalla root del progetto** (i path negli eseguibili sono
  hardcoded).
- Il file del livello va in `levels/` (i `.blend` di zombie e prop stanno in
  `blend/`, non mescolarli).

## 1. Setup della scena

1. Nuovo file → salva subito come `levels/mio_livello.blend`.
2. **1 unità Blender = 1 metro**; `(X,Y)` = piano di gioco, `Z` = quota.
3. Modella tutto nel **quadrante positivo**: il mondo è `[0,W]×[0,H]` e
   l'origine di Blender è l'origine di gioco.
4. Camera, luci di lavoro, mesh di riferimento: o le lasci (camera/luci sono
   ignorate di suo), o le chiami con un nome che inizia per **`_`**
   (es. `_ref_foto`) — tutto ciò che inizia per `_` è invisibile all'export.

## 2. Il terreno (collection `terrain`)

1. Crea una collection top-level chiamata esattamente **`terrain`**.
2. Dentro mettici la mesh del suolo (con gli eventuali buchi-palazzo).
3. **Vincolo duro**: l'angolo min dell'AABB XY del terreno deve stare a
   **(0,0)** entro mezza cella (0.25 m con cella default) — altrimenti
   l'export **fallisce**. Non trasla da solo: allineare è compito tuo.
4. Da questa collection l'export produce `assets/terrain/<livello>.glb` + il
   bake `.zhm` (heightmap di collisione) e la riga `terrain …` nel `.scn`.

Se il livello non ha terreno (arena piatta), devi dichiarare
`world_w`/`world_h` a mano (vedi §6).

## 3. Palazzi e rocce (collection `statics`)

1. Seconda collection top-level riservata: **`statics`** — tutto ciò che è
   indistruttibile e invalicabile.
2. Per ogni mesh l'export emette **automaticamente** anche il footprint nav:
   `poly <h> solid` dal hull convesso XY, altezza = estensione Z. Visiva e
   collisione nascono dallo stesso oggetto e non possono divergere.
3. Opt-out: custom property **`nav = "none"`** sull'oggetto per decoro alto
   ma attraversabile (archi, pensiline).
4. Attenzione al limite: hull con **max 16 vertici** in pianta, sennò
   l'export blocca. Palazzi = box semplici, non mesh scolpite.

## 4. Le entità di gameplay (prefisso nome + custom properties)

Il TIPO lo dà il prefisso del nome oggetto, i PARAMETRI le custom properties
(Object Properties → Custom Properties). Il suffisso `.001` di Blender è
ignorato: **duplicare con Shift+D funziona e basta**. Regola nome:
`tipo[_etichetta][.qualunque]` — es. `spawn_nord.002` è uno `spawn`.

| Cosa     | Come si modella                                   | Custom properties |
|----------|---------------------------------------------------|-------------------|
| `goal`   | plane sull'area drain                             | — |
| `spawn`  | plane sull'area d'emissione                       | — |
| `pack`   | plane (branco dormiente)                          | — |
| `cost`   | plane (paura/fango/richiamo)                      | `weight` **obbligatoria** (negativa = richiamo) |
| `wall`   | plane o box sul muro distruttibile                | `hp` (default 500), `cost_mult` (default 1.0) |
| `turret` | **empty** "plain axes" (o mesh: conta solo l'origine) | `range` (30), `heavy` (0), `hp` (0 = default host) |
| `poly`   | volume 3D vero (box estruso): footprint+altezza escono da soli | `height` (override), `cost` (se presente → costo nav invece di solido) |
| `exit`   | plane sull'area d'emissione scriptata (director di missione) | `rate` **obbligatoria** (agenti/s), `delay` (s, default 0), `pool` (default 0 = illimitato) |
| `lz`     | **empty**, uno solo per livello; lo yaw orienta il container | — |

Trappole:

- I rect (`goal`/`spawn`/`pack`/`cost`/`wall`) sono **axis-aligned per
  formato**: un plane ruotato esporta l'AABB che lo contiene (l'export warna
  se yaw ≠ 0). Mura oblique non esistono nel formato.
- Un `wall` più sottile della cella nav (0.5 m) warna: rasterizza a strisce
  vuote. Fallo spesso almeno una cella.
- Trucco per la breccia/sezione debole (stile `arena_breach`): più oggetti
  `wall` affiancati, quello debole con `hp` e `cost_mult` bassi.

## 5. I prop di decoro

Due strade:

1. **Con libreria** (`blend/props.blend`, una collection per chiave di
   catalogo — placeholder generati da `gfx/props_library_make.py`):
   File → Link della collection, poi Add → Collection Instance.
   Sposti/ruoti l'istanza e vedi la mesh vera; l'export usa il **nome della
   collection** come chiave.
2. **Senza libreria** (placeholder): un empty chiamato `prop_<chiave>`,
   es. `prop_bench.003`.

La chiave DEVE esistere in `assets/props/catalog.txt`, sennò l'export blocca.
Le colonne entity-axis del catalogo (`solid H WxD / hp mult / opac / mass`)
sono APPLICATE dall'host (2026-07-04, `prop_world.c`): un prop `solid` alza
il suo footprint W×D nella nav (ruotato con l'istanza), con hp finiti è
assediabile e crolla, `opac` regola i proiettili. Quindi **anche gli edifici
possono essere prop** (es. `building`): conviene quando lo stesso palazzo
ricorre in più livelli o dovrà avere stati di danno; `statics` resta per i
volumi fusi nel terreno del singolo livello.

## 6. Parametri di scena (Scene Properties → Custom Properties)

| Property             | Effetto            | Note |
|----------------------|--------------------|------|
| `cell`               | `cell v`           | default 0.5 |
| `world_w`, `world_h` | `world W H`        | default: AABB del terreno; **obbligatori senza terreno** |
| `set_<nome>`         | `set <nome> v`     | es. `set_k_density` → parametro `SimPParams`; occhio ai typo, `scene_load` li ignora in silenzio |
| `mission`            | `mission …`        | stringa, inizia per `survive` o `clear` (es. `"survive 180 prep 0"`) |
| `budget`             | `budget N`         | budget di piazzamento PREP |
| `biostock`           | `biostock N`       | scorta iniziale munizioni (omesso = default host) |

Con una `lz` nel livello NON serve un `goal`: l'host costruisce core
assediabile + goal centrale dalla LZ (`build_lz_core`).

## 7. Export

Dalla root del progetto:

```bash
~/Scaricati/blender-5.1.0-linux-x64/blender --background levels/mio_livello.blend \
    --python gfx/export_scn.py -- \
    --out assets/scenes/mio_livello.scn
```

Default già giusti: mesh+zhm in `assets/terrain/`, catalogo
`assets/props/catalog.txt`. Opzioni utili:

- `--no-bake` — salta il bake `.zhm` quando stai solo iterando sul layout
  (molto più rapido);
- `--ppm 4` — densità campioni della heightmap.

La validazione è **bloccante**: prefisso ignoto, entità fuori mondo, chiave
prop fuori catalogo, hull > 16 vertici, limiti formato (64 rect per tipo,
256 poly, 256 prop), `cost` senza `weight`, terreno non a (0,0) → exit 1 e
**niente output**. I messaggi dicono quale oggetto è il colpevole. Warning
(il `.scn` esce comunque): rect ruotati, wall sottili, spawn/goal dentro un
solido.

## 8. Provare il livello

```bash
make vat_horde
./vat_horde assets/scenes/mio_livello.scn
```

- `VAT_HORDE_RATE=...` accelera lo spawn per osservare assedi e crolli senza
  aspettare.
- La modalità EDIT in-game va bene per il tuning rapido, ma **F2 salva uno
  scratch**: le modifiche che vuoi tenere le riporti a mano nel `.blend`,
  che resta la fonte di verità.

## 9. Checklist rapida prima dell'export

- [ ] terreno in collection `terrain`, angolo a (0,0)
- [ ] palazzi in `statics` (box semplici; `nav="none"` dove serve)
- [ ] almeno uno `spawn` e un `goal`, dentro `[0,W]×[0,H]`
- [ ] `cost` ha `weight`; `wall` spessi ≥ 1 cella; niente rect ruotati
- [ ] prop con chiavi presenti nel catalogo
- [ ] roba di lavoro prefissata con `_`

# EDITOR_PLAN — piano di completamento dell'editor di livelli

Piano operativo per portare la modalità EDIT di `vat_horde` a EDITOR COMPLETO
con cui autorare i livelli del gioco. Compagno di `GAME_PLAN.md` (M9): il
gioco definisce le entità (`mission`, `exit`, `lz`…), l'editor le fa
disegnare. Il DESIGN resta `EDITOR_DESIGN.md` (principi, formato, pipeline
terreno §9-§10 — già implementata); questo file è l'AGENDA: stato vero del
codice, fasi, verifiche. Sintesi in `TODO.md` §M7.

Metodo e vincoli: identici a `GAME_PLAN.md` §1 (design→test headless→
vat_horde→verifica visiva→commit; core intoccato; librerie leggere).

## 0. Stato vero del codice (luglio 2026) — cosa NON rifare

FATTO e verificato (dettaglio in `EDITOR_DESIGN.md` §6/§9):
- **Picking** `vat/edit_pick.h` (`pick_y0`, `test_pick`), finestra
  ridimensionabile + F11.
- **Modalità EDIT** in `vat_horde` (TAB / `VAT_HORDE_EDIT=1`): sim ferma,
  overlay rect color-codate + poly in costruzione + cursore, tool a
  tastiera 1-8, snap cella (G), `[`/`]` parametri, F2 salva.
- **Scene = unica verità** + re-instantiate su Play (`build_world`/
  `free_world`); logica di mutazione PURA in `vat/editor.h`
  (`test_editor`): drag→rect, poly close/reject, delete-at, snap,
  place-prop, roundtrip save.
- **Pipeline terreno COMPLETA** (§9): bake `.zhm`/ZHM2 con buchi-palazzo,
  loader glb (cgltf) terreno+statici, seating di sprite/strutture/ombre/
  volo su `terrain_z`, veto `ter_blocked`.
- **Prop di decoro** (§10 5b): catalogo `props/catalog.txt`, tool `ED_PROP`
  (piazza/ruota/cicla/cancella), placeholder procedurali, distruttibili
  (`destruct.c`).
- **Formato scena** già oltre il design originale: `wall <hp> <cost_mult>
  x y w h` (struttura distruttibile RECT, host-applied — ha SOSTITUITO il
  `poly hp core` proposto in EDITOR_DESIGN §2) e `turret x y [range]
  [heavy]`. Applicati da `build_world`, usati dalle mappe-banco `arena_*`.

MANCA (resto di questo piano):
- fase 1 editor: select/move/resize (oggi `ED_SELECT` non fa nulla:
  solo create + delete-at), HUD a schermo (oggi nel titolo finestra),
  griglia visiva, editing dei `set`/`world`/`cell` da editor.
- tool per le entità gameplay esistenti (`wall`, `turret`) e future
  (`exit`, `lz`, `mission`, `budget` — arrivano con GAME_PLAN fase A).
- UI vera (pannelli proprietà), undo, validazione, arte prop (`.glb`).

## 1. Intreccio con GAME_PLAN (chi definisce cosa)

Regola: **il formato `.scn` lo estende chi implementa la semantica**
(GAME_PLAN fase A aggiunge `mission`/`exit`/`lz` insieme a `mission.c`);
l'editor aggiunge il TOOL nella fase successiva. Mai il contrario: un tool
che scrive righe che nessuno consuma non è verificabile.

Sequenza consigliata (intervallata, non in blocco):
```
E1 (usabilità base)  →  GAME_PLAN A (mission.c)  →  E2 (tool gameplay)
→  GAME_PLAN B..D (giocando coi livelli fatti in E2)  →  E3 (UI nuklear,
condivisa con GAME_PLAN G)  →  E4 (undo/validazione)  →  E5 (arte prop)
```
E1 è indipendente e si può fare SUBITO; E2 richiede GAME_PLAN A.

---

## Fase E1 — Completare la fase-1 (usabilità terreno)

**Obiettivo**: chiudere il debito della fase 1: selezionare/muovere/
ridimensionare ciò che si è disegnato, e leggere lo stato a schermo.

**Costruire** (tutto in `vat/editor.h` + host; zero API core):
- **Select/Move/Resize** (`ED_SELECT`, tool 1 — oggi vuoto):
  - hit-test unificato `ed_pick_entity(sc, x, y)` → `{tipo, indice}`
    (l'ordine di priorità: prop → turret/wall → rect → poly; il
    point-in-poly e il delete-at esistono già, fattorizzarli).
  - click = seleziona (highlight nell'overlay: bordo più chiaro);
    drag = MUOVE l'entità (delta snappato); per i rect, drag su un
    ANGOLO (raggio di presa ~0.5 m) = resize; `Del`/`X` = cancella la
    selezione (oggi RMB cancella "sotto il cursore", resta come scorciatoia).
  - entità selezionata → i tasti parametro (`[`/`]`, `+`/`-`) editano LEI
    (peso cost, altezza poly, hp/mult wall…) invece dei default del tool.
  - i vertici dei poly si spostano singolarmente SOLO se selezionato un
    poly (drag sul vertice, raggio di presa); niente insert/delete vertice
    in E1 (annotato in coda).
- **HUD a schermo con font bitmap**: `stb_easy_font.h` (single-header,
  coerente con stb già in `vat/`; il download lo fa l'UTENTE) → quad ASCII
  nel flat shader, proiezione screen-space. Righe: tool corrente, snap
  on/off, coordinate cursore, parametri del tool/selezione, messaggi
  (es. "salvato"). Sostituisce l'HUD-nel-titolo in EDIT; in PLAY il titolo
  resta finché GAME_PLAN G non porta la UI di gioco.
- **Griglia visiva**: linee della cella nav nel viewport (solo in EDIT,
  decimate ai tier di zoom bassi), + evidenza della cella sotto il cursore
  quando lo snap è attivo.
- **Editing `set`/`world`/`cell`**: v1 MINIMA — lista dei `set` di scena
  navigabile a tasti (mostra chiave=valore, `[`/`]` modifica);
  `world`/`cell` NON si editano da UI (si cambiano nel file: ridimensionare
  il mondo invalida raster e terreno — rimandato, annotato in coda).

**Verifica**: `test_editor` esteso (hit-test per ogni tipo con priorità,
move con snap, resize angoli con normalizzazione, edit-parametro sulla
selezione, roundtrip dopo move/resize). HUD/griglia = verifica visiva
(una riga su cosa guardare, poi all'utente).

## Fase E2 — Tool per le entità gameplay

**Prerequisito**: GAME_PLAN fase A committata (`mission.c` + `exit`/`lz`/
`mission`/`budget` nel formato scena, col suo roundtrip in `test_scene`).

**Costruire**:
- Tool nuovi (tasti 9, 0, …; o secondo anello di tool con Shift):
  - `ED_WALLRECT` — muro distruttibile: drag rect come i cost, campi
    `hp`/`cost_mult` editabili da selezione (l'entità `wall` esiste già).
  - `ED_TURRET` — click piazza, `[`/`]` range, `H` heavy; overlay: disco
    range + arco. (Torrette FISSE di livello; il piazzamento del giocatore
    è place.c, altra cosa.)
  - `ED_EXIT` — drag rect + campi script (`rate`, `delay`, `pool`) sulla
    selezione; overlay color-codato con etichetta dei parametri.
  - `ED_LZ` — click piazza il punto di atterraggio (uno solo per scena:
    ri-piazzare lo SPOSTA); overlay elicottero stilizzato.
  - `mission`/`budget` — non sono entità spaziali: si editano nella lista
    `set`-like di E1 (kind/durata/prep/budget).
- `ed_pick_entity` e delete/move di E1 si estendono ai tipi nuovi
  (turret/exit/lz sono punti/rect: nessun caso nuovo di geometria).
- **Playtest in-editor del loop di missione**: TAB → PLAY parte in PREP
  con la missione della scena (già gratis: `build_world` deriva tutto
  dalla Scene). Tornare in EDIT = drop del sim (già così).

**Verifica**: `test_editor` esteso ai tool nuovi (place/hit/move/delete/
param + roundtrip); `test_scene` copre già il formato (fatto in GAME_PLAN
A). Visiva: costruire una mini-missione completa NELL'EDITOR (LZ + 2 exit
scriptate + mura + torrette), TAB e giocarla.

## Fase E3 — UI: pannelli con nuklear (condivisa con GAME_PLAN G)

**Decisione da confermare con l'utente**: `EDITOR_DESIGN.md` §5 diceva
"cimgui in fase 2", ma la preferenza REGISTRATA più recente (CLAUDE.md,
"librerie leggere") dice **nuklear/microui, NON cimgui** (toolchain C++).
Questo piano assume **nuklear**, una sola integrazione per editor E gioco
(GAME_PLAN fase G): stesso context, due set di pannelli. Il download del
single-header lo fa l'utente.

**Costruire**:
- integrazione nuklear nel GL context di `vat_horde` (render backend
  minimale sul flat shader o il backend GL3 ufficiale; input SDL3 già
  in loop);
- **pannello proprietà** dell'entità selezionata (campi numerici veri:
  script delle exit, hp/mult, mission) — sostituisce l'editing a tasti
  `[`/`]` dove i campi sono >2;
- **palette tool + prop** (bottoni al posto dei tasti-numero; la palette
  prop mostra il catalogo con label);
- **lista `set`/mission** come form.
- I tasti restano come scorciatoie: la UI è additiva, la logica resta in
  `editor.h` (i pannelli chiamano le stesse funzioni di mutazione — la
  testabilità headless non cambia).

**Verifica**: la logica resta coperta da `test_editor` (la UI chiama le
stesse mutazioni); la UI in sé è verifica visiva.

## Fase E4 — Robustezza: undo, validazione, QoL

**Costruire**:
- **Undo/redo**: stack di snapshot dell'intera `Scene` (è una struct
  piatta di dimensione fissa — memcpy, ~decine di KB; cap ~64 livelli,
  push a fine mutazione). Redo = stack gemello. `Ctrl+Z`/`Ctrl+Y`.
  Test: sequenza di mutazioni + undo totale = scena bit-uguale all'inizio.
- **Validazione** (bottone/auto al save, esiti a HUD):
  - goal raggiungibile da ogni spawn/exit: flood-fill sulla nav derivata
    (il `phi` del core lo dà gratis: instantiate di prova + check `phi`
    finito nelle celle spawn);
  - spawn/exit/LZ non dentro un muro/statico (`ter_blocked` + `solid`);
  - se c'è `mission`, c'è la LZ (il core della sconfitta);
  - warn non bloccanti (l'utente può salvare comunque).
- **QoL** (a scelta dell'utente, in ordine di resa): duplica entità
  selezionata (`Ctrl+D`), nudge a frecce, contatori entità a HUD,
  `R` in PLAY = re-instantiate rapido (replay della missione senza
  passare da EDIT).

**Verifica**: `test_editor` per undo (bit-equality) e per la validazione
(scene sintetiche: goal murato → warn; spawn nel muro → warn; scena sana
→ zero warn). QoL = visiva.

## Fase E5 — Arte: prop `.glb` e rifiniture di catalogo

**Costruire** (quando l'utente produce l'arte in Blender):
- loader mesh prop: il catalogo ha GIÀ il campo `mesh` (path `.glb`);
  caricare via cgltf (riuso del loader statici) le mesh dei tipi usati,
  render per-istanza al posto del placeholder procedurale (fallback:
  placeholder se il file manca — già il comportamento del formato).
  Attenzione ai prop DISTRUTTIBILI: il topple attuale inclina i box
  procedurali (`prop_box_lean`) — per le mesh vere basta la stessa
  rotazione rigida attorno alla base come matrice modello.
- palette prop con anteprima (E3) e, se il playtest li chiede, gli stati
  di danno del catalogo (`EDITOR_DESIGN` §10 — mesh alternative a soglie
  di hp; oggi non servono: i prop sono decoro one-shot).

**Verifica**: visiva (è arte) + `test_props` per le estensioni di formato
del catalogo.

## Questioni aperte / rimandate deliberatamente

1. **Resize di `world`/`cell` da editor**: invalida raster, terreno baked
   e coordinate; farlo bene = migrazione della scena. Rimandato finché un
   caso reale non lo chiede (workaround: si edita il file a mano).
2. **Insert/delete di singoli vertici poly** (E1 fa solo move): utile per
   ritoccare footprint complessi; aggiungere se il level design reale lo
   invoca.
3. **Multi-selezione / selezione ad area**: solo se le mappe diventano
   grandi da gestire; l'undo di E4 riduce il bisogno.
4. **cimgui vs nuklear**: assunto nuklear (vedi E3) — CONFERMARE con
   l'utente prima di E3; se cambia idea, la logica in `editor.h` non
   cambia (solo il layer pannelli).
5. **`exit` vs `spawn` esteso** (aperta storica di EDITOR_DESIGN §8): si
   decide in GAME_PLAN fase A, l'editor si adegua in E2.

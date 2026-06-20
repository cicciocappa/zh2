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

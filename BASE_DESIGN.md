# La base: consegna, difesa, estrazione — design tecnico

> **STATO: IMPLEMENTATO (2026-07-08), verifica visiva utente in corso.**
> Fasi 1a/1b (footprint orientato + box container + arco mortaio) committate
> in precedenza; questa passata aggiunge: stati `APP_DEPLOY`/`APP_EXTRACT` in
> `app.c` (skippabili con INVIO, testati in `test_app`), cinematiche chinook
> (`assets/models/chinook.glb`, nodi `helicopter_body`/`rotor1`/`rotor2`/
> `cable` — il cavo si srotola scalando la Y attorno alla sua ancora in cima,
> i rotori girano attorno all'asse misurato dalla normale media, disegnati in
> 2 copie sfasate di 90° come trucco anti-strobo), modello base
> `assets/models/base_and_mortar.glb` (nodi `container`/`mortar_stand`/
> `mortar_carriage`: lo stand ruota sull'azimut di mira, il tubo — verticale a
> riposo — si inclina all'alzo balistico `atan(4·apex/d)`, slew limitato nel
> fixed loop), gittata min/max del mortaio (default 12/90 m, env
> `VAT_HORDE_MORTAR_MIN/MAX`; click fuori gittata rifiutato, X grigia, anelli
> di gittata in aiming) e colpo che parte dalla BOCCA del tubo. Il container
> appeso al cavo è stampato da `build_struct_mesh` (`gLzHeld`); senza glb
> restano i placeholder (box arancio) e le cinematiche si saltano da sole.
> Banco di prova: `./game heli_test_campaign.txt` (mission_demo ha la `lz`;
> la campagna normale non ce l'ha ancora → DEPLOY/EXTRACT auto-skip).
> Evolve il concetto DEPLOY di GAME_PLAN §1
> ("l'elicottero È il core") e sostituisce la nota "elicottero = fiction futura"
> di `build_lz_core`. Nessuna API core nuova: la base è già una struttura
> assediabile + goal; le cinematiche sono fiction renderer-side; il gating di
> fase vive nella shell (`app.c`).

## 1. Principio

La **base** è un **container** consegnato da un **elicottero** al punto `lz`
della scena. È tre cose in una:

1. **il TARGET dell'orda** — goal del flow field + struttura assediabile con HP;
2. **il PUNTO DI PARTENZA degli attacchi speciali** (mortaio oggi, altri poi):
   il proiettile parte dalla base verso il bersaglio;
3. **la condizione di SCONFITTA** — HP a zero (l'orda sfonda il container) =
   partita persa.

Differenza con GAME_PLAN §1: lì l'elicottero È il core. Qui l'elicottero è il
**veicolo transitorio** (consegna all'inizio, estrazione alla vittoria) e il
**container è la base persistente** che resta in campo per tutto il livello.
Narrativamente: ti calano una base operativa, la difendi, se reggi te la
vengono a riprendere.

## 2. Cosa esiste già (inventario)

| Pezzo | Dove | Stato |
|---|---|---|
| Punto base nel `.scn` | `lz x y` (scene.c) | FATTO |
| Base = struttura assediabile + goal | `build_lz_core` in vat_horde: anello 5×5 `is_core`, goal 3×3, HP `VAT_HORDE_LZ_HP` (1500), `gLzCore` | FATTO (ma resa come box-muri, nessun modello) |
| Sconfitta al crollo | `def_lost` (core `is_core` → LOST in ogni stato) | FATTO |
| Macchina di missione | `mission.c`: PREP → ASSAULT → WON/LOST | FATTO |
| Flusso applicativo | `app.c`: TITLE → MENU → BRIEFING → PREP → ASSAULT → DEBRIEF | FATTO |
| Attacco mortaio | `host_blast` + pool strike in vat_horde | FATTO (v0: parte dal punto X, NESSUNA origine base — placeholder in attesa di QUESTO doc) |
| Rendering strutture (danno/crollo) | `build_struct_mesh` (scurimento col danno, sparizione al crollo) | FATTO |
| Pipeline modelli 3D | glb flat-color (`load_prop_models`), VAT animato (zombie), torrette a nodi (`base`/`gun`) | FATTO — riusabile per heli/container |
| Barre HP world-space | **SCARTATE** (2026-07-08: ingombranti alla scala del gioco) → sostituite dall'**hover inspect**: tooltip screen-space al cursore (nome + mini-barra HP) sull'elemento sotto il mouse — torrette, muri, barriere, prop assediabili, base. `hover_resolve`/`hover_tooltip_draw` in vat_horde | FATTO |
| Asset elicottero / container / cavo | — | **DA CREARE (utente)**; placeholder procedurali per la meccanica |

## 3. La base come entità

- **Posizione e orientamento**: `lz x y [yaw]` della scena — direttiva estesa
  con un angolo opzionale (default 0). Una sola base per livello. Il container
  arriva **nudo**: nessuna cinta di mura/cancelli automatica — è il giocatore, se
  vuole, a circondarlo di barriere in PREP.
- **Dimensione (fissa)**: il container è un oggetto reale unico → **6,1 × 2,44 m**
  di footprint (container ISO 20", carico a gancio tipico di un Chinook),
  ~**12×5 celle** a 0,5 m. Solo posizione+orientamento variano per livello; la
  taglia no. (Rimpiazza il 5×5 assi-allineato di `build_lz_core`.)
- **Modello**: un **container** glb (nuovo asset). Placeholder = box 6,1×2,44×2,6 m
  colorato. Le celle restano la collisione/assedio; il container è la mesh sopra.
- **Footprint e assedio**: un **rettangolo ruotato** dello `yaw`, rasterizzato in
  celle assediabili con `scene_raster_cells` (la stessa via dei prop solidi in
  `prop_world.c`) invece dell'anello quadrato. L'orda vi converge (goal), preme i
  bordi (sensore d'assedio → attacchi discreti su HP), il container **si scurisce
  col danno** come le strutture e, ad HP 0, **crolla → partita persa** (`def_lost`).
- **HP**: dal livello (già `core_hp` nel campaign / `VAT_HORDE_LZ_HP`). Con
  **barra HP world-space** sopra il container (quad flat, non nuklear).
- **Origine attacchi speciali (da implementare ORA)**: il mortaio (e futuri)
  partono da `(lz_x,lz_y)`: il proiettile **parte dal container** con un arco e
  cade sul punto mirato (sostituisce il placeholder "appare sul bersaglio" del
  colpo v0). Se la base è distrutta la partita è già persa → nessun caso limite.

## 4. Le cinematiche dell'elicottero (fiction)

Due sequenze, speculari. Traiettoria e tempi sono **stato host** (interpolazione
renderer-side), non animazioni dentro il modello — solo il rotore gira nel glb.

### 4.1 Intro — CONSEGNA (inizio livello, prima della frase di preparazione)

1. l'elicottero entra da un bordo mappa portando il container appeso al cavo;
2. vola fino sopra il punto `lz`, si **abbassa**;
3. **depone il container** (cavo sganciato) sul punto designato;
4. **risale** e **vola via** oltre il bordo opposto.

A sequenza finita → la base è "viva", parte la PREP (frase preparatoria +
fortificazione). Durante la consegna: **niente orda**, e il placement è
congelato (vedi §7, decisione).

### 4.2 Outro — ESTRAZIONE (a vittoria, dopo l'assalto vinto)

1. l'elicottero rientra, scende sopra il container;
2. **aggancia col cavo** il container;
3. **solleva** e **vola via** portandolo oltre il bordo.

A sequenza finita → DEBRIEF (esito, progressi). Se il livello è **perso** (base
a 0) non c'è estrazione: crollo del container → LOST → DEBRIEF.

## 5. Modelli, animazione, asset

- **Elicottero**: glb con rotore animabile (giro continuo). La discesa/risalita
  e il volo sono traiettoria host. Placeholder = corpo box + due quad rotanti.
- **Container**: glb statico. Placeholder = box.
- **Cavo**: segmento (linea spessa / cilindro sottile) fra il ventre
  dell'elicottero e il tetto del container nelle fasi di aggancio/deposito.
- Sono tutti **fiction**: nessun test deterministico sulla grafica (verifica
  visiva). Testabili invece le **transizioni di stato** e il **gating** (§9).

## 6. Integrazione e API

- **Nessuna API core nuova.** La base è già `def_add_structure` (assediabile,
  `is_core`) + `simp_set_goal`; l'origine strike è un semplice `(x,y)` host.
- **Dove vivono gli stati cinematica** (deciso §8.1): **shell `app.c`** — nuovi
  stati `APP_DEPLOY` (fra BRIEFING e PREP) e `APP_EXTRACT` (fra ASSAULT-vinto e
  DEBRIEF), con `APP_ACT_*` corrispondenti. La missione resta congelata come già
  in APP_PREP; la cinematica è pura presentazione. Entrambi skippabili
  (`APP_IN_CONFIRM`/click salta e passa allo stato successivo).
- **Rendering**: un modulo host (es. `heli` in vat_horde) tiene lo stato della
  sequenza (fase, t, posizioni) e disegna heli+container+cavo; la base-container
  statica la disegna il renderer strutture/prop.

## 7. Timeline di un livello

```
MENU
 └─ BRIEFING (testo del livello)         [conferma]
     └─ DEPLOY  (heli consegna il container, ~N s)   ← nuovo
         └─ PREP    (frase prep + fortificazione)     [VIA ALL'ORDA]
             └─ ASSAULT (ondate; l'orda punta la base)
                 ├─ VINTA → EXTRACT (heli preleva il container, ~N s)  ← nuovo
                 │            └─ DEBRIEF (esito + progressi)
                 └─ PERSA (base a 0) → LOST → DEBRIEF
```

## 8. Decisioni (sciolte con l'utente, 2026-07-07)

1. **Stati cinematica** → **shell `app.c`** (`APP_DEPLOY`/`APP_EXTRACT`).
2. **Cinematiche skippabili** → **sì**, entrambe (tasto/click salta la sequenza).
3. **Placement durante il DEPLOY** → **no**: bloccato finché l'elicottero ha
   finito; la PREP (e il piazzamento) iniziano dopo.
4. **Cinta protettiva iniziale** → **no**: il container arriva nudo, è il
   giocatore a fortificarlo volendo.
5. **Footprint** → rettangolo **6,1 × 2,44 m** (container 20", §3), **orientabile**
   via `lz x y [yaw]`. Taglia fissa, solo posizione+yaw per livello.
6. **Origine mortaio dalla base** → **implementata subito** (arco dal container
   al bersaglio).

Ancora aperte (le sciogliamo quando arriviamo al pezzo, non bloccano lo scheletro):

- **Durata** esatta delle due sequenze (si tara a occhio).
- **HP/feedback**: valore di default e resa del danno (scurimento? fumo? crepe?).
- **Riparazione** base con biomassa (fase E) — probabilmente fuori scope qui.
- **Elicottero vulnerabile**? (bozza: no — orda assente/ferma, heli intoccabile).
- **Placeholder** procedurali con cui partire (box container + box heli + due
  quad-rotore + segmento-cavo) prima degli asset dell'utente.

## 9. Piano di verifica

- **Cinematiche**: verifica **visiva** (fiction). Placeholder box → poi asset.
- **Stati e gating** (testabile, deterministico): l'orda NON parte prima
  dell'ASSAULT; il placement è chiuso durante il DEPLOY; la base è il goal e la
  sconfitta scatta ad HP 0; la sequenza di stati (DEPLOY→PREP→ASSAULT→
  EXTRACT/LOST→DEBRIEF) è corretta. Estende `test_mission.c` o un nuovo
  `test_app`/`test_base` a seconda della decisione §8.1.
- **Origine strike dalla base**: se implementata, un test che il colpo parte da
  `(lz_x,lz_y)` e impatta il bersaglio al tempo atteso (estende `test_strikes`
  di fase F).

## 10. Fuori scope (per doc futuri)

- Estetica/varianti dell'elicottero, suoni dedicati, camera cinematografica.
- Meta-progressione fra livelli (la base che "cresce" di campagna in campagna).
- Multi-base / basi mobili.

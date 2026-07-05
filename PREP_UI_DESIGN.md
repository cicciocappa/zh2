# PREP_UI_DESIGN — interfaccia della fase di preparazione

> Figlio di `PLACEMENT_DESIGN.md` (che dichiarava la "UI grafica ricca" fuori
> scope) e di `GAME_APP_DESIGN.md` (stato APP_PREP). DECISO 2026-07-05.
> Scope: SOLO il target `game` (-DGAME_SHELL). La sandbox `vat_horde` tiene
> l'interfaccia attuale a tasti (`P`, `[`/`]`, `,`/`.`) — è uno strumento di
> lavoro, non ha bisogno della barra.

## 1. Obiettivo

La fase PREP oggi funziona (piazzamento, budget, validazione ghost) ma è
invisibile: lista piatta ciclata con `[`/`]`, feedback nella title bar. Serve
un'interfaccia da GIOCO: il giocatore vede COSA può piazzare, QUANTO costa,
quanto budget resta, e passa all'assalto con un gesto chiaro.

Decisioni prese (2026-07-05, con l'utente):
- **Barra in basso** stile RTS (riferimento WC2, coerente con GFX_DESIGN §1 e
  con la barra di fase già esistente in alto).
- **3 tab: TORRETTE / BARRIERE / TRAPPOLE** = filtro su `PlKind`. La tab non è
  una struttura nuova: il catalogo resta piatto, la barra mostra solo le voci
  del kind attivo.
- **Cassonetto e Auto NON sono piazzabili dal giocatore**: escono dal catalogo
  del game e diventano entità di livello (v. §9). `PL_BIN`/`PL_CAR` restano in
  `place.c` (il catalogo è del chiamante: la sandbox può tenerli).
- **Trappole = tab prevista dal design, si popola con la MINA** quando arriva
  `def_blast` (EXPLOSION_DESIGN, prossima in roadmap). Finché il catalogo non
  ha voci `PL_TRAP` la tab si disegna disabilitata ("PRESTO") — la UI non
  aspetta l'esplosione per nascere.
- **Barriere tracciate a LINEA** come negli RTS (§5) — assorbe la fase B di
  GAME_PLAN (`PL_WALL_LINE`).
- **Undo** dei piazzamenti in PREP (§6).

## 2. Catalogo v1 del game

Una voce = una riga di `PL_CAT` + i parametri di commit. I mattoni esistono
tutti: `DefTurret.heavy`, `simp_set_opacity` (asse C), `def_add_structure`.

| tab       | voce        | costo  | note                                        |
|-----------|-------------|--------|---------------------------------------------|
| TORRETTE  | Leggera     | 100    | l'attuale: range 40, period 0.12, dmg 40    |
| TORRETTE  | Pesante     | 250    | `heavy=1`: colpo lento che gibba (defense)  |
| BARRIERE  | Barricata   | ~12/m  | a LINEA (§5): 300 HP per modulo 2.5 m       |
| BARRIERE  | Cancellata  | ~20/m  | a LINEA; **opacità 0.3**: blocca gli zombie,|
|           |             |        | le torrette sparano ATTRAVERSO (asse C,     |
|           |             |        | test_cover). Wiring = quello di prop_world: |
|           |             |        | struttura + set_opacity celle, crollo →     |
|           |             |        | opacità 0 (già gestito)                     |
| TRAPPOLE  | Mina        | 40     | FUTURA: def_blast one-shot su trigger di    |
|           |             |        | prossimità (EXPLOSION_DESIGN §mine fase D)  |

Le barriere sono STRUMENTI A LINEA (§5), prezzate al metro (costo per
modulo, proporzionale alla lunghezza): il giocatore non sceglie la lunghezza
del pezzo, traccia una linea e il fill a moduli è interno.

I parametri di commit oggi hardcoded in `commit_turret` (range, period,
damage) migrano in `PlItem`: campi nuovi `heavy`, `range`, `fire_period`,
`damage`, `opacity` (default 1 = solido pieno). Una voce nuova = una riga,
zero casi speciali — regola di PLACEMENT_DESIGN invariata.

**Nome/descrizione/icona: v1 tutte PLACEHOLDER** (deciso 2026-07-05): card
con nome + costo in font8 e un'icona segnaposto (quadratino colorato o sprite
provvisorio). La pipeline icone vera arriva con gli asset UI.

## 3. Layout della barra

Disegnata da `shell_build_ui` con i mattoni esistenti (quad 2D + font8),
zero dipendenze nuove. Altezza fissa ~110 px, larghezza piena, sfondo scuro
semi-trasparente sopra la mappa.

```
├──────────────────────────────────────────────────────────────────┤
│ [1 TORRETTE]  [2 BARRIERE]  [3 TRAPPOLE]   ↶    budget: 450 $    │
│ ┌────────┐ ┌────────┐                                            │
│ │Leggera │ │Pesante │     Torretta leggera — 100$                │
│ │  100$  │ │  250$  │     gittata 40 m, 8.3 colpi/s              │
│ └────────┘ └────────┘             [INVIO] VIA ALL'ORDA           │
├──────────────────────────────────────────────────────────────────┘
```

- **Riga tab**: la tab attiva evidenziata (bordo/colore); TRAPPOLE grigia con
  "PRESTO" finché non ha voci. Budget a destra, sempre visibile (rosso quando
  la voce selezionata non è affordabile). Bottone ↶ = undo (§6).
- **Card voce**: icona placeholder + nome + costo. Selezionata = bordo acceso;
  non affordabile = testo spento. Click = seleziona (e attiva il piazzamento).
- **Pannello info**: voce selezionata con 1-2 stat leggibili (gittata e rateo
  per le torrette, HP/m per le barriere; "spara-attraverso" per la cancellata).
- **VIA ALL'ORDA**: bottone cliccabile = `APP_IN_CONFIRM` (equivale a INVIO).
  È l'unico punto che fa partire l'assalto: gesto chiaro, niente INVIO
  accidentale — v. questione aperta Q3.

La barra esiste SOLO in `APP_PREP` (v1: piazzamento durante l'assalto chiuso,
come oggi via `mission_placement_open`).

## 4. Input

- **Mouse**: hit test PRIMA del mondo — se il click cade dentro la barra è
  UI (tab/card/bottone), MAI un commit di piazzamento (il bug classico).
  Sopra la barra: LMB = `pl_commit` (o drag di linea per le barriere, §5),
  RMB = deseleziona (ghost off).
- **Tastiera**: `1`/`2`/`3` = tab; `[`/`]` = voce prec/succ NELLA tab;
  `,`/`.` = rotazione (torrette n/a, resta per coerenza sandbox);
  `Ctrl+Z` = undo; `INVIO` = via all'orda; `ESC` = menu (già così in app.c).
- Cambiare tab seleziona la prima voce affordabile della tab (o la prima).

## 5. Barriere a LINEA (deciso 2026-07-05; assorbe GAME_PLAN fase B)

Il giocatore traccia una linea sul suolo (drag: click = ancora, rilascio =
fine), come negli RTS. Il problema del residuo ("linea da 11 m, moduli da
2.5 m: e il metro che avanza?") si scioglie osservando che il motore è GIÀ
quantizzato: collisione e nav lavorano per cella da **0.5 m** (13 scene su
14; l'agente default ha raggio 0.30 m → **diametro 0.6 m**). Decisioni:

- **Snap di lunghezza a 0.5 m, angolo LIBERO.** La linea è world-space, non
  vincolata agli assi: la rasterizzazione in celle di un rettangolo ruotato
  esiste già (`scene_raster_cells`, la usa prop_world per i prop a yaw
  arbitrario).
- **Fill a moduli, greedy, interno**: la linea si riempie con moduli da
  2.5 m + il residuo con moduli da 1 m e 0.5 m (es. 11 m = 4×2.5 + 1×1).
  Il giocatore non li sceglie: seleziona "Barricata" e disegna. Ogni
  lunghezza ha la sua mesh (v1: box flat-color per modulo, ruotato lungo la
  linea — placeholder come tutto il resto).
- **Un modulo = una struttura** (`def_add_structure`): HP e crollo per
  modulo → l'orda apre una BRECCIA locale, non demolisce la linea intera.
  Con moduli minimi da 0.5 m il STRUCT_CAP (192) regge mura lunghe
  realistiche (11 m = 5 strutture).
- **Niente compenetrazione**: la validazione resta PER-CELLA (statici, prop
  solidi, muri esistenti = veto). Il muro non "entra nell'autobus": si
  ferma alla cella prima. La granularità 0.5 m garantisce di poter arrivare
  A FILO di qualsiasi ostacolo — che è ciò che rende accettabile il veto.
- **Chiudere i varchi è garantito dalla fisica**: un buco residuo da 0.5 m
  è più stretto del diametro agente (0.6 m) — nessuno zombie ci passa. E il
  caso patologico "varco nav-aperto ma fisicamente intransitabile che
  calamita l'orda" è già gestito dal campo `jam` (M3.7): la coda ferma si
  prezza fuori mercato e il Dijkstra devia. Da VERIFICARE nel banco (§10).
- **Commit tutto-o-niente (v1)**: se una cella della linea è vietata o il
  budget non copre, il ghost mostra il tratto rosso e il commit rifiuta.
  Piazzamento parziale ("metti quello che ci sta") = questione aperta Q4.
- **Ghost di linea**: durante il drag si vedono i moduli previsti + costo
  totale live accanto al cursore; verde/rosso per validità.

Trade-off ONESTO da giudicare a occhio: con angoli liberi la collisione è
la scala di celle rasterizzate, la mesh è dritta lungo la linea → mismatch
visivo/fisico fino a ~mezza cella (0.25 m) sui bordi. Standard nei giochi
con nav a griglia; se a 32 px/m disturba, si ripiega su snap d'angolo a 45°.

## 6. Undo (deciso 2026-07-05)

Piazzare per sbaglio non deve costare: **stack di undo, solo in PREP**.
Ogni commit spinge un record (tipo, id struttura/torretta, celle, costo; una
LINEA intera = UN record, l'undo toglie tutta la linea); `Ctrl+Z` (e il
bottone ↶ in barra) fa pop: rimborso PIENO, rimozione dell'entità, celle e
opacità ripristinate. Lo stack si SVUOTA al via dell'assalto (via = impegno).
- Strutture: serve un path di rimozione pulita (il crollo esiste ma genera
  detriti/fiction); riuso del teardown senza debris.
- Torrette: MANCA `def_remove_turret` in defense (c'è solo il disable) —
  piccola API nuova, swap-and-pop nell'array torrette (attenti agli id nei
  record di undo: id stabili o fixup).
- I record referenziano SOLO roba piazzata in questa PREP: niente undo di
  entità di livello.

## 7. Architettura

- **Stato UI shell-side**: tab attiva, hover, rect delle card (per l'hit
  test), stack di undo e drag di linea vivono in vat_horde `#ifdef GAME_SHELL`,
  NON in `place.c`, che resta logica pura (sessione+validazione+commit).
- **API nuove in place.c** (minime):
  - `pl_select(p, idx)` — selezione diretta per indice (oggi solo `pl_cycle`);
    la barra seleziona per click, il cycle della sandbox resta.
  - `PlItem` esteso coi campi di commit (§2). `commit_turret`/`commit_barricade`
    leggono dalla voce invece che dalle costanti.
  - la LINEA (§5): validate/commit di un segmento world-space (ancora+fine)
    che internamente rasterizza e riempie a moduli — stessa coppia
    validate/commit del punto singolo, così test_place la copre headless.
  - enum: `PL_TRAP` sostituisce il commento `/* PL_TRAP… */` (commit = no-op
    finché non c'è def_blast; nessuna voce lo usa in v1).
- **Filtro tab**: la shell costruisce, una volta a `pl_init`, l'indice
  `tab→[voci]` scandendo il catalogo per kind. Niente duplicazione del dato.

## 8. Ghost migliorato (QoL, stessa passata)

- Torretta selezionata → **cerchio di gittata** attorno al ghost (usa
  `it->range`), verde/rosso con la validità. È il singolo feedback più utile
  per decidere DOVE mettere una torretta; oggi il ghost è solo un cerchietto
  di footprint.
- Barriera → ghost di linea a moduli (§5) con costo live.

## 9. Draggable come entità di livello (prerequisito separato, PICCOLO)

Decisione utente: cassonetti/auto li piazza il level designer. Serve
un'entità di scena: `drag bin|car x y [rot]` nel formato `.scn` (+ prefisso
in BLENDER_LEVEL/export_scn.py), applicata dall'host in `build_world` come
già wall/turret (i draggable sono GAME-side: `simp_drag_add`). Il commit
`PL_BIN`/`PL_CAR` di place.c resta per sandbox/test. È un mini-lavoro
indipendente dalla barra: si può fare prima o dopo.

## 10. Ordine di implementazione

1. `place.c`: `PL_TRAP` nell'enum, `pl_select`, `PlItem` esteso, commit
   data-driven, cancellata (opacità al commit + crollo). Test: `test_place`
   esteso (cancellata → transmit > 0 sulle sue celle, heavy flag arriva alla
   torretta, select diretto).
2. `place.c`: linea (§5) — raster + fill a moduli + validate/commit
   tutto-o-niente. Test headless: 11 m → 4×2.5+1×1, veto su ostacolo, costo
   esatto, determinismo; banco visivo col varco da 0.5 m (il jam devia?).
3. defense: `def_remove_turret` + teardown struttura senza debris → undo (§6).
   Test: piazza/undo N volte = budget e mondo bit-identici allo stato iniziale.
4. Shell: barra (render + hit test + tasti + undo). Verifica VISIVA
   dall'utente (preferenza nota: niente screenshot-diff per l'estetica).
5. Ghost gittata torrette + ghost di linea.
6. Entità `drag` di scena (§9).
7. Mina: quando EXPLOSION_DESIGN consegna `def_blast` (tab si popola da sola).

## 11. Questioni aperte

- **Q1 vendita/refund**: click su un pezzo piazzato → rimborso (parziale?).
  Utile, ma serve il picking delle strutture; v2. (L'undo §6 copre intanto
  l'errore immediato.)
- **Q2 piazzamento durante l'assalto**: oggi chiuso. Se un giorno si apre
  (a costo maggiorato?), la barra è già lì — decisione di gameplay, non di UI.
- **Q3 conferma VIA**: partire per sbaglio è costoso (PREP illimitata persa).
  v1: il bottone basta? o doppio INVIO / hold? Da provare a mano.
- **Q4 commit parziale della linea**: "metti quello che ci sta fino
  all'ostacolo" invece del tutto-o-niente. Più comodo, meno prevedibile.
  Decidere provando il tutto-o-niente.

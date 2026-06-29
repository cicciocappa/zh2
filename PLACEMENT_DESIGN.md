# PLACEMENT_DESIGN — piazzamento a runtime dal giocatore

> Stato: DESIGN (2026-06-29, branch `opus`). Sistema di gioco, core intoccato.
> Metodo: design-doc → `test_place.c` deterministico → wiring `vat_horde` →
> verifica visiva a occhio → commit. Vedi `TODO.md` M5b e
> `M5_DESIGN.md` §7 ("Muri/strutture costruite dal giocatore").

## 1. Obiettivo e scope

Durante la PARTITA (sim viva, NON l'editor che è EDIT a sim ferma) il giocatore
seleziona un oggetto da un catalogo, lo posiziona col mouse, e lo conferma
spendendo un budget. UN SOLO sistema unificato — **selezione → preview/ghost →
validità → commit/spesa** — di cui barricate, torrette e (in futuro) trappole
sono semplici VOCI di catalogo, non tre meccaniche separate.

Anti-obiettivo: tre flussi di piazzamento copincollati. Se aggiungere una
trappola richiede di toccare la macchina di stato, il design ha fallito: una
trappola nuova deve essere SOLO una riga di catalogo + un commit handler.

Fuori scope (doc futuri): economia/biomassa che ALIMENTA il budget (M5b — qui
il budget è un intero dato), UI grafica ricca (qui: ghost 3D + HUD testuale),
rotazione/orientamento fine degli oggetti (v1 = asse-allineato + step a 90°).

## 2. Mattoni che ESISTONO già (niente da inventare nel core)

| Serve | API esistente | File |
|---|---|---|
| Schermo→mondo a y=0 | `pick_y0(vp,mx,my,SW,SH,&wx,&wy)` | `vat/edit_pick.h` |
| Barricata distruttibile | `def_add_structure(g,hp,0)` + `def_struct_cell` (alza muro + tier `wall_cost` barricata) + `def_struct_set_debris` (crollo→detriti draggable) | `defense.c` |
| Torretta | `def_add_turret(g,&DefTurret)` | `defense.c` |
| Oggetto trascinabile (cassonetto/auto) | `simp_drag_add` / `place_car` (2 dischi+rod) | `sim_particles.h`, `vat_horde.c` |
| Budget | `def_set_budget` / `def_budget` / `def_spend(g,cost)` (1=pagato) | `defense.c` |
| Veto "c'è spazio" | `simp_free_at(s,x,y,r)` (no overlap agenti/muri) | `sim_particles.h` |
| Veto "sopra statico" | `ter_blocked` (footprint palazzo/roccia) | editor/`terrain.c` |
| Costo sfondamento per-cella | `simp_set_wall_cost` (tier barricata vs palazzo) | `sim_particles.h` |
| Grid nav (celle ↔ metri) | `simp_grid_w/_h`, `simp_cell_size` | `sim_particles.h` |

Conclusione: **zero API nuove al core.** Tutto vive in un modulo di gioco, come
`defense.c`.

## 3. Dove vive: modulo `place.h` / `place.c` (game-side, core-agnostico)

Come `defense.c`: pure C, niente GL/SDL, riusabile da `test_place.c` E
`vat_horde`. Conosce `SimP*` e `DefGame*` (per commit verso draggable/torrette/
strutture), NON conosce il rendering. Tiene SOLO lo stato della *sessione di
piazzamento* (cosa è selezionato, dov'è il cursore, se è valido); il disegno del
ghost e la lettura del mouse stanno nel chiamante.

```c
typedef enum { PL_BARRICADE, PL_TURRET, PL_BIN, PL_CAR, /* PL_TRAP… */ PL_NITEMS } PlKind;

typedef struct {                 /* una voce di catalogo (data-driven) */
    PlKind kind;
    const char *name;            /* "Barricata", "Torretta"…           */
    int   cost;                  /* unità di budget                    */
    float w, h;                  /* footprint asse-allineato (m); car: lunghezza */
    float radius;                /* per i dischi (bin/car/torretta)    */
    /* parametri specifici riempiti al commit (hp barricata, DefTurret base…) */
} PlItem;

typedef struct {
    int    active;               /* modalità piazzamento on/off        */
    int    sel;                  /* indice voce catalogo selezionata   */
    float  cx, cy;               /* cursore mondo (da pick_y0)         */
    int    rot90;                /* 0..3, orientamento a step di 90°   */
    int    valid;                /* commit consentito qui? (calcolato) */
    int    reason;               /* PL_OK / PL_NOFUNDS / PL_BLOCKED / PL_OVERLAP */
} Placement;
```

API (tutta game-side, deterministica, niente I/O):

```c
void  pl_init(Placement *p, const PlItem *catalog, int n);
void  pl_set_cursor(Placement *p, float wx, float wy);   /* da pick_y0 */
void  pl_cycle(Placement *p, int d);                     /* cambia voce */
void  pl_rotate(Placement *p, int d);                    /* rot90 ±1    */

/* Ricalcola validità SENZA mutare il mondo: budget? footprint libero? non sopra
 * uno statico? Chiamata ogni frame mentre la sessione è attiva → guida il
 * colore del ghost. Veto: simp_free_at su ogni disco del footprint + callback
 * pl_blocked (host: ter_blocked) + def_budget >= cost. */
int   pl_validate(Placement *p, DefGame *g, SimP *s);    /* → p->valid, p->reason */

/* Conferma: se valido, spende il budget (def_spend) E materializza l'oggetto
 * (dispatch su kind → simp_drag_add / def_add_turret / def_add_structure+celle).
 * Ritorna 1 se piazzato. Il commit NON ricalcola validità: chiamare pl_validate
 * prima (vat_horde lo fa già ogni frame). */
int   pl_commit(Placement *p, DefGame *g, SimP *s);
```

Dispatch del commit (lo SWITCH è l'UNICO punto che cresce per voce nuova):

- **PL_BARRICADE**: rasterizza il rect ruotato (w×h) in celle nav, una
  `def_add_structure(g, hp, 0)` + `def_struct_cell` per cella (alza muro + tier
  barricata via `def_struct_cell`, vedi `BARRICADE_WALL_TIER`), `def_struct_set_debris`
  per il crollo→detriti. Reroute automatico via il commit nav già fatto da
  `def_struct_cell`.
- **PL_TURRET**: riempie un `DefTurret` base (range/arc/fire dal catalogo) e
  `def_add_turret`. Opz. `def_turret_make_destructible`.
- **PL_BIN**: `simp_drag_add(s, cx, cy, radius, mass)`.
- **PL_CAR**: i due dischi + rod (la `place_car` di `vat_horde` → fattorizzare in
  `place.c` o lasciare la helper al chiamante e chiamarla dal commit handler).
- **PL_TRAP** (futuro): nuova riga catalogo + nuovo case. Niente altro tocco.

## 4. Validità / veto (cuore della UX)

`pl_validate` produce `valid` + `reason` (per il feedback: ghost verde / rosso +
motivo nell'HUD). Regole in ordine:

1. **Budget**: `def_budget(g) >= item->cost`. No → `PL_NOFUNDS`.
2. **Sopra statico**: callback host `pl_blocked(cx,cy,…)` (vat_horde la lega a
   `ter_blocked`, il footprint palazzo/roccia). Sì → `PL_BLOCKED`.
3. **Spazio fisico**: `simp_free_at(s, x, y, r)` su ogni disco del footprint
   (per la barricata: campiona le celle; per bin/car/torretta: i dischi reali).
   Occupato da orda/muro → `PL_OVERLAP`. *Nota:* piazzare una barricata IN MEZZO
   alla folla è discutibile — v1: vietato (free_at fallisce), evita di
   intrappolare/espellere agenti. Tuning futuro: consentirlo e lasciare che il
   PBD shovi (le barricate sono già sfondabili).
4. Altrimenti `PL_OK`, `valid=1`.

Determinismo: `pl_validate` legge solo stato (budget, grid, posizioni) — nessun
RNG. Stesso mondo + stesso cursore ⇒ stessa risposta (testabile).

## 5. Budget

Il budget è l'intero già gestito da `defense.c` (`def_set_budget` all'avvio,
`def_spend` al commit). Qui NON si genera reddito (è M5b economia): `place.c`
spende e basta. `pl_commit` chiama `def_spend(g, cost)` PRIMA di materializzare;
se per qualche motivo non basta (race con pl_validate — non dovrebbe), annulla.

## 6. Wiring in `vat_horde` (PLAY)

Sostituisce/assorbe i tasti ad-hoc `B` (cassonetto) e `N` (auto) — diventano
voci di catalogo, non tasti dedicati. Bozza interazione:

- **`P`**: toggle modalità piazzamento (come `TAB` per EDIT, ma a sim VIVA).
- **rotella / `[` `]`**: `pl_cycle` (scorre il catalogo).
- **`,` `.`**: `pl_rotate` (orientamento 90°, solo barricata/car).
- **mouse move**: ogni frame `pick_y0` → `pl_set_cursor` → `pl_validate`.
- **LMB**: `pl_commit` (se valido). Click su invalido = no-op + flash rosso.
- **RMB / `Esc`**: esci dalla modalità.
- **render ghost**: riusa il pass flat-shaded dell'overlay editor (`ed_overlay`
  disegna già rect/poly color-codati) — qui un quad/disco al cursore, **verde se
  `valid` else rosso**. La sim continua a girare sotto (a differenza di EDIT).
- **HUD**: voce selezionata + costo + budget residuo + motivo se invalido.

La camera resta guidabile (frecce/zoom). Il blast `E` e gli altri tasti PLAY
restano; il piazzamento è uno STATO sovrapposto, non una modalità esclusiva come
EDIT (la sim non si ferma).

## 7. Verifica — `test_place.c` (deterministico, no SDL/GL)

Sul modello di `test_director.c`/`test_base.c`. Scena minima in-code (grid +
DefGame + budget). Casi:

1. **Budget gate**: budget per 2 torrette; 3 commit → 2 piazzano, il 3° dà
   `PL_NOFUNDS`, `def_budget` esatto a fine.
2. **Veto spazio**: commit sopra un agente/muro → `PL_OVERLAP`, niente piazzato,
   budget intatto. Su cella libera → piazza.
3. **Veto statico**: `pl_blocked` mockata true su un footprint → `PL_BLOCKED`.
4. **Barricata reroute**: piazza una barricata che chiude un corridoio →
   verificare che le celle siano muro (`simp_is_wall`/`def_cell_struct`) e che il
   flow devii (drain da un varco alternativo > 0; sigillo totale = assedio).
5. **Rotazione**: footprint w≠h, `rot90` 1 → celle occupate trasposte (asse
   lungo ruotato).
6. **Determinismo**: stessa sequenza di (cursore, kind, commit) ⇒ stesso stato
   mondo (checksum posizioni draggable + lista celle muro + budget). No-NaN.

Aggiungere `test_place` a `Makefile` (`make test`) e al `.gitignore`.

## 8. Ordine di implementazione

1. `place.h`/`place.c` + catalogo minimo (barricata, torretta, cassonetto) +
   `test_place.c` casi 1-3,6 → `make test` verde.
2. Casi 4-5 (reroute barricata, rotazione).
3. Wiring `vat_horde` (modalità `P`, ghost, HUD), assorbe `B`/`N`.
4. Verifica visiva a occhio dall'utente (piazza barricate sotto le torrette,
   l'orda devia; budget si esaurisce) → commit.

## 9. Questioni aperte

- **Auto (car) nel catalogo**: il commit della car usa la rod-link; va bene come
  voce, ma il footprint per il veto è una capsula (2 dischi) non un rect — il
  validate campiona i due dischi separatamente (già previsto in §4).
- **Rimozione/vendita**: piazzato male? v1 niente undo a runtime (le barricate
  sono sfondabili dall'orda comunque). Valutare un "raccogli" con rimborso
  parziale quando arriva l'economia (M5b).
- **Trappole**: design separato (DA FARE). Questo sistema le ospita come voce +
  commit handler; il loro EFFETTO (danno/rallenta/esplode all'innesco) è roba
  loro, non del piazzamento.
- **Validità barricata in mezzo alla folla**: v1 vietata (§4.3); rivedere a occhio.
- **Budget come reddito**: tutto M5b (biomassa). Qui è un intero statico.

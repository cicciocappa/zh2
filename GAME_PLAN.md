# GAME_PLAN — piano di completamento del gioco (M9)

Piano operativo per portare il sandbox attuale a GIOCO completo. Scritto per
essere eseguito fase per fase in sessioni future (anche da modelli diversi):
ogni fase è autoconsistente, dice cosa RIUSA, cosa COSTRUISCE, quali API core
nuove servono (minimizzate e segnalate in maiuscolo) e come si VERIFICA.
Agenda sintetica in `TODO.md` §M9; contesto architetturale in `CLAUDE.md`.

## 0. Il loop di gioco target (visione, giugno 2026)

1. **DEPLOY** — il giocatore scende con un ELICOTTERO in una strada, una
   piazza o uno spiazzo nei campi. Il punto di atterraggio (LZ) è il cuore
   della base: l'elicottero È il core (struttura assediabile §7 di
   M5_DESIGN → la sconfitta esiste già: `def_lost`).
2. **Missione**: *resistere T minuti* oppure *uccidere N zombie* (di norma:
   tutti quelli che gli spawner emetteranno — pool finiti).
3. **PREP** — fase di preparazione a tempo/illimitata: il giocatore piazza
   **torrette** (leggere, pesanti, lanciafiamme, acido…, upgradabili),
   **trappole** (fossati, mine, trappole elettriche ricaricabili…) e
   **barriere** (filo spinato, mura, cancellate…, resistenza ∝ costo)
   consumando le risorse assegnate all'inizio della missione. Le barriere
   si piazzano DISEGNANDO LINEE sulla mappa (alla Age of Empires), non
   come singoli rettangoli.
4. **ASSAULT** — i director scatenano le orde. Ogni kill rilascia
   **biomassa**, spendibile in corsa per: upgrade delle difese, riparazione
   barriere, **attacchi speciali** (bombardamento, mortaio, esche che
   dirottano l'orda) e — decisione da playtest — piazzare ULTERIORI difese
   durante l'assalto (place.c lo permette già; è una manopola di design,
   non di codice).
5. **Munizioni**: le torrette esauriscono i colpi; le scorte di ricarica
   sono limitate → il giocatore sceglie quale torretta ricaricare prima.
6. **WIN** al timer/kill target; **LOSE** al crollo del core (elicottero).

## 1. Metodo di lavoro (INVARIANTE, vale per ogni fase)

Il metodo che ha prodotto tutto il codice verificato finora. NON derogare:

1. **Design doc prima** (o sezione nuova in questo file): API, dati, piano
   di verifica. Decisioni discusse con l'utente PRIMA di scrivere codice.
2. **Test headless deterministico** (no SDL/GL, come `test_place`,
   `test_director`): la meccanica si dimostra a numeri, con check di
   determinismo + no-NaN. `make test` deve restare verde.
3. **Aggancio a `vat_horde`** (l'unico tool visivo; il sandbox 2D è morto).
4. **Verifica visiva dell'utente**: compilare, dire in una riga cosa
   guardare, fermarsi. Screenshot headless (`VAT_HORDE_SHOT`) solo per
   correttezza, non come prima validazione estetica.
5. Commit per fase, messaggio `gameplay: …` come i precedenti.

Vincoli architetturali (da `CLAUDE.md`, ripetuti perché vitali):
- I core (`sim_particles`, `sim`) restano **zero-dep, renderer-agnostici**.
  La logica di gioco vive nei moduli game-side (`defense.c`, `place.c`,
  `destruct.c`, futuri `mission.c`/`traps.c`): parlano al core SOLO via
  API `simp_*`. Ogni API core nuova va contrattata e mappabile su compute
  shader (M8).
- Niente malloc negli step; SoA; RNG deterministici seedati; riferimenti
  persistenti per HANDLE/SLOT, mai per indice denso.
- Librerie leggere: per la UI in-game usare **nuklear o microui**
  (single-header C), NON Dear ImGui.

## 2. Inventario: cosa esiste già (non reinventare)

| Meccanica richiesta            | Mattone esistente                                       |
|--------------------------------|---------------------------------------------------------|
| Base assediabile + sconfitta   | `def_add_structure`/`def_struct_cell`/`def_lost` (§7)   |
| Torrette leggere/pesanti, piercing | `DefTurret` + `def_update` (M5 §4), `simp_query_ray` |
| Ondate + spawn burst-free      | `DefDirector` (§8), `simp_free_at`                      |
| Budget di piazzamento          | `def_set_budget`/`def_budget`/`def_spend`               |
| Piazzamento runtime + ghost    | `place.c` (catalogo, validate/commit) + hook vat_horde  |
| Barricate distruttibili+detriti| strutture + tier sfondamento + hybrid (`DRAG_DESIGN`)   |
| Esplosioni/lanci in aria       | `simp_apply_impulse_ex` (M3.2), danno AoE (`test_query`)|
| Attrazione/repulsione nav      | `simp_add_cost` (w<0 = richiamo screamer → ESCHE)       |
| Sciogliere pile di cadaveri    | `simp_corpse_clear` (fuoco/acido — sinergia lanciafiamme)|
| Rallentamento post-spawn       | `simp_set_vpref` (crawl) — per debuff/stun soft         |
| FX (fiamme, fumo, esplosioni)  | `fx_particles` (emitter data-driven `FxEmitterDef`)     |
| Editor livelli + formato scena | `scene.h` vettoriale + EDIT mode (`EDITOR_DESIGN`)      |
| Kill count / eventi render     | `def_kills`, `DefEventFn`                               |

Ordine delle fasi: A→B→C→D→E→F→G→H. A–B sbloccano il "gioco che si gioca";
C–F sono contenuto strategico incrementale (ognuna giocabile da sola);
G (UI) parte minimale in A e cresce a ogni fase; H è tuning finale.
Stima: 1–2 sessioni per fase col metodo §1.

## 3. Bilanciamento: `balance.cfg` (trasversale, richiesto 2 lug 2026)

Decine di parametri di tuning (HP zombie per tipo, range/cadenza/danno
torrette, HP e `seg_len` delle barriere, costi, rate director…) andranno
in **UN file di config letto a runtime**, non in un header compilato.
Raccomandazione (motivata, da confermare col primo uso):

- **Default compilati nel codice** (struct `Balance` con inizializzatori):
  il file può mancare o essere parziale — zero fragilità, i test headless
  girano sui default e restano deterministici.
- **`balance.cfg` opzionale che li sovrascrive**: testo `chiave valore`
  con commenti `#`, chiavi namespaced (`turret.light.range 18`,
  `barrier.wall.seg_len 2.0`, `zombie.man.hp 100`). Parser zero-dep
  ~50 righe, stesso stile di `props/catalog.txt` e del `set k v` di scena.
- **Hot-reload** con un tasto in `vat_horde` (rilettura + riapplica alle
  tabelle): il tuning si fa GIOCANDO, senza ricompilare né riavviare —
  è il motivo per cui il file batte l'header statico, non la velocità di
  compilazione (che qui è irrilevante).
- Le tabelle data-driven previste dalle fasi (TurretDef in C, PlItem in B,
  TrapDef in D, tipi barriera) si RIEMPIONO da qui; il `set k v` di scena
  resta per i parametri della SIM per-scena (altra cosa: fisica, non
  bilanciamento). Il file nasce in fase A/B con le prime chiavi vere e
  cresce con ogni fase; niente chiavi speculative.

---

## Fase A — Macchina a stati di missione (`mission.c`)

> **FATTA (2026-07-04).** `mission.h/.c` + director con `start_delay`/`pool`
> (defense §8), formato `.scn` esteso (`exit`/`lz`/`mission`/`budget`, nomi
> BLENDER_LEVEL §8), `lz` → core assediabile + goal (`build_lz_core` in
> vat_horde, elicottero = fiction futura), piazzamento solo in PREP, INVIO =
> via all'assalto (nella shell comanda la shell: in APP_PREP la missione è
> congelata, START_ASSAULT → `mission_go`). Verifica: `test_mission` (SURVIVE
> vinta/persa via `def_struct_damage`, CLEAR con pool esatti e dormienti
> ignorati, determinismo); banco visivo `assets/scenes/mission_demo.scn`.

**Obiettivo**: trasformare la demo in PARTITA: PREP → ASSAULT → WIN/LOSE,
con la missione dichiarata nel `.scn`.

**Costruire** (game-side, zero API core):
- `mission.h/.c`: stato `MISSION_PREP / ASSAULT / WON / LOST`, timer,
  config `MissionDef { kind: SURVIVE|CLEAR; survive_s; prep_s (0 = PREP
  illimitata, si esce con un tasto/bottone "via"); budget_start; }`.
  Update per step: `LOST` se `def_lost`; `SURVIVE` vince al timer;
  `CLEAR` vince quando TUTTI i director hanno esaurito il pool E gli
  agenti vivi (non dormienti fuori mappa) sono 0.
- **Director con pool finiti**: è il task già disegnato in TODO M5b
  «spawn event SCRIPTATI (director-per-uscita)» — campi `start_delay`,
  `pool`, `base_rate` per director, UN director per uscita. Le missioni
  CLEAR lo RICHIEDONO (serve un totale finito). Implementarlo qui.
- Formato `.scn` (retro-compatibile, come previsto da `EDITOR_DESIGN` §2):
  `mission survive 300 [prep 90] [budget 500]` / `mission clear …`;
  `exit x y w h rate [delay] [pool]` = spawn rect + script del suo director.
- I director partono SOLO in ASSAULT; in PREP la sim gira (per vedere il
  mondo vivo) ma senza emissione.
- **Elicottero/LZ**: entità `lz x y` nel `.scn` → a `build_world` diventa
  il core (struttura assediabile, HP alti) + goal centrale. La discesa
  dell'elicottero è PURA FICTION renderer-side in vat_horde (mesh glb
  scriptata su una rampa di quota nei primi ~5 s di PREP + FX polvere con
  `fx_particles`); l'asset `.glb` lo fa l'utente in Blender. In v1 basta
  un placeholder (box) — la meccanica non dipende dall'arte.

**Verifica**: `test_mission.c` headless — SURVIVE vinta al timer con core
in piedi; SURVIVE persa (core giù prima del timer); CLEAR vinta a pool
esauriti e mappa pulita; pool del director esatto (emessi == pool);
determinismo + no-NaN. In vat_horde: HUD di stato (fase, timer, kill/pool)
e blocco del piazzamento fuori PREP (salvo decisione biomassa, fase E).

## Fase B — Barriere disegnate a LINEA (estensione `place.c`)

**Obiettivo**: piazzare le barriere trascinando segmenti (alla AoE),
con tipi a resistenza/costo diversi.

**Costruire** (game-side, zero API core):
- `place.c`: nuovo kind `PL_WALL_LINE` con stato di sessione ancora→cursore
  (`ax,ay` al primo click, anteprima fino al cursore, commit al secondo
  click; ESC/RMB annulla). Rasterizzazione del segmento in celle nav
  (DDA/Bresenham con spessore in celle = `it->h`); la STESSA enumerazione
  per validate (ogni cella: `simp_free_at` + veto statici) e commit —
  come già fa `pl_cells`, che resta per i footprint rect.
- **Costo al metro**: `cost` della voce = budget per CELLA (o per metro);
  `pl_validate` calcola il totale del segmento corrente e lo espone per
  il ghost/HUD (rosso se non affordabile). Segmenti lunghi = cari.
- **Auto-segmentazione** (richiesta 2 lug 2026): la linea disegnata viene
  SPEZZATA automaticamente in segmenti adiacenti di lunghezza fissa
  `seg_len` (da `balance.cfg` per tipo barriera, es. 2 m → un muro di
  10 m = 5 segmenti; l'ultimo assorbe il resto), **ogni segmento = una
  struttura** `def_add_structure` con il SUO pool HP (`hp_per_seg` da
  config) → il muro crolla UN PEZZO alla volta, mai tutto insieme.
  Brecce = gap fra segmenti; sezione debole = segmento economico.
  La STESSA suddivisione va applicata in `build_walls_from_scene`
  (vat_horde) ai rect `wall` di scena, che oggi fanno 1 rect = 1 pool
  unico: si spezza lungo l'asse lungo del rect, il formato `.scn` non
  cambia (il rect resta UNA entità logica, la segmentazione è al build).
- Catalogo barriere (voci `PlItem`, tutte già esprimibili):
  - `filo spinato` — economico, HP bassissimi, `cost_mult` di sfondamento
    minimo (l'orda lo apre in fretta ma NE È RALLENTATA fisicamente
    mentre preme). v1: barriera solida fragile (zero meccaniche nuove).
    v2 (opzionale, vedi fase D): hazard passabile che ferisce/rallenta.
  - `mura` — care, HP alti, tier medio.
  - `cancellata` — HP medi; l'idea "le torrette sparano attraverso"
    richiede celle solide-per-agenti ma trasparenti-ai-ray → API CORE
    NUOVA (maschera LoS in `simp_query_ray`). RINVIARE: v1 = muro
    normale; annotare in Questioni aperte.
- Ghost: il marker attuale è un disco; per la linea servono N marker o un
  quad orientato (riusa il buffer overlay; banale).

**Verifica**: `test_place` esteso — raster del segmento (orizzontale,
verticale, diagonale, spessore 2), costo ∝ celle, veto se una cella del
segmento è occupata, commit → muri + reroute (già testato per i rect),
determinismo. Visiva: disegnare un perimetro attorno alla LZ e vederlo
assediato/sfondato sul segmento debole (mappe-banco `arena_*` come riferimento).

## Fase C — Tipi di torretta, upgrade, munizioni

**Obiettivo**: arsenale differenziato e la scelta strategica "chi ricarico?".

**Costruire** (game-side; API core: NESSUNA — vedi nota fossato in D):
- **Catalogo torrette data-driven**: tabella `TurretDef { name, cost,
  range, fire_period, damage, heavy, piercing, kind }` che riempie
  `DefTurret` al commit (oggi `commit_turret` ha i numeri hardcoded —
  spostarli in tabella). Voci iniziali:
  - *leggera* / *pesante* — esistono già (logoramento vs gib+impulso).
  - *lanciafiamme* — niente ray: cono corto = `simp_query_circle` sul
    settore (filtro angolare sugli indici ritornati), danno per tick a
    TUTTI i bersagli nel cono (multi-hit), FX fiamme `fx_particles`.
    Bonus: `simp_corpse_clear` periodico nel cono (brucia le pile).
  - *acido* — proiettile lobbed (fiction visiva) + AoE al punto d'impatto:
    danno + `simp_corpse_clear` (scioglie i cadaveri → contro-gioco alle
    barricate di corpi). DoT v1 = danno secco; DoT vero solo se il
    playtest lo chiede (richiederebbe un timer per-slot in defense.c).
- **Upgrade**: `def_turret_upgrade(g, id)` = tier++ con moltiplicatori da
  tabella (damage/range/period; il piercing è già un flag upgrade).
  Costo in budget/biomassa. UI: click sulla torretta / tasto sul target.
- **Munizioni**: campi `ammo, ammo_max` in `DefTurret` (decremento per
  colpo in `def_update`, a 0 la torretta tace ma resta bersaglio);
  scorta globale `ammo_reserve` in `DefGame`; `def_turret_reload(g,id)`
  trasferisce dalla scorta (quantità min(reserve, max-ammo)). La scorta
  si ricompra con la biomassa (fase E). Lanciafiamme/acido: "colpi" =
  secondi di erogazione / proiettili, stessa macchina.

**Verifica**: `test_turret`/`test_defense` estesi — cono lanciafiamme
(bersagli dentro/fuori settore vs brute force), acido pulisce
`corpse_mass` nel raggio, ammo scende e la torretta si ferma a 0, reload
limitato dalla scorta, upgrade cambia i numeri, determinismo. Visiva:
FX fiamme/acido, HUD ammo per torretta.

## Fase D — Trappole (`traps.c`)

**Obiettivo**: mine, trappole elettriche ricaricabili, fossati.

**Stato**: la MINA è FATTA e cablata nell'host (2026-07-06) — core `traps.c`
(one-shot a prossimità → `TrapBlastFn`), piazzamento in PREP via `place.c`
(`PL_TRAP` → `traps_add`, undo), `traps_update` mappata su `host_blast`,
render `landmine.glb`. Vedi PREP_UI_DESIGN §10.7. ZAP (ricaricabile) e MOAT
(fossato, richiede `simp_set_speed_scale`) restano da fare.

**Costruire** (modulo game-side nuovo `traps.h/.c`, gemello di
`destruct.c`: possiede lo stato runtime, legge il core via query):
- `TrapDef { kind: MINE|ZAP|MOAT; cost, trigger_radius, damage, blast_r,
  up_ratio, recharge_s, uses }` + pool di istanze piazzate via `place.c`
  (kind `PL_TRAP`, footprint disc/rect).
- *mina* — one-shot: `simp_query_circle(trigger_r)` → se n>0: danno AoE
  (kill per indice DECRESCENTE o via handle — trappola M3.4 documentata)
  + `simp_apply_impulse_ex` (corpi in aria) + decal/FX + `simp_add_danger`
  al punto (il sangue-paura fa da deterrente emergente: l'orda impara a
  evitare il campo minato — meccanica GRATIS).
- *elettrica* — come la mina ma con `recharge_s` (timer) e `uses`
  (ricariche pagate in biomassa, fase E); danno senza impulso.
- *fossato* — regione che l'orda evita E che rallenta chi la attraversa.
  L'evitamento è gratis (`simp_add_cost` w>0 sull'area). Il RALLENTAMENTO
  fisico per-cella oggi non esiste: **UNICA API CORE NUOVA del piano**:
  `simp_set_speed_scale(x, y, f)` — campo per-cella `speed_scale`
  (default 1.0) campionato nello steering come moltiplicatore di v_pref.
  Mappa 1:1 su compute (un sample in più nel job steering), niente stato
  per-agente, deterministico. Serve anche al filo spinato v2 e al "fango"
  già citato nei doc. Contrattarla con l'utente prima di implementarla.
- Rendering: mesh placeholder per tipo (box/disco), stato armed/ricarica
  leggibile (colore), buca del fossato = decal scura v1.

**Verifica**: `test_traps.c` — mina uccide nel raggio e lancia in aria
(riusa le metriche di `test_impulse`), elettrica rispetta il ciclo
carica/scarica, fossato: tempo di attraversamento di una colonna con/senza
`speed_scale` (rapporto atteso), ordine kill sicuro, determinismo+no-NaN.

## Fase E — Biomassa (economia in-partita, ex M5b)

**Obiettivo**: i kill fruttano risorse spendibili DURANTE l'assalto.

**Costruire** (game-side):
- v1 SEMPLICE (decisione: niente droni/raccolta fisica in v1, come già
  annotato in TODO M5b): `biomass` in `DefGame`, accreditata alla morte
  con bounty per tipo (`EnemyDef.bounty`; il punto di aggancio esiste —
  "biomassa-stub" in M5 §6). Il blob a terra + TTL resta come FX visivo
  opzionale (pool come i cadaveri), NON come meccanica di raccolta.
- Spese (tutte già possibili tecnicamente dopo C/D):
  `def_turret_upgrade`, `def_turret_reload`/scorta munizioni,
  `def_struct_repair(g, id, hp)` (nuova, banale: hp += min(…, max-hp),
  vietata su strutture collapsed), ricariche trappole, attacchi speciali
  (fase F), e — se si decide di sì — piazzamenti aggiuntivi in ASSAULT
  (place.c già funziona a runtime; è solo una policy di `mission.c`).
- Bilanciamento: bounty ∝ pericolosità (tank ≫ walker); i prezzi in una
  tabella unica (facile da tarare).

**Verifica**: `test_defense` esteso — bounty accreditati per tipo, repair
capped, spesa rifiutata senza fondi, determinismo. Visiva: contatore
biomassa nel HUD, blob FX.

## Fase F — Attacchi speciali

**Obiettivo**: mortaio, bombardamento aereo, esche. Tutti game-side,
zero API core.

**Costruire**:
- Modulo `strikes.c` (o dentro `mission.c`): richieste a target
  `(x, y, kind)` con costo biomassa, delay di arrivo, poi effetto:
  - *colpo di mortaio* — dopo `delay_s`: danno AoE + `simp_apply_impulse_ex`
    + decal cratere + FX; N colpi con dispersione (RNG di gioco seedato).
  - *bombardamento* — fila di esplosioni lungo una direzione (riusa il
    mortaio in sequenza).
  - *esca* — `simp_add_cost` NEGATIVO (richiamo, clamp −0.8 già nel core)
    su un'area, con TTL gestito lato gioco: allo scadere si RIMUOVE
    esattamente il delta applicato (add_cost dell'opposto) — tracciare
    l'importo per cella applicato per non sporcare il campo. L'orda
    converge sull'esca e ignora la base per un po'.
- Input: modalità targeting col mouse (come il ghost di placement).

**Verifica**: `test_strikes.c` — mortaio uccide nel raggio al tempo
giusto; esca: drain/flusso si sposta sull'area richiamo e TORNA normale
allo scadere (campo costi bit-uguale a prima dell'esca); determinismo.

## Fase G — UI in-game (incrementale, parte in fase A)

**Obiettivo**: uscire dall'HUD-nel-titolo. Necessaria per giocare davvero.

**Costruire**:
- v0 (fase A): font bitmap minimale (già in agenda EDITOR fase 1) per
  timer/fase/risorse/messaggi a schermo.
- v1 (da fase C in poi): **nuklear** (single-header, preferenza utente
  annotata in CLAUDE.md; il download lo fa l'utente) sopra il GL context
  esistente: toolbar di piazzamento (voci catalogo + costi + selezione),
  pannello torretta (ammo, upgrade, reload), bottoni attacchi speciali,
  barre HP su core/segmenti sotto assedio (world-space, quad flat —
  anche senza nuklear).
- Il piazzamento resta guidato da `place.c` (stato) — la UI è solo input.

**Verifica**: visiva (è UI). I moduli sotto restano coperti dai test.

## Fase H — Contenuto, tuning, meta

- 3 mappe-missione corrispondenti alla fiction: *strada* (urbana, palazzi
  tier alto + uscite metro scriptate), *piazza* (aperta, multi-direzione),
  *campi* (spiazzo, branchi dormienti `pack` da svegliare + recinzioni).
  Si costruiscono nell'EDITOR (che intanto cresce, TODO M7).
- Passata di bilanciamento: budget iniziali, prezzi, bounty, HP barriere,
  rate/pool director per missione. Tabelle centralizzate → tuning veloce.
- Ordine di completamento consigliato SOLO dopo che A+B+C sono giocati a
  mano dall'utente: il playtest decide le priorità di D–F.
- Meta fuori-missione (campagna, progressione fra missioni, sblocchi):
  FUORI SCOPE di questo piano — doc futuro quando il loop singola-missione
  diverte.

## Questioni aperte (decidere con l'utente al momento giusto)

1. **Piazzare difese in ASSAULT?** Tecnicamente pronto; è una policy di
   `mission.c` (fase E). Provare entrambe nel playtest.
2. **Cancellata sparabile-attraverso**: richiede maschera LoS per-cella in
   `simp_query_ray`. API piccola ma core: solo se il tipo barriera vale
   la spesa.
3. **Filo spinato v2 come hazard passabile** (ferisce/rallenta senza
   bloccare): dipende da `simp_set_speed_scale` (fase D) + danno da
   cella (nuovo). v1 solido-fragile basta per lanciare.
4. **Drain vs attacking** (questione storica in TODO): col goal=LZ e le
   strutture attorno il modello attuale regge; rivalutare solo se il
   playtest mostra zombie che "evaporano" sul goal in modo illeggibile.
5. **Multi-goal** (basi di raccolta esterne, ex M5b avanzato): rinviato a
   dopo il loop base; richiede l'attribuzione multi-goal del drain.
6. **Munizioni**: quali torrette ne consumano (tutte? le pesanti sì e le
   leggere no?) — scelta di design in fase C.

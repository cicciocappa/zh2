# GUIDA AL COMPLETAMENTO DEL GIOCO

> Scritta il 2026-07-06 (da Fable, per istruire le sessioni future con Opus o
> altri modelli). Questo file dice **cosa manca, in che ordine farlo e come**.
> Non duplica i design doc: li indicizza e fissa lo stato REALE al 6 luglio,
> che è più avanti di quanto dicano alcune checkbox di `TODO.md`.

## 0. Prima di toccare qualsiasi cosa

Leggere, in quest'ordine:

1. `CLAUDE.md` — architettura, convenzioni, preferenze dell'utente. VINCOLANTE.
2. `GAME_PLAN.md` — il piano M9 fase per fase (A…H). Questa guida ne è
   l'aggiornamento di stato + l'ordine operativo.
3. Il design doc della fase su cui si lavora (tabella §2 sotto).

Regole non negoziabili (ripetute perché si dimenticano):

- **Metodo invariante** (GAME_PLAN §1): design doc discusso con l'utente →
  test headless deterministico → aggancio a `vat_horde` → **verifica visiva
  dell'utente** (compila, di' in una riga cosa guardare, FERMATI) → commit per
  fase. `make test` deve restare SEMPRE verde (32 test attuali).
- **Zero API core nuove** salvo quelle già contrattate nel piano (oggi ne
  resta UNA: `simp_set_speed_scale`, fase D fossato — va ri-contrattata con
  l'utente prima di scriverla). La logica di gioco vive nei moduli game-side
  (`defense.c`, `place.c`, `traps.c`, `mission.c`, …) che parlano al core
  solo via `simp_*`. Ogni ipotetica API core va discussa PRIMA e deve mappare
  su compute shader (M8).
- Niente malloc negli step, SoA, RNG seedati, riferimenti per HANDLE/SLOT mai
  per indice denso. Niente probing dell'ambiente; download/installazioni
  (es. nuklear) li fa l'utente. Conversazioni in italiano, codice e commenti
  in inglese.
- Bilanciamento: i numeri nuovi nascono in tabelle data-driven e, dalla fase
  C in poi, in `balance.cfg` (GAME_PLAN §3: default compilati + file
  opzionale che li sovrascrive + hot-reload con un tasto in vat_horde).
  Il file NON esiste ancora: nasce con le prime chiavi vere, niente chiavi
  speculative.

## 1. Stato reale al 2026-07-06 (fidati di questo, non delle checkbox)

| Fase GAME_PLAN | Stato | Note |
|---|---|---|
| A — Missione (`mission.c`) | **FATTA** | PREP→ASSAULT→WON/LOST, `.scn` esteso (`exit`/`lz`/`mission`/`budget`), `test_mission`, banco `mission_demo.scn`. |
| B — Barriere a linea | **FATTA di fatto** (assorbita da PREP_UI step 1-5, commit 962d8d7…b5b0d22) | Linea con fill a moduli, un modulo = una struttura, undo, ghost gittata, barra a 3 tab, cancellata sparabile-attraverso (asse C opacità). Restano i leftover in §3.1. |
| Esplosioni (EXPLOSION_DESIGN) | **Core + fase 2a FATTI** | `def_blast`/`def_damage_agent`/`destruct_force` + colonne catalogo `resist`/`burn` (`test_blast`); `host_blast` in vat_horde con FX colonna, danno da caduta chiuso. **Resta fase 2b** (§3.2). |
| D — Trappole (`traps.c`) | **MINA FATTA** (core+host, `test_traps`, `test_place` caso 14) | Restano ZAP e MOAT (§3.4). |
| C — Torrette: tipi/upgrade/ammo | **DA FARE** | §3.3. |
| E — Biomassa | **DA FARE** | §3.5. |
| F — Attacchi speciali | **DA FARE** | §3.6 — quasi tutto riuso di cose già esistenti. |
| G — UI in-game | **v0 fatta** (font8 + barra PREP) | v1 nuklear da fare (§3.7). |
| H — Contenuto + tuning | **DA FARE** | §3.8. |

Esistono già e NON vanno reinventati: la shell applicativa (`make game`,
`app.c`, briefing da `campaign.txt`, progressi in `progress.txt`), il
richiamo da fuoco (`def_set_fire_lure`, `test_lure`), i prop solidi/assediabili
(`prop_world.c`), i draggable/auto (DRAG_DESIGN), i prop one-shot
(`destruct.c`), l'export livelli da Blender (`BLENDER_LEVEL.md`,
`gfx/export_scn.py`). L'inventario completo dei mattoni è in GAME_PLAN §2 e
EXPLOSION_DESIGN §2.

## 2. Mappa doc ↔ lavoro

| Lavoro | Design doc |
|---|---|
| Leftover barriere/PREP | `PREP_UI_DESIGN.md` (§11 questioni aperte) |
| Esplosioni fase 2b | `EXPLOSION_DESIGN.md` (§7 scorch, §6 incendi, header = stato) |
| Torrette (C) | `GAME_PLAN.md` fase C + `M5_DESIGN.md` §4 (torrette esistenti) |
| Trappole (D) | `GAME_PLAN.md` fase D + `PREP_UI_DESIGN.md` §10.7 (com'è cablata la mina) |
| Biomassa (E) | `GAME_PLAN.md` fase E (+ M5 §6 "biomassa-stub" = punto d'aggancio) |
| Strikes (F) | `GAME_PLAN.md` fase F + `EXPLOSION_DESIGN.md` (mortaio = def_blast) |
| UI (G) | `GAME_PLAN.md` fase G |
| Livelli (H) | `BLENDER_LEVEL.md` + `EDITOR_PLAN.md` |

## 3. Il lavoro rimanente, nell'ordine consigliato

L'ordine privilegia: prima chiudere i mezzi-fatti (2b, leftover B), poi il
contenuto strategico che rende il gioco GIOCO (C→D→E→F), poi UI e contenuto.
Una fase ≈ 1–2 sessioni col metodo invariante.

### 3.1 Leftover fase B / PREP (mezza sessione)

- **Verifica in gioco della breccia**: al crollo di un SINGOLO modulo di una
  linea, il varco si apre e l'orda ci imbuta? (Annotato come "da controllare
  in gioco" dopo il fix scalettatura b5b0d22.) Solo playtest + eventuale fix.
- **Filo spinato v1** = nuova voce catalogo barriere: economica, HP
  bassissimi, `cost_mult` di sfondamento minimo. È SOLO una riga di
  `PlItem` + numeri: zero meccaniche nuove (la v2 hazard-passabile dipende
  da `simp_set_speed_scale`, vedi fossato).
- Decidere con l'utente le questioni PREP_UI §11 quando emergono dal
  playtest (refund, commit parziale della linea, conferma VIA).

### 3.2 Esplosioni fase 2b (1 sessione)

Da `EXPLOSION_DESIGN.md` (header = stato preciso):

- **Scorch §7 v1**: decal-cratere persistente a terra (pool come i decal di
  sangue) + annerimento facciate degli edifici colpiti (darkening della mesh,
  stesso trucco del danno strutture). Statics immuni in v1 (decisione §10.3).
- **Incendi dei prop `burn`**: prop con flag `burn` colpito → emitter
  fuoco+fumo (`fx_start_emitter` a durata) + `simp_add_danger` a peso basso
  per la durata (area denial, decisione §10.1; NIENTE propagazione, §10.2).
- Richieste utente annotate (2026-07-06), da fare qui o come polish dopo:
  (i) particelle esplosione a SPRITE texture invece dei billboard flat;
  (ii) lancio di qualche debris 3D dall'esplosione (stile mesh-gib).
- Verifica: visiva (è fiction); il core è già coperto da `test_blast`.

### 3.3 Fase C — Torrette: tipi, upgrade, munizioni (1–2 sessioni) + nascita di `balance.cfg`

GAME_PLAN fase C, zero API core. In sintesi:

- **Catalogo `TurretDef` data-driven** che riempie `DefTurret` al commit
  (oggi `commit_turret` in `place.c` ha numeri hardcoded → spostarli in
  tabella). Qui nasce `balance.cfg` (GAME_PLAN §3): parser zero-dep
  `chiave valore` con namespacing (`turret.light.range 18`), default
  compilati, hot-reload con un tasto in vat_horde.
- **Lanciafiamme**: niente ray — `simp_query_circle` sul settore + filtro
  angolare, danno per tick multi-bersaglio, FX fiamme, `simp_corpse_clear`
  periodico nel cono (brucia le pile).
- **Acido**: proiettile lobbed (fiction) + AoE all'impatto: danno +
  `simp_corpse_clear`. DoT v1 = danno secco.
- **Upgrade**: `def_turret_upgrade(g,id)` = tier++ con moltiplicatori da
  tabella.
- **Munizioni**: `ammo/ammo_max` in `DefTurret` (a 0 tace ma resta
  bersaglio), scorta globale `ammo_reserve`, `def_turret_reload`. Decisione
  di design da prendere con l'utente: QUALI torrette consumano ammo
  (questione aperta GAME_PLAN §Q6).
- Verifica: `test_turret`/`test_defense` estesi (cono vs brute force, acido
  pulisce `corpse_mass`, ammo/reload/upgrade, determinismo). Visiva: FX +
  HUD ammo.
- Trappola nota: gli indici da `simp_query_circle` muoiono al primo kill —
  convertire subito in handle o killare per indice decrescente (M3.4).

### 3.4 Fase D — Trappole restanti: ZAP e MOAT (1 sessione)

La mina è il modello: `traps.c` esiste, `PL_TRAP` in `place.c` esiste,
`traps_update` → `host_blast` esiste (PREP_UI §10.7).

- **ZAP (elettrica)**: come la mina ma con `recharge_s` (timer) e `uses`
  (ricariche pagate in biomassa, fase E); danno SENZA impulso. Quasi solo
  dati + un timer nel pool trappole.
- **MOAT (fossato)**: evitamento = `simp_add_cost` w>0 (gratis). Il
  rallentamento fisico richiede **`simp_set_speed_scale(x,y,f)`** — l'UNICA
  API core nuova di tutto il piano: campo per-cella campionato nello
  steering come moltiplicatore di v_pref, niente stato per-agente, mappa
  1:1 su compute. **Contrattarla con l'utente PRIMA di implementarla.**
  Serve anche a filo spinato v2 e al "fango".
- Verifica: `test_traps` esteso — ciclo carica/scarica della ZAP; tempo di
  attraversamento di una colonna con/senza speed_scale (rapporto atteso);
  determinismo + no-NaN.

### 3.5 Fase E — Biomassa (1 sessione)

- v1 SEMPLICE: `biomass` in `DefGame`, accreditata alla morte con bounty per
  tipo (`EnemyDef.bounty` — il punto d'aggancio "biomassa-stub" esiste in
  defense §6). Niente raccolta fisica/droni; il blob a terra è FX opzionale.
- Spese: `def_turret_upgrade`, `def_turret_reload`, ricariche ZAP,
  `def_struct_repair(g,id,hp)` (NUOVA ma banale: cap a max, vietata su
  collapsed), strikes (fase F). Prezzi in tabella unica → `balance.cfg`.
- Decisione playtest (GAME_PLAN §Q1): piazzare difese anche in ASSAULT?
  È solo una policy di `mission.c`, il codice lo permette già.
- Verifica: `test_defense` esteso — bounty per tipo, repair capped, spesa
  rifiutata senza fondi, determinismo. Visiva: contatore HUD.

### 3.6 Fase F — Attacchi speciali (1 sessione)

Tutto riuso, zero API core:

- Modulo `strikes.c`: richieste `(x,y,kind)` con costo biomassa e delay.
- **Mortaio** = dopo `delay_s`, chiama la primitiva blast ESISTENTE
  (`def_blast` core + fiction `host_blast`); N colpi con dispersione da RNG
  di gioco seedato. **Bombardamento** = fila di mortai in sequenza.
- **Esca** = `simp_add_cost` NEGATIVO su area con TTL e rimozione ESATTA
  allo scadere. ATTENZIONE: il clamp a −0.8 del core rende sbagliata la
  sottrazione cieca — il pattern giusto (leggere i delta per-cella applicati
  di ritorno da `simp_user_cost`) è GIÀ implementato e testato in
  `def_set_fire_lure` (`test_lure`, rimozione bit-esatta): copiarlo.
- Input: targeting col mouse come il ghost di placement.
- Verifica: `test_strikes.c` — kill nel raggio al tempo giusto; campo costi
  bit-uguale a prima dell'esca dopo lo scadere; determinismo.

### 3.7 Fase G — UI nuklear (1 sessione, dopo C)

- **Il download di nuklear lo fa L'UTENTE** (single-header C; chiederglielo,
  non cercarlo sul sistema né scaricarlo).
- Sopra il GL context esistente: toolbar piazzamento (la barra PREP c'è già
  in pure-C: nuklear la sostituisce/estende), pannello torretta (ammo,
  upgrade, reload), bottoni strikes, barre HP world-space su core/segmenti
  (queste ultime = quad flat, non serve nuklear).
- `place.c` resta il proprietario dello stato; la UI è solo input.
- Verifica: visiva. I moduli sotto restano coperti dai test.

### 3.8 Fase H — Contenuto e tuning (aperta)

- 3 mappe-missione: *strada* (urbana, palazzi tier alto + uscite metro),
  *piazza* (aperta multi-direzione), *campi* (branchi dormienti `pack` +
  recinzioni). Fonte di verità = `.blend` per livello in `levels/`
  (convenzione `BLENDER_LEVEL.md`, export `gfx/export_scn.py`; Blender
  portatile in `~/Scaricati/blender-5.1.0-linux-x64/`, non nel PATH).
  Gli asset 3D li fa l'utente; i placeholder bastano per la meccanica.
- Passata di bilanciamento su `balance.cfg` GIOCANDO (hot-reload).
- L'ordine fine di D–F può essere riordinato dal playtest di A+B+C: è una
  decisione dell'utente, chiederglielo quando A+B+C sono giocati a mano.
- Meta/campagna fuori-missione: FUORI SCOPE (doc futuro).

## 4. Questioni aperte da decidere CON l'utente (non deciderle da soli)

1. Piazzamento in ASSAULT sì/no (fase E, policy di `mission.c`).
2. Quali torrette consumano munizioni (fase C).
3. `simp_set_speed_scale`: contrattare l'API prima del fossato (fase D).
4. Filo spinato v2 hazard-passabile (dopo speed_scale; v1 basta per lanciare).
5. PREP_UI §11: refund, commit parziale linea, conferma VIA (dal playtest).
6. Drain vs attacking / multi-goal: NON toccare salvo che il playtest mostri
   problemi (GAME_PLAN §Q4–Q5).

## 5. Igiene di repo (da fare alla prima occasione)

- I binari di test `test_cover`, `test_lure`, `test_mission`,
  `test_prop_world` (e i futuri `test_blast`, `test_traps`, `test_strikes`)
  sono untracked: aggiungerli a `.gitignore` come gli altri eseguibili.
- `considerazioni.txt` è tracciato in git e contiene note di design MISTE a
  materiale personale dell'utente estraneo al progetto: non citarlo nei
  commit, non copiarne il contenuto nei doc, e suggerire all'utente di
  spostare le note di design nei design doc e il resto FUORI dal repo.

## 6. Come si verifica (riassunto operativo)

- `make test` = tutti i test headless (zero SDL/GL); devono passare TUTTI.
- `make game` = la shell giocabile (vat_horde + `-DGAME_SHELL`).
- Verifica visiva: compila, UNA riga su cosa guardare, stop — l'utente
  guarda. Screenshot headless (`VAT_HORDE_SHOT`, `SANDBOX_SHOT`) solo per
  correttezza o se l'utente non può guardare.
- Banchi visivi utili: `assets/scenes/mission_demo.scn` (partita completa),
  `arena4/arena_weak/arena_breach` (mura distruttibili),
  `props_demo.scn` (prop + cancellate). `VAT_HORDE_RATE` accelera lo spawn.
- Commit per fase, messaggi `gameplay:` / `traps:` / `explosions:` come i
  precedenti.

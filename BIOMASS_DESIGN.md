# Biomassa — economia di partita (design tecnico)

> **STATO: IMPLEMENTATO (2026-07-10).** Meccanica dettata dall'utente il
> 2026-07-09 (vedi anche GAME_PLAN fase E "biomassa" e Blocco 3 di
> torrette 2.0, che questo doc assorbe in parte: i kit di upgrade prodotti
> qui sono la valuta del pannello upgrade del Blocco 3). Moduli: `bio.h/.c`
> (convertitore), caricatori + `def_struct_repair` in defense.c, `biostock`
> in scene.c, wiring host in vat_horde (GAME_SHELL). Test: `test_bio`,
> `test_turret_mag`. Questioni §12 SCIOLTE con l'utente il 2026-07-10:
> Q1 tank PERSISTE; Q2 mortaio SOLO colpi prodotti, store parte pieno;
> Q3 riparazione a CLICK sulla struttura (tasto R arma il kit); Q4 ricarica
> a CLICK sulla torretta; Q5 HUD v1 = barra ASSALTO con card cliccabili +
> tasto O che cicla l'output; Q6 `biostock N` nel `.scn`, default 1 di ogni
> munizione (mortaio pieno). Restano per Blocco 3: balance.cfg (§9), pannello
> upgrade torrette; droni §8 = v2.

## 1. Principio

Ogni zombie ucciso frutta **biomassa**. La biomassa confluisce nell'UNICO
**convertitore** della base, il cui output è selezionabile dal giocatore in
qualsiasi momento: kit di riparazione, kit di upgrade, munizioni per
torrette (4 tipi), colpi di mortaio — estensibile. Lo stoccaggio è
**cappato per risorsa** (default 3): se il convertitore produce una risorsa
già al massimo, la biomassa in arrivo è **sprecata**. La pressione di gioco
è tutta lì: non esiste accumulo passivo, il giocatore deve gestire l'output
mentre difende.

Le torrette hanno **munizioni infinite ma caricatore limitato**: svuotato
il caricatore restano inattive per qualche secondo (ricarica automatica).
Le munizioni prodotte dal convertitore servono a **ricaricare all'istante**
(azzerano il tempo di ricarica). Il mortaio consuma i colpi prodotti.

Due valute, due fasi: il **budget** ($) resta la valuta del PIAZZAMENTO in
PREP; la **biomassa** è la valuta DURANTE l'assalto. Non si convertono.

Tutti i parametri (rese, costi, cap, caricatori, tempi) sono **manopole di
`balance.cfg`** (Blocco 3) e in parte **upgradabili in partita** (§7),
incluso il convertitore stesso: l'upgrade ricorsivo "100 kill per un kit,
il kit migliora la conversione, il prossimo ne costa 90" è voluto.

## 2. Cosa esiste già (inventario)

| Pezzo | Dove | Stato |
|---|---|---|
| Kill con corpo/tipo noti | eventi `DEF_EV_DEATH`/`DEF_EV_GIB` (slot, body) + `g->kills` | FATTO — l'host li ascolta già per gib/sangue: la resa per body si aggancia lì |
| Bounty per kill | GAME_PLAN fase E (mai implementata) | ASSORBITA da questo doc |
| Torrette con tipo | `DefTurretKind` 0/1/2/3 (light/heavy/flame/acid) | FATTO — mappa 1:1 sui 4 tipi di munizione |
| Loop di fuoco per torretta | `def_update` (period, `fired`) | FATTO — il caricatore si decrementa lì |
| Torrette silenziabili | `def_turret_disabled` (crollo) | FATTO — la ricarica è un secondo motivo di silenzio, stesso pattern per il render |
| Riparazione strutture | `def_struct_damage` (solo danno) | manca il verso opposto (`def_struct_repair`, §10) |
| Mortaio | BASE_DESIGN §3 (`gStrikes`, GAME_SHELL) | FATTO — oggi spara gratis, prenderà i colpi dallo store |
| Upgrade torrette (5 assi) | Blocco 3 (deciso, non implementato) | i kit prodotti qui ne diventano la valuta |
| Cadaveri: TTL + rimozione | pool `simp_corpse` (TTL) + decal | FATTO — i droni (§8) sono SOLO fiction sopra la rimozione che già avviene |
| balance.cfg hot-reload | Blocco 3 (deciso, non implementato) | tutte le manopole §9 ci finiscono |

Il core sim NON chiede nessuna API nuova. Defense chiede il caricatore
(§5) e la riparazione; l'economia è un modulo nuovo game-side (§3).

## 3. Architettura: `bio.c` + caricatori in defense + wiring host

Tre livelli, come il resto del gioco:

- **`bio.h`/`bio.c`** (modulo game-side NUOVO, zero-dep come mission/traps,
  niente defense/FX dentro): lo stato dell'economia — output selezionato,
  serbatoio di biomassa grezza, store per item, costi/cap/livelli upgrade.
  Deterministico, testabile headless. Non sa cosa sia una torretta: parla a
  item astratti (§6).
- **defense.c**: il CARICATORE per torretta (§5) + `def_struct_repair`. La
  verità di gioco resta qui, come per HP e assedio.
- **host (`vat_horde`)**: il wiring — evento kill → `bio_add(resa[body])`;
  tasto/click "ricarica ora" → `bio_take(munizione del kind)` →
  `def_turret_reload_now`; mortaio → `bio_take(BIO_MORTAR)` prima di
  sparare; kit riparazione → `def_struct_repair`; HUD del convertitore;
  fiction droni (§8, v2).

## 4. Il convertitore

Un solo stato, poche regole:

    tank += resa           a ogni kill (se lo store dell'output NON è pieno;
                           pieno = biomassa SPRECATA, regola utente)
    tank >= costo[output]  → tank -= costo, store[output] += 1 (istantaneo)

- Il **serbatoio è biomassa grezza**: al cambio di output il progresso
  RESTA (è lo stesso serbatoio che alimenta la macchina) — proposta §12.Q1.
- Niente componente temporale in v1: la conversione scatta al superamento
  della soglia. (Un tempo di lavorazione — e quindi una coda visibile — si
  può aggiungere dopo, è una manopola in più; i droni §8 daranno comunque
  latenza percepita alla raccolta.)
- Lo spreco è INFORMATO: l'HUD mostra "STOCCAGGIO PIENO" quando l'output
  selezionato è al cap (il giocatore sta buttando biomassa e deve saperlo).
- Callback `BioProducedFn(item)` per l'host: suono/flash quando esce un
  item (come gli eventi defense).

## 5. Caricatori torretta (defense.c)

`DefTurret` guadagna quattro campi, tutti con default legacy-safe:

    mag_size   colpi per caricatore; 0 = INFINITO (legacy: tutti i test M5
               e le scene esistenti non cambiano di un bit, stesso pattern
               di aim_tol<=0)
    mag        colpi rimasti (init = mag_size)
    reload_s   secondi di inattività a caricatore vuoto (default 5)
    reload_t   countdown in corso (0 = pronta)

Nel loop di fuoco di `def_update`: ogni COLPO decrementa `mag` (per
flame/acid il "colpo" è l'attivazione a `fire_period` — il getto conta a
tick, coerente col DoT); `mag == 0` → la torretta NON acquisisce/spara e
`reload_t = reload_s`; a countdown finito `mag = mag_size`. Il lure (che
segue `fired`) si spegne da solo durante la ricarica — giusto così: la
torretta muta non attira.

API nuove:

    def_turret_reload_now(g, tid)   ricarica ISTANTANEA (l'host la chiama
                                    dopo aver consumato l'item giusto)
    def_turret_reloading(g, tid)    secondi restanti (0 = pronta) — per il
                                    render (canna abbassata/lampeggio) e HUD

Munizione per kind: light → PROIETTILI, heavy → PROIETTILI PESANTI,
flame → COMBUSTIBILE, acid → ACIDO. La mappa vive nell'host (bio non
conosce i kind).

## 6. Gli item

| Item | Effetto | Costo default (biomassa) | Cap |
|---|---|---|---|
| `BIO_AMMO_LIGHT` | ricarica istantanea di una torretta light | 25 | 3 |
| `BIO_AMMO_HEAVY` | idem, heavy | 35 | 3 |
| `BIO_AMMO_FUEL` | idem, flame | 30 | 3 |
| `BIO_AMMO_ACID` | idem, acid | 30 | 3 |
| `BIO_MORTAR` | un colpo di mortaio | 40 | 3 |
| `BIO_REPAIR` | kit riparazione: +HP a una struttura | 60 | 3 |
| `BIO_UPGRADE` | kit upgrade: valuta del pannello Blocco 3 | 100 | 3 |

Resa per body (default, da tarare): walker/runner 1, tank 8 (in
proporzione agli HP; tabella per `DefBody` in balance.cfg). Numeri tutti
"default accettabili da tarare in sandbox", come da tradizione (EXPLOSION
§10.4).

Riparazione (v1): `def_struct_repair(g, id, hp)` — clampa a `hp_max`,
**solo strutture NON crollate** (un crollo è definitivo: celle già
liberate, reroute già avvenuto — "riparare" un muro caduto sarebbe un
re-piazzamento, e quello è mestiere della PREP). Il targeting UI è §12.Q3.

## 7. Upgrade ricorsivo (la curva)

I kit `BIO_UPGRADE` si spendono sul pannello torrette del Blocco 3 (5 assi,
già deciso) E sul convertitore stesso. Upgrade del convertitore (uno per
kit, livelli successivi):

    costo[item] *= k_conv      (default 0.9)     con FLOOR a 0.5·costo_base
    cap[item]   += 1           (ramo alternativo)  con TETTO a cap_base+3

Il floor/tetto evitano la degenerazione a fine partita (conversione quasi
gratis = economia rotta); la scelta del ramo (costi vs stoccaggio) è del
giocatore. Livelli e moltiplicatori in balance.cfg.

## 8. Droni (fiction, v2 — non blocca nulla)

Il credito biomassa avviene AL KILL, istantaneo (§4): i droni sono pura
scenografia della raccolta, senza effetto di gioco. Un pool piccolo di
drone-billboard (o box 3D stile gib) che partono dalla base verso i
cadaveri freschi, "aspirano" (il cadavere/decal svanisce — la rimozione
fisica c'è già: TTL del pool corpse) e tornano. NON tocca `danger` (il
sangue resta e decade da solo, coerente con blood-fear) e NON altera il
ruolo ostacolo dei cadaveri appena creati (i droni puntano quelli vicini a
scadenza TTL). Se in futuro si vorrà biomassa legata alla raccolta FISICA
(rischio/rendimento: cadaveri lontani = biomassa persa), è un cambio di
regola in bio.c, non di architettura — annotato, non v1.

## 9. balance.cfg (Blocco 3)

Sezione `[biomass]`: rese per body, costi per item, cap per item,
`k_conv`+floor, cap upgrade stoccaggio; sezione `[turret]` esistente del
Blocco 3 guadagna `mag_size`/`reload_s` per kind. Hot-reload = si tara a
occhio in partita, che è il punto del Blocco 3.

## 10. API nuove (tutte game-side)

| API | Dove | Note |
|---|---|---|
| modulo `bio.h/.c` (`bio_init/set_output/add/take/count/cost/cap/upgrade`) | NUOVO | §4/§6/§7; callback produced; zero-dep |
| campi `mag_size/mag/reload_s/reload_t` in DefTurret | defense.c | §5; `mag_size 0` = legacy bit-identico |
| `def_turret_reload_now(g, tid)` | defense.c | ricarica istantanea |
| `def_turret_reloading(g, tid)` | defense.c | per render/HUD |
| `def_struct_repair(g, id, hp)` | defense.c | clamp a hp_max, no su crollate |
| wiring kill→bio, ricarica, mortaio→store, HUD convertitore | vat_horde | §3 |
| droni | vat_horde | §8, v2, pura fiction |

Core sim: **zero API nuove**.

## 11. Piano di verifica

- `test_bio.c` (headless): (1) rese — N kill accumulano nel tank, soglia →
  item, tank scalato del costo esatto; (2) spreco — store al cap + kill →
  tank invariato (biomassa persa), HUD-flag esposto; (3) cambio output a
  metà progresso — tank persiste, il nuovo item esce al SUO costo; (4)
  take — consuma 1, a zero rifiuta; (5) upgrade — costo scende con k_conv,
  si ferma al floor; ramo cap sale e si ferma al tetto; (6) determinismo —
  stessa sequenza ⇒ stesso stato.
- `test_turret_mag` (o caso nuovo in test_defense): (1) mag_size 0 =
  comportamento legacy INVARIATO (guardia di regressione, confronto
  kill-count su scenario M5 esistente); (2) mag N → N colpi poi silenzio
  per reload_s esatti, poi riprende; (3) `reload_now` azzera il countdown e
  riempie; (4) flame/acid decrementano a tick; (5) il lure si spegne
  durante la ricarica; (6) determinismo.
- Banco visivo: scena assalto con 2 torrette a caricatore corto — si
  osserva il ciclo fuoco/silenzio/ripresa, la ricarica istantanea da store,
  lo spreco a stoccaggio pieno (HUD), il mortaio che rifiuta senza colpi.

## 12. Questioni aperte (da sciogliere con l'utente)

1. **Tank al cambio output**: proposta = PERSISTE (è biomassa grezza, §4).
   Alternativa severa: si azzera (punisce lo switch, ma frustra).
2. **Mortaio**: consuma SOLO colpi prodotti (proposta: sì — senza colpi non
   spara; parte con lo store pieno) o ha anche un colpo "gratis" a cooldown
   lungo?
3. **Targeting del kit riparazione**: click sulla struttura col kit attivo
   (stile pennello)? o "ripara la struttura più danneggiata nel raggio
   base"? La v1 più semplice è il click.
4. **Ricarica istantanea — input**: click sulla torretta / tasto R su
   hover? (La torretta in ricarica va evidenziata comunque, §5.)
5. **HUD convertitore in assalto**: v1 a TASTI (cicla output) + contatori
   in un angolo; il pannello vero arriva con la UI del Blocco 3/fase G.
6. **PREP**: il convertitore lavora anche in PREP (kill di prova non
   esistono, quindi di fatto no) — gli store PARTONO pieni, vuoti, o
   configurabili per missione? Proposta: configurabile nel `.scn`
   (`biostock N`), default 1 di ogni munizione.

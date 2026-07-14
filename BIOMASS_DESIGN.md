# Biomassa — economia di partita (design tecnico)

> **STATO: v2 IMPLEMENTATA 2026-07-14** (§1–§6, §8–§10). La v1 (convertitore
> a output selezionabile + store per item) è stata SMONTATA: la sua UX era
> bocciata sul campo — barra d'assalto affollata (azioni del giocatore
> mescolate alle card di produzione) e output fisso da sorvegliare proprio
> mentre l'attenzione deve stare sull'orda. La v2 elimina il convertitore:
> **biomassa = valuta unica, spesa diretta a domanda**.
>
> Nel codice: `bio.c/.h` riscritti (serbatoio + `bio_add`/`bio_take`),
> `def_turret_set_facing` in defense, `biotank` in scene.c (`biostock`
> parsato e ignorato con warning), barra a 3 verbi + riparazione a
> mantenimento + REGOLA + barra di reload world-space in `vat_horde`;
> `test_bio` riscritto, caso REGOLA in `test_turret_mag`.
> **RESTA DA FARE: §7 (pannello upgrade al debrief)** — aspetta gli assi
> torretta del Blocco 3 e la risposta a Q2 (persistenza in campagna); oggi
> `tank_cap` si alza solo da scena/`bio_set_cap`.
>
> Parti v1 sopravvissute invariate: caricatori torretta (§5, con
> `reload_s` 12 s), `def_struct_repair`, rese per body, separazione
> budget $ / biomassa.

## 1. Principio (v2)

Ogni zombie ucciso frutta **biomassa**, che confluisce in un unico
**serbatoio** con capienza massima (default **500**). A serbatoio pieno i
kill vanno **sprecati**: la pressione a spendere durante l'assalto resta,
ma è tutta su UN numero leggibile a colpo d'occhio — niente più output da
selezionare, niente store per item, niente previsione ("che munizione mi
servirà tra 30 secondi?").

La biomassa si spende **direttamente sulle azioni**, nel momento del
bisogno:

- **MORTAIO** — un colpo costa N biomassa (niente scorte di colpi);
- **RIPARA** — 1 biomassa = 1 HP su una struttura danneggiata;
- **REGOLA** — riorienta la direzione principale di una torretta
  (verbo di gameplay NUOVO: oggi il facing è inchiodato al piazzamento);
- **ricarica istantanea** — click su una torretta in ricarica = caricatore
  pieno subito, a costo per kind.

Gli **upgrade escono dall'assalto**: si comprano a FINE LIVELLO (debrief)
con la biomassa residua del serbatoio — incluso l'upgrade della capienza
del serbatoio stesso. Questo crea la tensione strategica voluta: ogni
colpo di mortaio e ogni riparazione hanno un costo-opportunità (biomassa
che non diventerà upgrade). Si gioca la partita tattica in assalto e la
partita strategica nel momento calmo.

Due valute, due fasi, come in v1: il **budget** ($) resta la valuta del
PIAZZAMENTO in PREP; la **biomassa** è la valuta dell'assalto (e del
debrief). Non si convertono.

## 2. Cosa cambia rispetto alla v1

| Pezzo v1 | Destino v2 |
|---|---|
| Convertitore (output selezionato, soglie, `BioProducedFn`) | **ELIMINATO** |
| Store per item + cap 3 + "STOCCAGGIO PIENO" | **ELIMINATI** (il buffer è il serbatoio) |
| 7 card cliccabili + tasto O in barra | **ELIMINATI** → 3 pulsanti-verbo (§4) |
| Item `BIO_AMMO_*` / `BIO_MORTAR` / `BIO_REPAIR` / `BIO_UPGRADE` | **ELIMINATI** → costi diretti in biomassa (§6) |
| Upgrade ricorsivo in assalto (§7 v1, kit) | **SPOSTATO** al debrief (§7), curva a costi crescenti |
| Serbatoio (tank) | RESTA, promosso a valuta unica; guadagna il CAP (500) |
| Rese per body (walker 1, tank 8, obeso 2…) | RESTANO |
| Caricatori torretta (`mag_size/mag/reload_s/reload_t`) | RESTANO, `reload_s` default 5 → **12 s** (§5) |
| `def_turret_reload_now` / `def_turret_reloading` | RESTANO (la ricarica manuale li usa pari pari) |
| `def_struct_repair` | RESTA (la modalità RIPARA lo chiama a flusso) |
| `biostock N` nel `.scn` | **SOSTITUITO** da `biotank` (§8) |
| Droni (fiction raccolta, §8 v1) | RESTANO v2+ (invariati: credito al kill, scenografia) |

## 3. Il serbatoio

    tank += resa[body]     a ogni kill, clamp a tank_cap (oltre = SPRECO)
    tank -= costo          a ogni azione (rifiutata se tank < costo)

- `tank_cap` default 500, upgradabile al debrief (§7).
- HUD: barra/numero del serbatoio sempre visibile in assalto; quando un
  kill viene sprecato (tank al cap) la barra lampeggia — lo spreco resta
  INFORMATO come in v1, ma non chiede nessuna azione correttiva oltre a
  "spendi".
- La biomassa parte da `biotank start` della scena (default 0) e NON si
  azzera tra PREP e assalto (in PREP semplicemente non arrivano kill).

## 4. La barra d'assalto: solo verbi

La riga dei comandi mostra SOLO le azioni del giocatore, ognuna un
pulsante con costo/stato:

    [ MORTAIO (M) — 40 bio ]  [ RIPARA (R) — 1 bio/HP ]  [ REGOLA (V) ]
    [ serbatoio: ███████░░ 340/500 ]

Ogni verbo è una **modalità esclusiva** (attivarne una spegne le altre),
si esce con RMB o col tasto/click di toggle — pattern già rodato dal
mortaio v1. In modalità il puntatore cambia **icona disegnata accanto al
cursore** (quad/glifi del layer UI, niente pipeline asset né SDL cursor):

- **MORTAIO** (tasto M): come oggi — X di mira, anelli di gittata, LMB
  programma il colpo. Unica differenza: al click riuscito `bio_take(costo)`
  invece del colpo da store; senza biomassa il click rifiuta (suono "no").
- **RIPARA** (tasto R): icona martello. Il bersaglio si risolve con
  l'hover-inspect (piani di quota: prende torrette e muri su tutta la
  sagoma, fix 2026-07-12). **Tieni premuto LMB su una struttura
  danneggiata → flusso continuo biomassa→HP** a `repair_rate` (default
  100 HP/s), 1 bio = 1 HP; si ferma da solo a `hp_max`, a serbatoio vuoto,
  o al rilascio. Il flusso (invece del click one-shot) rende la spesa
  granulare: con serbatoio 500 e torrette da 250 HP una riparazione piena
  è mezzo serbatoio, il giocatore deve poter dosare. Mai su strutture
  crollate (regola v1 invariata: il crollo è definitivo).
- **REGOLA** (tasto V): icona a V (evoca il cono). Click su una torretta
  → il cono di mira appare ancorato alla torretta e segue il mouse (stesso
  gesto e stesso codice del piazzamento direzionale: drag orienta,
  rilascio committa il nuovo facing; RMB annulla). Gratis in v1 (manopola
  `adjust_cost` se in taratura girare le torrette gratis si rivelasse
  troppo forte). Il cono di direzione vive SOLO qui e nel piazzamento:
  l'hover-inspect non lo mostra più (tolto 2026-07-12, era rumore).

Fuori da ogni modalità, il click sul mondo resta camera (pan/rotate) con
un'eccezione: **click su torretta in ricarica = ricarica istantanea**
(§5) — nessuna modalità dedicata, il bersaglio è autoevidente per via
della barra di reload.

## 5. Ricarica torrette: la penalità che dà senso alla spesa

Il ciclo v1 (caricatore → 5 s di silenzio → pieno) rendeva la ricarica
manuale INUTILE: il giocatore non fa in tempo a cliccare che la torretta
si è già ricaricata da sola. La v2 la rende una decisione:

- `reload_s` default **12 s** (per kind, in balance.cfg — comunque
  "molto più di 10": la finestra deve far MALE, una torretta muta per 12
  secondi sotto pressione è una breccia che si apre).
- **Barra di reload automatica** sopra la torretta in ricarica:
  world-space, appare SOLO durante la ricarica e sparisce a caricatore
  pieno. (La decisione 2026-07-08 contro le barre HP world-space resta
  valida: quella era informazione permanente e ingombrante, questa è
  transitoria e actionable.) Il giocatore vede quanto manca e decide:
  quasi piena → aspetto gratis; appena iniziata e l'orda preme → pago.
- **Click sulla torretta in ricarica** = `bio_take(costo_kind)` →
  `def_turret_reload_now` (API v1 invariate). Costi per kind: light 25,
  heavy 35, flame 30, acid 30 (i numeri della tabella item v1, che erano
  già "costo in biomassa di una ricarica" — cambia solo che spariscono
  gli item intermedi).
- La canna grigia/abbassata durante la ricarica (render v1) resta; il
  lure spento in ricarica resta (la torretta muta non attira).

## 6. Costi (default, da tarare in sandbox)

| Azione | Costo default (biomassa) |
|---|---|
| Colpo di mortaio | 40 |
| Riparazione | 1 / HP (`repair_rate` 100 HP/s in mantenimento) |
| Ricarica light / heavy / flame / acid | 25 / 35 / 30 / 30 |
| Regola direzione | 0 (manopola `adjust_cost`) |

Rese per body invariate dalla v1: walker/runner 1, obeso 2, tank 8
(tabella per `DefBody` in balance.cfg). Tutti i numeri sono manopole §9.

## 7. Upgrade a fine livello (debrief)

Al debrief di un livello VINTO, la biomassa residua nel serbatoio si
spende in un pannello upgrade. Ci vanno:

- **capienza serbatoio** +100 per acquisto (la meta-risorsa: più margine
  di manovra al livello dopo);
- gli assi torrette del Blocco 3 (già decisi, il pannello è lo stesso —
  la valuta passa dai kit `BIO_UPGRADE` alla biomassa residua).

Curva a **costi crescenti** (es. serbatoio: 150, poi ×1.5 a livello, in
balance.cfg) — l'inverso della curva v1 (che scontava): qui la risorsa
arriva a fiumi e la curva deve frenare, non premiare. La v1 aveva
floor/tetto anti-degenerazione; qui il freno è la crescita del costo.

Cosa comprano gli acquisti e come persistono lungo la campagna
(campaign.txt, GAME_APP_DESIGN) è la questione aperta Q2 (§12).

## 8. Scene e balance

- `.scn`: `biostock N` (v1) **deprecato** — parsato e ignorato con
  warning, per non rompere le scene esistenti. Nuovo:

      biotank [start] [cap]     biomassa iniziale e capienza (default 0, 500)

- `balance.cfg` (Blocco 3), sezione `[biomass]` v2: rese per body,
  `tank_cap`, costi azione (mortaio, ricariche per kind, `repair_rate`,
  `adjust_cost`), curva upgrade (base e moltiplicatore per voce);
  `[turret]` tiene `mag_size`/`reload_s` per kind (default 12 s).

## 9. API (game-side; core sim: zero API nuove)

| API | Dove | Note |
|---|---|---|
| `bio.h/.c` v2: `bio_init(start,cap)`, `bio_add` (ritorna lo sprecato, per il flash HUD), `bio_take(n)`, `bio_tank/cap` | riscrittura | via output/store/item/upgrade-ricorsivo; il modulo resta zero-dep e deterministico |
| `def_turret_set_facing(g,tid,ang)` | defense.c | REGOLA: trasla `arc_min/arc_max` mantenendo l'ampiezza; `ang` diventa il nuovo centro. La torretta ri-acquisisce da sola |
| caricatori + `reload_now`/`reloading` + `def_struct_repair` | defense.c | INVARIATI dalla v1 |
| host: 3 pulsanti-verbo, cursori-icona, riparazione a mantenimento, barra reload world-space, click-ricarica, pannello upgrade nel debrief | vat_horde | §4/§5/§7; si smonta la UI card (§13) |

## 10. Piano di verifica

- `test_bio` riscritto (più corto della v1): (1) add accumula e clampa al
  cap, ritorna lo sprecato esatto; (2) take scala e rifiuta sotto zero;
  (3) determinismo.
- `test_turret_mag`: già copre il ciclo colpi→silenzio→ripresa,
  `reload_now`, tick flame, lure spento, guardia legacy `mag_size 0` —
  resta valido pari pari (cambia solo il default di `reload_s` nelle
  scene, non nel test). Caso NUOVO: `def_turret_set_facing` — ampiezza
  arco conservata, acquisizione nel nuovo settore, determinismo.
- Banco visivo (scena assalto, caricatori corti): barra di reload che
  appare/sparisce; click-ricarica che costa; mortaio che rifiuta a
  serbatoio scarso; riparazione a mantenimento che dosa e si ferma a
  hp_max; REGOLA che gira una torretta e il fuoco segue; flash di spreco
  a serbatoio pieno; debrief con pannello upgrade.

## 11. Droni (fiction, v2+ — invariato dalla v1)

Il credito biomassa avviene AL KILL, istantaneo: i droni restano pura
scenografia della raccolta (billboard dalla base ai cadaveri freschi,
"aspirano", tornano). NON toccano `danger` né il ruolo ostacolo dei
cadaveri. Se un giorno la biomassa dovesse legarsi alla raccolta FISICA
(cadaveri lontani = biomassa persa), è un cambio di regola in bio.c, non
di architettura.

## 12. Questioni aperte

1. **Riparazione a mantenimento vs click**: la spec dice mantenimento
   (flusso, dosabile). Se all'atto pratico il channel risultasse rognoso
   (eventi mouse + fixed timestep), fallback v1: click = ripara
   `min(hp_mancanti, tank)` in un colpo. Da decidere in sandbox.
2. **Persistenza upgrade lungo la campagna**: gli acquisti del debrief
   valgono solo il livello dopo o per sempre? E la biomassa residua NON
   spesa si perde (proposta: sì, use-it-or-lose-it — tiene i livelli
   autocontenuti e rende il pannello una decisione, non un salvadanaio)?
3. **REGOLA in PREP**: il facing si sceglie già al piazzamento; serve
   anche il ri-orientamento gratuito in PREP (comodità) o solo in
   assalto? Proposta: anche in PREP, stesso gesto.
4. **Costo mortaio vs riparazione**: 40 bio/colpo contro 250 bio per
   rimettere in piedi una torretta — il rapporto giusto si vede solo
   giocando; annotare in balance.cfg che sono le due manopole da tarare
   INSIEME (comprano entrambe "sopravvivenza adesso").

## 13. Smontaggio della v1 (FATTO, 2026-07-14)

1. ✅ `bio.c/h`: via output/store/item/costi per item/upgrade ricorsivo/
   callback produced; dentro `bio_init(start,cap)`, `bio_add` (ritorna lo
   sprecato) e `bio_take(cost)` su valuta unica. `test_bio` riscritto.
2. ✅ defense.c: default `reload_s` 12 s (lato host) + `def_turret_set_facing`
   (arco traslato, ampiezza conservata). Nessun altro tocco.
3. ✅ vat_horde: via le 7 card, il tasto O, i rami upgrade in barra, gli store
   del mortaio; dentro i 3 pulsanti-verbo + cursore-icona + barra di reload
   world-space + riparazione a mantenimento + REGOLA (assalto E prep, Q3).
   I costi stanno in testa al file (`BIO_MORTAR_COST`, `BIO_REPAIR_RATE`,
   `BIO_RELOAD_COST[]`, `BIO_ADJUST_COST`) finché non arriva balance.cfg.
4. ✅ scene.c: `biotank [start] [cap]`; `biostock` parsato, ignorato, warning.
   `gfx/export_scn.py`: custom property `biotank` (+ `biotank_cap`).
5. ✅ CLAUDE.md e questo doc aggiornati.
6. ⏳ Pannello upgrade al debrief (§7): rimandato al Blocco 3 (serve avere
   assi torretta da comprare) + decisione Q2 sulla persistenza.

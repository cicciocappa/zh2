# Cadaveri come fattore di gioco: design tecnico

> **DESIGN / PROPOSTA** (non ancora implementato). Estende i cadaveri-ostacolo di
> M3.3 (`simp_corpse_*`) da semplice barricata temporanea a *fattore strategico*:
> i mucchi cedono sotto il calpestio (non sono difesa permanente) e, se crescono
> troppo contro un muro, formano una **rampa** che lascia scavalcare — un pericolo
> da gestire, non un alleato gratis. Apre una distinzione di gameplay fra difese
> che **producono** cadaveri (mitragliatrici, tagliole) e difese che **non** ne
> producono o li **rimuovono** (fuoco, acido, esplosivi).

Principio guida (come M3 e l'assedio): **il core resta gameplay-agnostico.** Il
core espone *fisica e campi* — quanto è alta la pila di cadaveri per cella, quanto
è compattata — riempiti nello stesso loop in cui già esistono i cadaveri. HP delle
strutture, tipi d'arma, politica di rimozione, soglia di scavalco e denaro vivono
nel codice di gioco. Tutto piatto, mappabile su compute shader come gli altri campi
per-cella (densità, jam).

Meccanica sorella: l'assedio frontale (`SIEGE_DESIGN.md`) e la rampa sono **due
vettori ortogonali di sconfitta dello stesso muro** — il muro può *cadere*
(assedio, HP→0) o essere *scavalcato* (rampa, altezza→`wall_h`). Vedi §7.

---

## 1. Il problema

I cadaveri di M3.3 sono dischi **a massa infinita** (`invm = 0`) con un **TTL** a
orologio (pool `CORPSE_CAP = 4096`). Funzionano da barricata: `test_corpses`
mostra 9 cadaveri che sigillano *del tutto* un corridoio da 4 m (0 drain). Già oggi
pesano sul flow field via densità/jam (`CORPSE_RHO = 2.0` aggiunto a `rho_raw` e
`jam_raw` per cella), ma **non** sono marcati solidi nella nav: la deviazione
attorno ai mucchi emerge dal PBD + dal costo di densità.

Con orde da 50k–100k, i morti sono migliaia. Due derive da evitare:

1. **Difesa permanente.** Un muro foderato di cadaveri all'esterno diventa
   inattaccabile (l'orda non lo tocca più) e la sfida è rovinata. Il TTL lo evita
   solo a orologio: grezzo, scollegato dalla pressione reale, e a regime un muro
   ben difeso *rigenera* il mucchio più in fretta di quanto il TTL lo smaltisca.
2. **Sigillo totale.** La massa infinita ferma l'orda netta a un varco. Vogliamo
   che un mucchio **rallenti molto ma non fermi del tutto**: la prima ondata
   inciampa, poi i corpi vengono calpestati, schiacciati, e oppongono via via meno
   resistenza.

## 2. Il modello: due variabili per cella, che tirano in direzioni opposte

Chiave del design: **accumulo** e **compattazione** sono grandezze separate. Se le
colleghi alla stessa variabile ottieni un comportamento confuso (calpestare
dovrebbe sia abbassare la resistenza orizzontale *sia* costruire una rampa: sono
effetti opposti). Due campi per **cella nav** (stessa risoluzione di `rho`/`jam`):

```c
float corpse_mass[cell];   /* volume di corpi accumulato. + a ogni morte,
                              decade lento nel tempo. → ALTEZZA della pila.   */
float corpse_pack[cell];   /* compattazione. + col calpestio (agenti che
                              passano sopra). → riduce resistenza orizzontale
                              e schiaccia l'altezza.                          */
```

Da questi due, una grandezza derivata che il gioco legge:

```c
height = k_h * corpse_mass / (1 + k_pack * corpse_pack);
```

- **`corpse_mass`** sale quando si aggiunge un cadavere (`simp_corpse_add` accumula
  il *volume* del disco, ∝ r², nella sua cella) e **decade lentamente** nel tempo
  (mezza vita lunga, ordine decine di secondi: i corpi marciscono / vengono
  rimossi). È ciò che il giocatore deve **temere quando cresce**.
- **`corpse_pack`** sale a ogni agente *a terra* che attraversa una cella con
  `corpse_mass > 0` (nello stesso istogramma di `rho`/`jam` — costo quasi nullo,
  siamo già nel loop). Schiaccia: riduce l'altezza (denominatore) **e** abbassa la
  resistenza orizzontale offerta dai corpi. Decade anch'esso (i corpi sotto
  ricominciano a "gonfiare" se smettono di calpestarli).

Risultato leggibile nel tempo:

| Stato pila | mass | pack | Effetto |
|---|---|---|---|
| Fresca, alta, poco pestata | alto | basso | **rampa** (pericolo); resistenza orizz. piena |
| Vecchia, molto pestata | alto | alto | quasi neutra orizz.; rampa bassa (schiacciata) |
| Smaltita | →0 | →0 | terreno libero |

**Dove nasce il pericolo, da solo.** A un *varco aperto* il traffico passante
calpesta forte → `pack` alto → si appiattisce: niente rampa permanente al
chokepoint. A un *muro assediato* gli zombie premono ma **non passano** (il muro
blocca): poco calpestio, `pack` basso, `mass` che cresce coi kill delle torrette
dietro → la pila resta alta → **rampa**. La meccanica prende di mira esattamente i
muri tenuti, senza casi speciali: emerge dalla geometria.

## 3. Rallentare senza fermare: massa finita

Per il rimedio "rallentano ma non fermano" bisogna mollare la **massa infinita**
attuale. Opzioni (da verificare in sandbox):

- **(a) Massa grande ma finita** sui cadaveri (`invm` piccolo non-zero, scalato da
  `corpse_pack`: più pestati → più cedevoli). Il PBD già lavora a rapporti di
  massa (M3.5): l'orda che spinge da dietro lentamente *sfonda* il mucchio invece
  di fermarsi netta. Cambio rispetto a `test_corpses` (dove sigillano del tutto):
  **da mettere in conto**, il test va aggiornato (sigillo → rallentamento).
- **(b) Raggio che cala con `pack`**: i corpi calpestati si "spalmano" e lasciano
  filtrare. Più semplice, ma meno fisico del momento che sfonda.

Raccomando **(a)**: si appoggia al sistema di masse esistente e dà il
comportamento "spinta della folla che sfonda" coerente con il resto del core.

## 4. La rampa: riusa volo e reroute (quasi gratis)

`height[cell]` adiacente a una cella-muro che si avvicina a `wall_h` (2 m, l'altezza
che i flyer già sorvolano) = la pila ha raggiunto la cima del muro. Due strade:

- **(a) Livello nav (consigliato).** Quando `height ≥ wall_h` su una cella a ridosso
  di un muro, il **gioco** rende quella cella-muro attraversabile
  (`simp_set_wall(s, cx, cy, false)`, esattamente come una breccia d'assedio). La
  modifica del terreno **forza il commit completo della nav** → l'orda si re-instrada
  *sopra* la rampa da sola. Riusa interamente la macchina di reroute dell'assedio
  (§4 di `SIEGE_DESIGN.md`). Il muro fisico resta: la "passabilità" è l'astrazione
  nav della salita. Visivamente lo sprite mostra l'arrampicata.
- **(b) Livello particellare (fedeltà visiva).** Gli agenti su una cella con
  `height > 0` ricevono un offset `z = height` (camminano *sulla* pila); chi cresta
  oltre `wall_h` può scavalcare via il percorso di volo (`SIMP_FLYING` ignora i muri
  sopra `wall_h`). Più bello ma più stato; tenuto come evoluzione.

Quando il calpestio/decadimento riabbassa `height` sotto soglia, il gioco richiude
la cella (o l'assedio la riapre): la rampa **non è permanente**, è un timer come
l'HP del muro.

## 5. API del core (proposta)

Due aggiunte, entrambe campi per-cella già naturali nel motore:

```c
/* Altezza della pila di cadaveri per cella nav (m). Derivata da mass/pack,
 * aggiornata a fine step. Il gioco la confronta con wall_h per la rampa.
 * Indicizzata per cella nav come rho/jam (NON per agente: niente problema
 * di indici instabili qui). */
const float *simp_corpse_height(const SimP *s);

/* Rimuove cadaveri (e ne azzera mass/pack) in un cerchio: fuoco/acido che
 * "smaltiscono" il mucchio. Game-facing per le difese senza-cadaveri di §6. */
void simp_corpse_clear(SimP *s, float x, float y, float radius);
```

Stato interno nuovo (due float per cella nav, accanto a `rho`/`jam`):
`corpse_mass[]`, `corpse_pack[]`, più i decadimenti nel loop di fine step (5b, dove
già vive il decay del TTL). `simp_corpse_add` accumula `π·r²` nella cella; il loop
di istogramma (già scorre gli agenti a terra) incrementa `corpse_pack` sulle celle
calpestate. **Zero malloc nello step**, due array piatti in più. Il TTL resta come
fail-safe (un corpo non calpestato sparisce comunque, alla lunga).

## 6. Distinzione fra armi (lato gioco)

La parte che trasforma i cadaveri da fastidio in **decisione strategica**. Tabella
d'arma (di gioco), nessun supporto nel core oltre `simp_corpse_add`/`clear`:

| Difesa | Cadaveri | Note |
|---|---|---|
| Mitragliatrice, tagliola, lama | **producono** (come il pennello KILL: ~30% lascia un corpo) | DPS alto ma *costruiscono il problema* |
| Esplosivo | nessuno (gib → impulso, non corpo) | usa `simp_apply_impulse`, sbalza |
| **Fuoco, acido** | nessuno **+ rimuovono gli esistenti** (`simp_corpse_clear`) | cremazione/dissoluzione |

Il loop di gioco si chiude: una mitragliatrice efficiente al varco *costruisce* la
pila; il giocatore deve prevedere un emettitore di fuoco/acido a copertura per
tenere `height` sotto `wall_h`. Trade-off reale — efficienza istantanea vs gestione
dell'accumulo — e identità alle difese. "Eliminare i mucchi" non è micro tedioso a
parte: è uno strumento già nel kit (fuoco/acido), con doppia funzione (danno +
pulizia).

## 7. Relazione con l'assedio (due vettori sul muro)

Assedio frontale (`SIEGE_DESIGN.md`) e rampa corrono **in parallelo** sullo stesso
muro: HP→0 lo abbatte, `height`→`wall_h` lo fa scavalcare. Rischio di **rumore**: il
giocatore deve capire quale minaccia sta vincendo. Mitigazioni di design:

- I due tendono a separarsi per geometria: l'assedio morde i **varchi/sezioni
  corte** dove la pressione frontale converge; la rampa cresce sui **muri lunghi
  tenuti** dove i kill si accumulano senza calpestio passante. Spesso non si
  sommano sullo stesso punto.
- Feedback visivo distinto: il muro che si **scheggia** (assedio) vs la pila che
  **cresce e cambia** (rampa). Il GFX_DESIGN ha già gore + cadaveri come layer →
  l'altezza della pila è renderizzabile (più corpi accatastati). Da trattare come
  **requisito di leggibilità**, non abbellimento: la rampa è punitiva se non la
  vedi arrivare.

## 7-bis. Ruolo anti-imbuto strategico (anti-degenere) — PERNO scelto

Oltre alla fedeltà fisica, l'accumulo cadaveri è lo **strumento principale contro
una semplificazione strategica**: "barrico una strada e ammasso *tutte* le torrette
sull'altro accesso che lascio aperto". Senza contromisure è la soluzione facile e
dominante. La contromisura **emergente** (preferita a una regola che proibisce):
il kill-zone aperto si **riempie di cadaveri** → si intasa → il flow **re-instrada
l'orda** verso il fianco lasciato scoperto (la strada barricata) → l'orda preme e
sfonda la barricata, dove le torrette non ci sono. **Il successo del giocatore si
auto-limita** attraverso la simulazione. Un'unica meccanica unifica tre cose che
erano TODO separati: *accumulo cadaveri* + *distruzione indiretta per attrito* +
*anti-imbuto*. Effetto collaterale voluto: crea **ritmo** (l'imbuto regge, poi si
intasa, poi il fronte si sposta; il decay riapre → oscilla, non death-spiral se
tarato) → premia il gioco adattivo, scoraggia il turtling.

> **Nota tecnica sul reroute (importante).** Il peso `CORPSE_RHO` su `rho`/`jam`
> resta **sotto `WALL_ENTER`** (§I.4 di `SIMULAZIONE.md`): da solo re-instrada
> l'orda **solo fra strade APERTE**. Per spingerla a **sfondare una barricata**
> (non solo a prendere un'altra strada aperta) il costo di una cella *davvero*
> intasata deve poter salire fino a livelli **comparabili a un muro** — una pila
> densa = un "muro di cadaveri" nella nav — così da competere col **costo di
> sfondamento per-cella** della barricata (più basso di un palazzo, vedi
> `M5_DESIGN.md` §7 e TODO core nav): allora il Dijkstra preferisce la barricata e
> l'orda devia lì. È la versione *continua* della leva discreta di §4(a) (rendere
> la cella attraversabile/solida): qui `corpse_mass` alza il costo fino a soglia
> muro. **Le due feature sono gemelle** — l'anti-imbuto via cadaveri NON funziona
> contro un giocatore che barrica, finché il costo cadaveri non può raggiungere la
> scala del costo-muro. Da progettare insieme.

Le altre leve discusse stanno **lato gioco**, complementari ma secondarie: costo
nav additivo sotto le torrette (nudge morbido — ma se l'alternativa è *sfondare*
una barricata serve costo > `WALL_ENTER`, rischio di affamare le torrette →
dose mite); torrette meno efficaci se ravvicinate (rete di sicurezza, ma appiattisce
il puzzle spaziale); screamer "intelligenti" che cercano il path più sicuro su un
campo di pericolo e guidano l'orda (counterplay di flavor, secondo giro di
pathfinding sui *pochi* screamer — più avanti).

## 8. Verifica — `test_corpse_pile.c` (piano)

Headless, sulla falsariga di `test_corpses`/`test_siege`. Quattro parti:

1. **Cedimento per calpestio (rimedio 1).** Corridoio con un mucchio fresco; orda
   che spinge. Assert: il drain è ~0 nei primi N step (mucchio fresco rallenta
   forte), poi *cresce monotòno* man mano che `corpse_pack` sale e l'orda sfonda —
   contro il controllo a massa infinita (drain piatto a 0). Misura `corpse_pack`
   medio che sale e `height` che cala sotto calpestio.
2. **Rampa (rimedio 2).** Muro che sigilla il goal; torretta (kill scriptati)
   dietro che accumula corpi contro il muro mentre l'orda preme (poco calpestio →
   `pack` basso). Assert: `corpse_height` sulla banda cresce monotòno fino a
   `wall_h`; allo scavalco (cella aperta + reroute) il drain passa da 0 a >0.
   Controllo: stessa scena ma con `simp_corpse_clear` periodico (fuoco) → `height`
   tenuto sotto soglia → 0 scavalco, assedio puro.
3. **Decadimento → non permanenza.** Mucchio piazzato, *nessun* nuovo kill: `mass`
   decade, `height`→0, il varco si riapre da solo (no difesa permanente).
4. **Determinismo** (due run identiche: stessi campi, stesso step di scavalco) e
   **zero NaN**.

Sweep da misurare: `k_h`/`k_pack`/half-life del decay vs (step di scavalco, % drain),
per dimensionare le manopole. Sandbox: pennelli fuoco/acido (clear) e overlay
ciclico `corpse_height` (sopra gli overlay densità/jam esistenti) per vedere la
pila crescere.

## 9. Note, trappole, mappatura GPU

- **Indici stabili non servono qui**: i campi sono per *cella nav*, non per agente —
  niente trappola degli indici densi (a differenza dell'assedio). I cadaveri non
  hanno handle per design (M3.3): il gioco non li punta.
- **Cambio rispetto a `test_corpses`**: la massa finita (§3) rompe il sigillo
  totale verificato lì. Il test va aggiornato (sigillo → rallentamento progressivo);
  documentare la rottura voluta.
- **`CORPSE_RHO` resta**: i corpi pesano già su `rho`/`jam` (deviazione del flow). La
  rampa è ortogonale (verticalità via `height`), non sostituisce il costo di densità.
- **Costo**: due float/cella nav in più (`mass`, `pack`) aggiornati nei loop già
  esistenti (istogramma a fine step, decay in 5b). `simp_corpse_clear` = una
  scansione del pool nel raggio (come gli impulsi brute force). Trascurabile.
- **GPU**: `corpse_mass`/`pack` sono campi per-cella identici a `rho`/`jam` →
  texture/SSBO, aggiornati nel kernel d'istogramma. `corpse_height` = un kernel
  pointwise. La decisione di scavalco (soglia → edit terreno) è CPU/gioco, rara.
- **Soglia di scavalco vs spessore del muro**: un muro di più celle non si scavalca
  con una sola cella di rampa al piede. La logica di gioco deve richiedere `height ≥
  wall_h` su una cella che confina con una cella-muro *e* aprire la cella di sbocco
  oltre (o trattare il muro spesso come non scavalcabile dai cadaveri — scelta di
  level design).

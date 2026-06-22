# Cadaveri come fattore di gioco: design tecnico

> **STATO (2026-06-23):** il **perno nav §7-bis** è IMPLEMENTATO (accumulo
> `corpse_mass`/`corpse_pack` per cella + `corpse_height` derivata + termine
> d'arco `k_corpse·min(height/wall_h,1)` a scala-muro; API `simp_corpse_height`
> / `simp_corpse_clear`; 6 param in `SimPParams`, default on; `test_corpse_pile`
> PASS). **Massa finita §3 (a) IMPLEMENTATA** (2026-06-23, `corpse_weight`
> default 40): l'orda shova la pila e sfonda invece di restare sigillata —
> "rallentano ma non fermano". RESTA lo scaling per `corpse_pack` (cedimento
> progressivo, l'`invm` non dipende ancora dal pack) e la tabella armi §6 (lato
> gioco). Il packing §2 alimenta già il costo nav (height §7-bis); il decay tiene
> le pile non-permanenti.
>
> **DECISIONE DI DESIGN (2026-06-23): la RAMPA §4 è TAGLIATA come meccanica.**
> Due vettori di sconfitta sullo stesso muro (assedio HP→0 *e* scavalco
> altezza→`wall_h`) costano troppo in leggibilità per il guadagno: il giocatore
> dovrebbe leggere *quale* minaccia sta vincendo, con due feedback distinti. E
> non serve: l'anti-degenere ("difesa permanente di cadaveri", "imbuto su un solo
> accesso") è **già coperto dal reroute** di §7-bis (la pila alza il costo nav →
> l'orda devia a cercare un varco più economico) + dal decay. Lo stallo su un
> muro *indistruttibile* tenuto a fuoco si rompe meglio coi **flyer** (il volo
> balistico già scavalca `wall_h`) o nemici speciali, non con la rampa. Quindi:
> UN solo modello mentale — *"i mucchi sono una minaccia crescente: puliscili
> (fuoco/acido) o non crearli sotto le torrette"* — e UN solo verbo per il
> giocatore. Lo "scavalcano i cadaveri" resta come **pura fiction visiva**
> (render, più avanti), senza regole. Vedi §4 e §7 riscritti.
>
> Estende i cadaveri-ostacolo di
> M3.3 (`simp_corpse_*`) da semplice barricata temporanea a *fattore strategico*:
> i mucchi nei varchi-killzone si intasano e **deviano l'orda** (non sono difesa
> permanente) — un pericolo da gestire, non un alleato gratis. Apre una
> distinzione di gameplay fra difese che **producono** cadaveri (mitragliatrici,
> tagliole) e difese che **non** ne producono o li **rimuovono** (fuoco, acido,
> esplosivi).

Principio guida (come M3 e l'assedio): **il core resta gameplay-agnostico.** Il
core espone *fisica e campi* — quanto è alta la pila di cadaveri per cella, quanto
è compattata — riempiti nello stesso loop in cui già esistono i cadaveri. HP delle
strutture, tipi d'arma, politica di rimozione e denaro vivono nel codice di gioco.
Tutto piatto, mappabile su compute shader come gli altri campi per-cella
(densità, jam).

Rapporto con l'assedio: il muro si sconfigge per **assedio** (`SIEGE_DESIGN.md`,
HP→0). La pila di cadaveri NON è un secondo vettore di sconfitta del muro (la
rampa è tagliata, vedi banner e §7): è una pressione sulla **nav** che fa
**deviare** l'orda — opera sui varchi/strade, non sul muro. Vedi §7.

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

Chiave del design: **accumulo** (quanti corpi) e **compattazione** (quanto
calpestati) sono grandezze separate. Il calpestio deve poter **abbassare** la pila
(meno costo nav / meno altezza visiva) mentre i nuovi kill la **alzano**: due
spinte opposte sulla stessa cella → servono due variabili, o il comportamento si
confonde. Due campi per **cella nav** (stessa risoluzione di `rho`/`jam`):

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

`height` è la grandezza che il gioco/render legge: alimenta il **costo nav**
(§7-bis: pila alta → costo scala-muro → l'orda devia) ed è esposta per overlay e
fiction visiva. (La rampa di scavalco è tagliata: `height` NON apre più muri.)

Risultato leggibile nel tempo:

| Stato pila | mass | pack | Effetto |
|---|---|---|---|
| Fresca, alta, poco pestata | alto | basso | costo nav alto (l'orda devia il varco); resistenza orizz. piena |
| Vecchia, molto pestata | alto | alto | quasi neutra orizz.; costo nav basso (schiacciata) |
| Smaltita | →0 | →0 | terreno libero |

**Dove la pila morde, da sola.** A un *varco aperto* il traffico passante
calpesta forte → `pack` alto → si appiattisce: niente blocco permanente al
chokepoint. A un *killzone tenuto* gli zombie vengono falciati ma il flusso non
scorre: poco calpestio passante, `pack` basso, `mass` che cresce coi kill →
la pila resta alta → il **costo nav** sale e l'orda **devia** verso il fianco più
debole. La meccanica prende di mira esattamente i punti dove accumuli i kill,
senza casi speciali: emerge dalla geometria. (Con la massa finita §3 ora attiva,
il traffico che sfonda una pila la calpesta davvero → `pack` sale e abbassa la
height/costo nav; il decay resta la garanzia di non-permanenza. Il pack non
modifica ancora l'`invm` fisico — cedimento progressivo = prossimo sotto-passo.)

## 3. Rallentare senza fermare: massa finita — FATTO (2026-06-23)

> **STATO: opzione (a) IMPLEMENTATA, massa finita FISSA.** Param `corpse_weight`
> (unità walker, default **40**, `<=0` = sigillo infinito legacy). I cadaveri
> ghost passano da `invm = 0` a `invm = 1/(corpse_weight·r0²)` (come
> `simp_spawn_desc`): l'orda che spinge li SHOVA a valle e sfonda. Le posizioni
> spinte dal PBD sono riscritte nel pool a ogni step (step 3b di `simp_step`); le
> coppie cadavere-cadavere sono saltate (sennò una pila di dischi sovrapposti si
> farebbe esplodere). `test_corpses` aggiornato: B = sigillo infinito (drain 0,
> moved 0), C = §3 finito (drain 44 vs 164 aperto → rallenta molto ma LEAKA,
> moved 9). Suite 22/22 verde. Lo SCALING per `corpse_pack` ("più pestati → più
> cedevoli", curva di cedimento progressiva) è il **prossimo sotto-passo**:
> oggi il pack agisce già sul costo nav (height §7-bis) ma non sull'`invm`.

Per il rimedio "rallentano ma non fermano" bisogna mollare la **massa infinita**:

- **(a) Massa grande ma finita** sui cadaveri (`invm` piccolo non-zero, in
  prospettiva scalato da `corpse_pack`: più pestati → più cedevoli). Il PBD già
  lavora a rapporti di massa (M3.5): l'orda che spinge da dietro lentamente
  *sfonda* il mucchio invece di fermarsi netta. **SCELTA e implementata** (vedi
  banner). Cambio rispetto a `test_corpses` (dove sigillavano del tutto): test
  aggiornato (sigillo legacy + rallentamento §3 affiancati).
- **(b) Raggio che cala con `pack`**: i corpi calpestati si "spalmano" e lasciano
  filtrare. Più semplice, ma meno fisico del momento che sfonda. **Scartata.**

NOTA fisica: i cadaveri shovati NON ricevono la proiezione sui muri (il
`job_wall` gira solo sugli agenti `< count`): un corpo spinto contro un muro
laterale può restarci incastrato invece di scorrere. Accettabile (la pila si
sposta lungo il varco, in apertura); irrigidire = wall-projection anche per i
ghost, se servisse.

## 4. La rampa di scavalco — ~~meccanica~~ TAGLIATA (solo fiction visiva)

> **DECISIONE (2026-06-23): la rampa NON è una meccanica di gameplay.** Due
> vettori di sconfitta sullo stesso muro (assedio *e* scavalco) costano troppo in
> leggibilità — il giocatore dovrebbe distinguere quale sta vincendo — per un
> guadagno già coperto altrove: l'anti-degenere lo dà il **reroute** di §7-bis
> (la pila alza il costo nav → l'orda devia), la non-permanenza il decay, e lo
> stallo su muro indistruttibile lo rompono **flyer**/nemici speciali (il volo
> balistico già scavalca `wall_h`). Un solo modello mentale, un solo verbo
> ("pulisci i mucchi"). Sotto, l'idea originale, conservata solo come possibile
> **abbellimento visivo** (gli sprite che si arrampicano), SENZA effetto sulla nav.

Se mai si volesse la sola *fiction* (nessuna regola): gli agenti su una cella con
`height > 0` ricevono un offset di render `z = height` (camminano *sulla* pila);
puro renderer (`vat_layer`), il core non cambia, nessun muro diventa attraversabile.
L'idea scartata era invece di aprire la cella-muro a `height ≥ wall_h`
(`simp_set_wall(..., false)`, come una breccia d'assedio) — **non si fa**: sarebbe
il secondo vettore di sconfitta che vogliamo evitare.

## 5. API del core (proposta)

Due aggiunte, entrambe campi per-cella già naturali nel motore:

```c
/* Altezza della pila di cadaveri per cella nav (m). Derivata da mass/pack,
 * aggiornata a fine step. Alimenta il costo nav §7-bis (saturazione a wall_h) ed
 * è esposta per overlay e fiction visiva. Indicizzata per cella nav come
 * rho/jam (NON per agente: niente problema di indici instabili qui). */
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
tenere la pila bassa, o il **costo nav sale e l'orda devia** il suo killzone verso
il fianco scoperto (§7-bis). Trade-off reale — efficienza istantanea vs gestione
dell'accumulo — e identità alle difese. "Eliminare i mucchi" non è micro tedioso a
parte: è uno strumento già nel kit (fuoco/acido), con doppia funzione (danno +
pulizia).

## 7. Relazione con l'assedio (UN vettore sul muro, niente rampa)

> Versione precedente: "due vettori sul muro" (assedio HP→0 *e* rampa
> altezza→`wall_h`). **La rampa è stata tagliata** (vedi banner e §4): troppo
> costo di leggibilità — il giocatore avrebbe dovuto distinguere quale minaccia
> stesse vincendo lo stesso muro — per un guadagno già coperto dal reroute §7-bis.

Resta **un solo** modo di sconfiggere un muro: l'**assedio frontale**
(`SIEGE_DESIGN.md`, HP→0). La pila di cadaveri NON attacca il muro: agisce sulla
**nav** e fa **deviare** l'orda (§7-bis), una pressione che opera sui
varchi/strade davanti al muro, non sulla struttura. Niente ambiguità "crolla o lo
scavalcano": il muro o lo abbatti con l'assedio, o lo tieni — e se ammassi i kill
davanti, la pila ti **disperde l'orda** altrove (la gestisci con fuoco/acido).

Leggibilità (resta un requisito, più semplice ora): muro che si **scheggia** =
assedio; pila che **cresce** davanti a un varco = costo nav che sta per deviare
l'orda. Il GFX_DESIGN ha già gore + cadaveri come layer → l'altezza della pila è
renderizzabile (più corpi accatastati) e fa da segnale "questo killzone si sta
intasando". Eventuale fiction "ci si arrampicano sopra" = puro abbellimento, senza
effetto nav (§4).

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
> l'orda devia lì. `corpse_mass` alza il costo nav fino a soglia muro (saturando
> a `wall_h`, ma SOTTO `WALL_ENTER` → palazzi e assedi a goal murati intatti).
> **Le due feature sono gemelle** — l'anti-imbuto via cadaveri NON funziona contro
> un giocatore che barrica, finché il costo cadaveri non può raggiungere la scala
> del costo-muro. (Questo è IMPLEMENTATO, vedi banner; il costo di sfondamento
> per-cella `wall_cost` era già fatto.)

Le altre leve discusse stanno **lato gioco**, complementari ma secondarie: costo
nav additivo sotto le torrette (nudge morbido — ma se l'alternativa è *sfondare*
una barricata serve costo > `WALL_ENTER`, rischio di affamare le torrette →
dose mite); torrette meno efficaci se ravvicinate (rete di sicurezza, ma appiattisce
il puzzle spaziale); screamer "intelligenti" che cercano il path più sicuro su un
campo di pericolo e guidano l'orda (counterplay di flavor, secondo giro di
pathfinding sui *pochi* screamer — più avanti).

## 8. Verifica — `test_corpse_pile.c` (FATTO, PASS)

Headless, sulla falsariga di `test_jam`/`test_corpses`. Quattro parti (il pivot
nav è isolato pinnando `corpse_weight=0`, sigillo idealizzato, così la §3 non
interferisce). Il cedimento §3 vive invece in `test_corpses` (parte C):

1. **PIVOT anti-imbuto.** Muro con un varco diretto APERTO (intasato da una pila
   di cadaveri scriptata) e una BARRICATA (banda solida a `wall_cost` basso) come
   unica alternativa. Tre run, stessa fisica, varia solo il termine nav:
   `k_corpse=0,k_jam=0` (baseline: coda al varco, barricata intatta) ·
   `k_jam=8,k_corpse=0` (il jam da solo NON batte la barricata) ·
   `k_corpse=300` (la pila supera la barricata → reroute → l'orda preme la
   barricata). Misura: `simp_wall_pressure` sommata sulla barricata = 0 → 11
   (jam) → 114 (cadaveri). Assert: pressione cadaveri ≫ jam ≫ baseline.
2. **Decadimento → non permanenza.** Pila, nessun nuovo kill: `corpse_height`
   decade monotòno a ~0 (la pila marcisce, niente difesa permanente).
3. **Clear (fuoco/acido).** `simp_corpse_clear` rimuove i corpi + azzera i campi e
   riapre il varco: drain ~0 da intasato → >0 dopo il clear.
4. **Determinismo** (due run pivot identiche: stessa pressione/drain) + **zero NaN**.

NOTA: il **cedimento §3** (massa finita) è verificato in `test_corpses` parte C
(sigillo→leak). La **rampa** (§4) è tagliata. Lo scaling `corpse_pack`→`invm`
(cedimento progressivo) e l'overlay `corpse_height` in `vat_horde` = follow-up.

## 9. Note, trappole, mappatura GPU

- **Indici stabili non servono qui**: i campi sono per *cella nav*, non per agente —
  niente trappola degli indici densi (a differenza dell'assedio). I cadaveri non
  hanno handle per design (M3.3): il gioco non li punta.
- **Cambio rispetto a `test_corpses`**: con la massa finita §3 (default) il sigillo
  totale non vale più (sigillo → rallentamento). Il test ora copre ENTRAMBI: parte
  B = sigillo infinito (`corpse_weight=0`, legacy), parte C = leak §3 (default 40).
- **`CORPSE_RHO` resta**: i corpi pesano già su `rho`/`jam` (deviazione fra strade
  aperte). Il termine `k_corpse` su `height` è ortogonale e sale fino a scala-muro
  (battendo le barricate), non sostituisce il costo di densità.
- **Costo**: tre float/cella nav in più (`mass`, `pack`, `height`) aggiornati nei
  loop già esistenti (istogramma + decay/height pointwise a fine step).
  `simp_corpse_clear` = una scansione del pool nel raggio (come gli impulsi brute
  force). Trascurabile.
- **GPU**: `corpse_mass`/`pack`/`height` sono campi per-cella identici a
  `rho`/`jam` → texture/SSBO, aggiornati nel kernel d'istogramma + un kernel
  pointwise per height. Niente decisione di scavalco (rampa tagliata): solo il
  termine d'arco nel Dijkstra, come `k_density`/`k_jam`.

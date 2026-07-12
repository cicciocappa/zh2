# Cadaveri come fattore di gioco: design tecnico

> **SVOLTA (2026-06-25): il perno anti-choke passa da "cadaveri" a "sangue→paura".**
> Decisione dell'utente. L'anti-degenere ("creo un choke point, piazzo le difese e
> resto a guardare") NON è più affidato all'accumulo cadaveri→costo nav (§7-bis) né
> ai mucchi visivi: è affidato a un **istinto animale** che fa **evitare le pozze di
> sangue** degli altri zombie. Il sangue lo stampiamo GIÀ a ogni morte (decal), e il
> costo nav lo abbiamo GIÀ (`simp_add_cost`, `w>0` = paura): alla morte basta
> aggiungere un costo additivo sulla cella, decaduto come il sangue. Più semplice,
> disaccoppia gameplay e rendering, e non è degenere perché il PBD shova comunque il
> fronte nella killzone (l'orda grande inonda lo stesso, la sonda piccola devia).
> Conseguenze su questo documento:
> - **Mound 3D TAGLIATI (§10.2 tier 2, §10.3-10.6): RIMOSSI** da `vat_horde`
>   (`build_mound`/`mnd_*`/`build_corpse_mounds` + ombre ellittiche + `mound_shadow.vs`
>   eliminati, 2026-06-25). Erano brutti e renderli passabili costava troppo (serviva
>   di fatto una ragdoll). Il cadavere resta: mesh in posa di morte (§10.2 tier 1) →
>   sagoma a terra persistente (§10.7).
> - **Sangue→paura: FATTO (2026-06-25), GRADUATO a scala-muro (anti-killbox).**
>   Campo `danger` per cella (decade su `danger_hl`, default 30 s), termine d'arco
>   **`k_danger·min(danger/danger_ref,1)`** in `nav_phi_begin` (`k_danger`=tetto
>   scala-muro default 400, `danger_ref`=sangue per saturare default 8), API
>   `simp_add_danger(x,y,r,w)` + `simp_danger_arr`. Cablato a ENTRAMBI i punti
>   morte in `defense.c` (`die_light`/`gib`, `DANGER_R`/`DANGER_W`). Sangue sparso
>   → nudge soft (reroute fra strade aperte, il PBD shova comunque il fronte → la
>   massa inonda); killbox tenuto → satura a scala-muro → l'orda sfonda le
>   barricate del giocatore invece di alimentare l'imbuto (contro "tutte le
>   torrette in un punto + muri attorno"). Resta < WALL_ENTER per cella (palazzi
>   intatti); vince il muro più economico (barricate prima dei palazzi). Verifica:
>   `test_blood_fear.c` (reroute soft 90%→96%, scala-muro: pressione barricata
>   0.06→90.9, decay a mezza-vita, determinismo, no-NaN).
> - **Costo-nav-cadaveri §7-bis (`corpse_mass/pack/height`→Dijkstra): PENSIONATO**
>   (`k_corpse` default 0). La macchina resta nel core, dormiente (riattivabile con
>   `k_corpse`>0, es. `test_corpse_pile`); `simp_corpse_height` non è più letto dal render.
> - **Anti-imbuto/reroute**: stessa funzione strategica, nuovo veicolo (campo di
>   pericolo = sangue invece di altezza pila). Unifica anche lo "screamer che cerca il
>   path più sicuro" (§7-bis, in fondo): è lo *stesso* campo.
>
> Quanto sotto (§1-§9, banner 2026-06-23) descrive il sistema cadaveri→nav previgente,
> conservato come storia del design. Resta valida la **tabella armi §6** (produce/
> rimuove cadaveri) e tutto §10.1/§10.2-tier1/§10.7 (rendering del cadavere singolo).
>
> **STATO (2026-06-23):** il **perno nav §7-bis** è IMPLEMENTATO (accumulo
> `corpse_mass`/`corpse_pack` per cella + `corpse_height` derivata + termine
> d'arco `k_corpse·min(height/wall_h,1)` a scala-muro; API `simp_corpse_height`
> / `simp_corpse_clear`; 6 param in `SimPParams`, default on; `test_corpse_pile`
> PASS). **Massa finita §3 (a) IMPLEMENTATA** (2026-06-23, `corpse_weight`
> default 40): l'orda shova la pila e sfonda invece di restare sigillata —
> "rallentano ma non fermano". Lo scaling `pack→invm` (cedimento progressivo) è
> stato valutato e **SCARTATO** (2026-06-23, vedi §3): inerte in pratica (`corpse_pack`
> non si accumula — il PBD tiene i centri agente fuori dalle celle cadavere) e
> comunque ridondante, l'anti-imbuto è già dato dal costo nav §7-bis. RESTA solo
> la tabella armi §6 (lato gioco). Il decay tiene le pile non-permanenti.
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
senza casi speciali: emerge dalla geometria. Il decay resta la garanzia di
non-permanenza. (NOTA 2026-06-23: in pratica `pack` resta ≈0 nelle scene
realistiche — incrementa solo se il *centro* di un agente a terra occupa una
cella cadavere, ma il PBD separa i dischi → height/costo nav sono guidati dal
solo `mass`, che è la grandezza che conta. Il `pack` è quindi un termine latente;
il cedimento fisico `pack→invm` è stato scartato per questo, vedi §3.)

## 3. Rallentare senza fermare: massa finita — FATTO (2026-06-23)

> **STATO: opzione (a) IMPLEMENTATA, massa finita FISSA.** Param `corpse_weight`
> (unità walker, default **40**, `<=0` = sigillo infinito legacy). I cadaveri
> ghost passano da `invm = 0` a `invm = 1/(corpse_weight·r0²)` (come
> `simp_spawn_desc`): l'orda che spinge li SHOVA a valle e sfonda. Le posizioni
> spinte dal PBD sono riscritte nel pool a ogni step (step 3b di `simp_step`); le
> coppie cadavere-cadavere sono saltate (sennò una pila di dischi sovrapposti si
> farebbe esplodere). `test_corpses` aggiornato: B = sigillo infinito (drain 0,
> moved 0), C = §3 finito (drain 44 vs 164 aperto → rallenta molto ma LEAKA,
> moved 9). Suite 22/22 verde.
>
> **Lo SCALING `pack→invm` ("più pestati → più cedevoli") è SCARTATO (2026-06-23).**
> Prototipato (`corpse_yield`, `invm = cinvm·(1+yield·pack)` con tetto al floor
> walker) e misurato: **inerte**. `corpse_pack` non si accumula nelle scene reali —
> incrementa solo quando il *centro* di un agente a terra cade in una cella con
> `corpse_mass`, ma il PBD separa i dischi agente da quelli cadavere, quindi i
> centri non ci entrano quasi mai; e dove la pila viene travolta, i corpi vengono
> shovati *fuori* dalla cella-deposito della massa. Risultato: drain bit-identico
> con yield on/off su tutti i pesi 4→32 (verificato). Ed è comunque **ridondante**:
> l'anti-imbuto/anti-degenere ("percorso libero pieno di torrette") è già coperto
> dal **costo nav §7-bis** (`mass→height→Dijkstra→reroute`, l'orda preferisce
> premere una barricata che insistere nel varco intasato). Stessa logica del taglio
> della rampa §4: un secondo vettore sullo stesso muro non vale il tuning/leggibilità.
> Il rimedio fisico resta la **massa finita FISSA** (sopra): rallenta senza fermare.

Per il rimedio "rallentano ma non fermano" bisogna mollare la **massa infinita**:

- **(a) Massa grande ma finita** sui cadaveri (`invm` piccolo non-zero, FISSA —
  lo scaling per `corpse_pack` "più pestati → più cedevoli" è stato scartato, vedi
  banner). Il PBD già lavora a rapporti di massa (M3.5): l'orda che spinge da
  dietro lentamente *sfonda* il mucchio invece di fermarsi netta. **SCELTA e
  implementata** (vedi banner). Cambio rispetto a `test_corpses` (dove sigillavano
  del tutto): test aggiornato (sigillo legacy + rallentamento §3 affiancati).
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

## 10. Rappresentazione visiva (render) — singoli vs mucchi

> **STATO (2026-06-24):** DESIGN + **PROTOTIPO mound in `vat_horde`** (tier 1-2-3
> di §10.6 fatti: cupola lumpy + colore per-faccia + arti hero; silhouette
> verificata a 0°/90° d'azimuth). Tutto qui è **render-side**
> (`vat_layer`/`vat_horde`): il core non cambia. L'unico aggancio è
> `simp_corpse_height` (§5), già esposto. Questa sezione **emenda GFX_DESIGN §5.1
> punto 1** ("il gioco gira a camera bloccata sull'iso"): vedi §10.1.
>
> Nota: la **normal map** di §10.3 è stata resa, nel prototipo, come **variazione
> di colore per-faccia** (`mnd_facecol`: mix verso palette carne/stoffa/sangue +
> jitter di luminosità) — coerente con lo stile faceted-flat di `vat_horde`
> (ostacoli/gib/prop sono tutti flat-shaded), dove una texture tangent-space
> stonerebbe. La normal map vera resta un'opzione se si passa a uno shader
> texturizzato per i mound.

### 10.1 Regime di camera: azimuth ruotabile, elevazione fissa

**Decisione di design:** la camera di gioco può **ruotare l'azimuth** (orbita
orizzontale attorno al punto guardato), ma l'**elevazione è bloccata** (sempre lo
stesso angolo di picchiata, alla 3/4). Questo **rilassa** l'assunzione di
GFX_DESIGN §5.1.1 (camera fissa sull'iso), pensata per rendere "invisibile" il
decal piatto bakato.

Conseguenza diretta: **il decal piatto NON è più la rappresentazione primaria del
cadavere singolo.** Sotto azimuth ruotante un disegno piatto a terra si tradisce —
ma è importante capire *perché*, perché cambia la strategia:

- **Lo shading bakato SOPRAVVIVE alla rotazione.** Con luce chiave world-fixed (il
  "sole" RTS della chiave NW, direzione fissa nel mondo, non agganciata alla
  camera) il diffuse di una forma è **azimuth-independent** (dipende da
  normale·luce, non da dove guardi). E un decal inchiodato al terreno **ruota col
  mondo** → la sua ombra/AO bakata resta coerente col sole da qualunque azimuth.
  Il lavoro di "fingere il volume con luce bakata" continua a reggere.
- **Quello che tradisce è il RILIEVO mancante: parallasse di silhouette.** A
  elevazione fissa, un oggetto con altezza reale, ruotando l'azimuth, vede le sue
  parti rialzate *scivolare* rispetto al terreno e cambiare auto-occlusione. Il
  decal piatto (`h=0`) sta incollato, non scivola mai. È **questo** l'artefatto, e
  una normal map non lo risolve (la normal dà shading, non silhouette né
  parallasse — §10.3).

### 10.2 Tre tier (LOD per rappresentazione)

Sfruttiamo che gli zombie sono già lowpoly 3D via VAT: il 3D dei cadaveri è quasi
gratis e l'azimuth diventa un non-problema dove conta.

1. **Singolo, fresco / vicino → mesh 3D statica (posa di morte congelata).** È
   **già** `vat_layer_die`: alla morte una visuale riproduce la clip dying una
   volta e poi **tiene l'ultimo frame** per un TTL, indipendente dall'agente sim
   già rimosso. È 3D vero → regge a ogni azimuth, parallasse corretta, zero
   finzione. Copre il 90% di ciò che l'occhio guarda da vicino. **Estensione:** a
   TTL scaduto, invece di sparire, il decedente confluisce nel mucchio (§10.4) o,
   se isolato/lontano, stampa il decal di tier 3.
2. **Accumulo (killzone sotto fuoco torrette) → mound mesh che cresce** (§10.3). I
   mucchi sono **pochi** — solo dove si concentra il fuoco — quindi una manciata di
   mesh istanziate, costo trascurabile.
3. **Lontano / zoom basso → decal piatto bakato.** Al tier di zoom basso (il "campo
   di densità = minimappa" di GFX_DESIGN) la parallasse di un corpo disteso è
   **sotto la soglia percettiva**: lì il decal è perfetto e scala a decine di
   migliaia di corpi senza geometria. **Resta valido** come LOD lontano — è la
   roadmap GFX §5.1–5.2 (height-decal + impostor cross-fade), solo declassata da
   "primaria" a "LOD distante". La transizione mesh→decal è su distanza/zoom, non
   sul tempo.

### 10.3 Il mound: forma, crescita, dettaglio — ~~IMPLEMENTATO~~ RIMOSSO (2026-06-25)

> **RIMOSSO** da `vat_horde` (vedi banner in testa): i mound erano brutti e
> renderli passabili costava troppo. §10.3-10.6 conservati come storia del design.


**Forma — NON un tronco di piramide a base quadrata.** Una piramide tronca a 4
facce ha spigoli netti che, ruotando l'azimuth, **telegrafano la primitiva** (a 0°
vedi una faccia, a 45° lo spigolo in mezzo): è il giveaway peggiore proprio sotto
la meccanica che vogliamo. Un mucchio di cadaveri ha silhouette **irregolare e
tonda** da ogni lato. Tre accorgimenti, costo crescente:

- **Base a molte facce / revolved** (cono o cupola a 8–12 lati invece di 4): da
  qualsiasi azimuth la silhouette resta tonda, niente spigolo che tradisce. Costa
  nulla e risolve l'80%.
- **Lumpiness nei vertici della mesh** (non nella normale): la mesh è low-poly,
  basta spostare un po' i vertici per un profilo bitorzoluto. La silhouette
  irregolare è ciò che la normal map **non potrà mai** dare, e qui costa pochi
  vertici.
- **3–4 arti "hero"** (un braccio, una gamba, una testa) piantati ad angoli random
  sul rim a spezzare l'outline. Sono ciò che l'occhio guarda; vendono l'insieme con
  due triangoli.

**Dettaglio di superficie — normal map (o simile).** Sotto sole world-fixed
l'illuminazione è azimuth-independent (§10.1) → il dettaglio di normal (corpi,
arti, pieghe che spezzano la luce) resta **coerente** mentre ruoti, e fa leggere il
mound come carne invece che come primitiva liscia. **Tienila.** Ma ricorda: la
normal **non aggiunge silhouette né parallasse** — il break-up del profilo deve
venire dalla *geometria* (sopra), non da lei.

**Crescita — Y-position (emersione dal suolo), NON Y-scale.**

- *Y-scale* stira i texel verticalmente → man mano che cresce, i cadaveri nella
  texture/normale si **allungano**. Brutto e visibile.
- *Y-position (emersione)*: il resto del mucchio sta **sotto il terreno** e viene
  spinto in su. Forma e densità di texel **costanti**; il terreno opaco clippa la
  parte sepolta **gratis** col depth buffer. Bonus: un cono/frustum sepolto emerge
  **prima dalla punta stretta**, poi le sezioni più larghe → il mucchio cresce in
  altezza **e** in impronta insieme, esattamente come si accatastano i corpi.
- La quota visibile (quanto è emerso) è legata a **`corpse_height` §7-bis** — lo
  stesso campo che guida il costo nav → visivo e nav restano in **sync gratis** (la
  pila che fa deviare l'orda è anche quella che si vede crescere).

**Contact shadow / AO alla base**, o il mound galleggia: un decal d'ombra morbido
sotto il rim lo inchioda a terra (questo sì, un decal piatto, ma per l'ombra di
contatto va benissimo — è proprio piatto).

### 10.4 Transizione singoli → mucchio (anti-pop)

Quando un gruppo di cadaveri singoli (mesh in posa di morte, tier 1) diventa un
mound (tier 2)? **Soglia su `corpse_height` per cella:** sotto → si mostrano i corpi
individuali; sopra → emerge il mound. Il rischio è il **pop** allo stacco.
Mitigazione: il mound **emerge SOTTO i singoli ancora visibili e li riassorbe**
mentre sale — così "inghiotte" i corpi invece di sostituirli con un cambio netto.

### 10.5 Filosofia "abbastanza buono"

Il gioco è **frenetico**: il giocatore non ha tempo di ispezionare i dettagli.
Basta una **vaga idea** di mucchio di cadaveri, non dev'essere perfetto. Priorità,
in ordine: prima la **silhouette tonda + lumpy** che regge a occhio sotto rotazione
(è ciò che si nota), poi semmai il dettaglio fine. Budget bersaglio: **una sola
mesh-mucchio low-poly condivisa**, scalata/ruotata/emersa per istanza, + **una
normal map condivisa** + pochi arti hero. Niente di per-corpo, niente
displacement/parallax-occlusion a runtime.

### 10.6 Prototipo (prossimo passo)

Nel sandbox `vat_horde` (che ha già camera con rotate, comodo qui): far emergere la
mesh-mucchio da `corpse_height` su una cella e **giudicare a occhio se la silhouette
tonda+lumpy regge ruotando l'azimuth**. Iterazioni: (1) cono/cupola liscio →
verificare che NON sembri primitiva; (2) + lumpiness vertici; (3) + normal map; (4)
+ arti hero. Fermarsi al primo tier che "dà l'idea" sotto rotazione veloce.

**FATTO (2026-06-24).** Implementato in `vat/vat_horde.c`: `build_mound` (cupola
14×4 a lobi + jitter, face-normal flat, colore per-faccia via `mnd_facecol`) +
`mnd_limb` (arti/teste come box orientati che sbucano radialmente, 2 su pile basse,
4 sulle alte). Alimentato dal campo `simp_corpse_height` live (soglia 0.20 m, una
cupola per cella sopra soglia) **e** da una modalità test (`VAT_HORDE_MOUNDTEST=1`
= fila di mound di altezza crescente, scollegata dalla sim). Toggle runtime **M**.
Verificato headless (`VAT_HORDE_SHOT`/`VAT_HORDE_CAM`) a azimuth 0° e 90°: la
silhouette resta tonda/irregolare (niente spigolo da primitiva), gli arti ruotano
con la geometria (parallasse corretta), il colore per-faccia rende il patchwork di
corpi. **Tier 1+2+3 raggiunti** — "dà l'idea di un mucchio di cadaveri" sotto
camera rotante, sufficiente per il ritmo frenetico (§10.5).

**Aggregazione celle→mound unico — FATTO (2026-06-24).** `build_corpse_mounds`:
flood-fill 8-vicini sulle celle `corpse_height >= 0.20` → componenti connesse, UN
mound per componente. Footprint = **ellisse della covarianza** pesata per altezza
(semi-assi `ra/rb` = 2·√autovalori + mezza cella, orientazione = autovettore
principale via `atan2`): una pila allungata (banda lungo un muro/varco) dà un mound
**stirato lungo il suo asse**, non un campo di cupolette. Altezza = picco della
componente; arti scalati col numero di celle. `build_mound` generalizzato a
footprint ellittico (trasformata `XF`: scala anisotropa + rotazione). Seed
**quantizzato** su griglia coarse (~2.5 m): la lumpiness/arti restano stabili mentre
il centroide deriva (no flicker per-frame). Verificato headless con `VAT_HORDE_PILE=1`
(disco→mound tondo, banda→mound stirato; orientazione world-space coerente a 0°/90°
d'azimuth); live reale senza crash. **Limite noto:** le componenti NON sono tracciate
fra i frame → se il centroide attraversa un bucket da 2.5 m la lumpiness si rimescola
(pop raro). Fix proprio = tracking persistente delle componenti (follow-up se dà
fastidio).

**Contact-shadow / AO alla base — FATTO (2026-06-24).** Un'istanza di ombra
ELLITTICA per mound (`vat/mound_shadow.vs` + `shadow.fs` riusato): il disco
unitario delle ombre agente (`shDisc`) scalato sui semi-assi `ra/rb` del footprint
e ruotato sull'asse principale (stessi parametri della cupola) → l'ombra segue la
pila allungata invece di restare tonda, ed è world-aligned (coerente sotto
rotazione d'azimuth). Quota = `ter_z+0.03`, raggi ×1.12 per huggare il rim, blend
+ no depth write, disegnata PRIMA delle cupole opache. `build_corpse_mounds` emette
anche le istanze d'ombra (param `shad`/`nshad`); il path `VAT_HORDE_MOUNDTEST`
riempie ombre circolari. Inchioda i mound a terra ("o il mound galleggia", §10.3):
verificato headless (`VAT_HORDE_MOUNDTEST` fila di 4 + `VAT_HORDE_PILE` banda) —
alone morbido sotto ogni rim, ellisse allineata alla banda.

**RESTA (follow-up, non bloccante):** (b) emersione graduale per Y-position legata
alla crescita di `corpse_height` (oggi la cupola appare già a piena altezza sopra
soglia) + transizione anti-pop dai decedenti singoli (§10.4); (d) eventuale normal
map vera se si texturizzano i mound; (e) tracking persistente delle componenti
(anti-flicker completo, vedi sopra).

### 10.7 Sagoma-cadavere persistente a terra (decal del corpo) — FATTO (2026-06-24)

Prima il decedente (mesh in posa di morte, §10.2 tier 1) a fine TTL svaniva
lasciando **solo** una macchia di sangue (disco). Ora lascia **anche la sagoma del
corpo** disegnata a terra ("come la sagoma del corpo nelle indagini per omicidio"),
persistente come la macchia.

**Tecnica — sprite top-down BAKATO dal modello, instanced (NON RTT in texture).**
La sagoma è un quad orientato texturizzato con uno sprite top-down del corpo. Gli
sprite vengono **bakati a init** (`vat_horde`, blocco "BAKE atlante sagome-cadavere"):
per ogni variante si renderizza il modello VAT nell'ultimo frame di una clip
`dying`/`death` con una **camera ortografica dall'alto** (guarda −Y, up +Z mondo,
semi-lato `CORPSE_HALF`=1.15 m) in una cella RGBA di un atlante (`CORPSE_CELL`=256
px × NVAR celle in riga); `alpha=1` dove c'è il corpo, 0 fuori → **cutout**. Lo
shading NW è già bakato nello sprite → niente asset esterni, lo sprite *è* il modello
reale. (Il crawler non ha clip morte → fallback frame 0, ma non genera mai un
decedente: cella mai campionata.) Debug: `VAT_HORDE_CORPSE_ATLAS=1` dumpa l'atlante
in `corpse_atlas.bmp` (sagoma su magenta).

**Perché instanced e non il bake-in-texture proposto.** I decal di sangue sono già
quad instanced persistenti in un ring buffer cappato (8192) ridisegnati ogni frame,
costo trascurabile. Le sagome riusano lo stesso schema (pool `q*` in `vat_layer`,
`vat_layer_fill_corpse_decals`, cap 4096, ring buffer = "tappeto dell'orrore"
limitato): bounded, semplice, zero UV sul terreno. Il **bake del POOL in una texture
"cadaveri" world-aligned** (l'idea originale) è la mossa giusta solo per permanenza
*illimitata* oltre il cap instanced, e richiede UV-map sul terreno + FBO di
compositing; si potrà fare dopo **senza toccare il lato emit** (il punto di
emissione resta identico). Rinviato.

**Pipeline runtime.** Alla scadenza del decedente (`vat_layer_update`):
`corpse_decal_add(x, y, heading, scala, variante, tinta)` accanto a `decal_add`
(macchia). Render (`vat/corpse_decal.vs`/`.fs`): quad `[-1,1]²` ruotato per heading,
posato a `ter_z+0.05` (sopra la macchia a +0.03), semi-lato = `CORPSE_HALF·scala`
(il tank, più grande, lascia una sagoma più grande), UV → cella d'atlante della
variante; FS = sample + cutout (`alpha<0.04` discard) + scurimento ×0.85 ("posato/
secco"). Blend, no depth write, sopra il sangue e sotto l'orda. Verificato headless
(1500 frame, 94 kill): corpi distesi orientati con pozza di sangue sotto, scala
coerente con gli zombie vivi.

**RESTA (polish, non bloccante):** (i) la cella delle varianti senza diffuse esce
slavata (sprite biancastro) — minore, riguarda i body atipici; (ii) la posa bakata
è sempre l'ultimo frame di `dying`, mentre il decedente vivo può mostrare `death`
(scelta random per-corpo): micro-incoerenza all'istante della transizione, entrambe
pose distese; (iii) varietà di posa (più frame/cluster) e fade lento opzionale.

### 10.8 Ring impostor della sagoma — VALIDATO A OCCHIO dall'utente (2026-07-12): è la strada

> **Verdetto (2026-07-12, dopo giudizio visivo in fxlab):** differenza notevole
> rispetto al decal piatto — il ring impostor è la rappresentazione persistente
> del cadavere. **16 viste NON negoziabili** (non scendere a 8); la leva di
> risparmio, se serve, è la **risoluzione della cella** (128 → 96/64), non il
> numero di viste. Prossimo passo: migrazione in `vat_horde` (tier mesh →
> impostor su TTL/zoom).

**Contesto.** Con la camera di GIOCO vincolata (decisione 2026-07-08 in
`vat_horde`: elevazione FISSA `GAME_CAM_EL` 0.40, yaw libero, ortografica, zoom
clampato) l'insieme delle direzioni di vista reali è un **anello 1D di azimuth**
a elevazione costante. Un octahedral impostor pieno (dominio 2D di viste,
pensato per camere libere) sprecherebbe ~90% dell'atlante su elevazioni mai
viste: la forma giusta qui è il **ring impostor** — N viste azimutali bakate
all'elevazione di gioco. Sostituisce l'esperimento normal-map di §10.7 in
roadmap: la normal dà solo shading, il ring dà la **silhouette per azimuth**
(la parallasse mancante di §10.1, l'artefatto vero del decal piatto).

**Nota ortografica (corretta dall'utente):** in proiezione orto la distanza
dalla camera NON cambia i pixel — la transizione mesh→impostor→(eventuale decal
al tier minimo) va decisa su **tier di zoom e/o TTL**, mai su distanza.

**Implementazione fxlab (2026-07-12):**
- `ring_bake()` in `fxlab.c`: 16 viste × cella 128 px, griglia colonne =
  var×NPOSE+posa (come §10.7), righe = vista k (camera a azimuth k·2π/16,
  elevazione `RING_EL` 0.40, ortho ±`RING_HALF` 1.15 centrata su
  (0, `RING_LOOKY` 0.35, 0)). Due texture: ALBEDO (outfit corrente, riga
  insanguinata o+16, **ribakata al volo al cambio outfit** — 16×18 draw da
  ~322 tri, costo nullo) + NORMALE world a heading 0 per vista
  (outfit-indipendente, bakata una volta). Il lit bakato sarebbe sbagliato per
  cadaveri ruotati (il sole gli girerebbe attorno): relight per-pixel in
  `corpse_imp.fs` ruotando la normale per heading, sole NW world-fixed, come
  il modo NORMAL di §10.7.
- Selezione vista: `world = Ry(heading)·model` ⇒ azimuth camera nel model
  frame = `az_cam − heading`; riga = quella vista (snap) o crossfade delle 2
  adiacenti (`corpse_imp.vs`). In ortho la vista è globale per frame; gli
  heading random per-corpo desincronizzano gli scatti di vista.
- Quad billboard nel piano di vista (right/up2 della camera), ancorato a
  terra + `RING_LOOKY`, spinto verso la camera di 0.6 m (in orto non sposta i
  pixel, solo la profondità: niente z-fight col terreno). Blend, no
  depth-write: i corpi (30–40 cm) non occludono mai i vivi — errore accettato v1.
- Toggle `I` in fxlab: DECAL PIATTO / RING snap / RING blend; `0` = elevazione
  di gioco; env `FXLAB_IMP=0/1/2`, `FXLAB_DIE=N` (morte normale headless),
  `FXLAB_RING_ATLAS=1` (dump anello).
- **Verificato headless**: silhouette/orientamento/posa dell'impostor
  combaciano con la mesh decedente a due azimuth (Δ90°), rotazione coerente
  col mondo; parità col decal piatto sulla cella verde della variante child
  (asset issue nota §10.7-i, non del ring).

**Memoria** (outfit singolo): 18 col × 16 viste × 128² RGBA ≈ 19 MB × 2
texture. Ogni outfit in più moltiplica solo l'ALBEDO. Decisione outfit
(2026-07-12): i CADAVERI convergono a **2-3 outfit neutri desaturati**
(fango+sangue; transizione di colore giocata nella fase MESH del decedente,
dai colori vivi al neutro) → il moltiplicatore outfit resta piccolo per
costruzione, e la continuità visiva allo swap mesh→impostor è garantita.
Manopole qualità: cella (128→192/256 se il close-up lo chiede — lo zoom di
gioco verrà ulteriormente ristretto), viste (16→8 col blend), celle
rettangolari (~2.7× di risparmio, il corpo disteso è largo e basso), BC1/BC3.

**Anti-tappeto (deciso 2026-07-12):** per rompere il "tappeto di cadaveri
tutti nella stessa posa" l'utente authorerà 2 brevi **settle animations**
(partono dall'ultimo frame di dying/death, arrivano a una posa leggermente
diversa in pochi frame), giocate nella fase MESH del decedente prima dello
swap a impostor → pose finali ×2-3 = colonne d'atlante in più, costo lineare
piccolo. Insieme a heading random + colori neutri, il timbro sparisce.

**Migrazione in `vat_horde` — FATTA (2026-07-12).** Cella 64 px (scelta
utente; 16 viste intoccabili), anello COMPLETO a 16 blocchi outfit
(continuità outfit per-cadavere garantita; elementali 14/15 con le loro
righe come prima): albedo 1152×16384 px = il MASSIMO GL tipico in altezza —
`nOutRing` viene clampato a runtime su `GL_MAX_TEXTURE_SIZE` (lo shader
satura l'outfit all'ultimo blocco, degrado grazioso). ~75 MB albedo, come
l'atlante piatto 256 px che SOSTITUISCE (net ≈ +5 MB di normale per-vista).
Il decal piatto top-down è rimosso da vat_horde (stesso costo per quad
dell'impostor ⇒ nessun motivo di tenerlo come tier); resta in fxlab per
confronto (toggle I). Tier: mesh decedente (TTL, invariato) → ring impostor
persistente. Verificato headless su `arena_breach` (139 kill): tappeto
coerente a due azimuth, zero errori FBO. Shader condiviso con fxlab
(`uNOut` = blocchi outfit: 1 in fxlab col ribake, 16 in gioco).

**Aperto:** (a) pendii — l'elevazione effettiva cambia col terreno inclinato;
se stona, 2-3 anelli di elevazione attorno a 0.40 (= ottaedrico ridotto);
(b) in gioco il bake per-outfit può andare offline (estensione della pipeline
sprite_render/outfit_bake) e compresso, se l'init si allunga;
(c) **OTTIMIZZAZIONE DA ESPLORARE (decisione utente 2026-07-12):** i 16
blocchi outfit dell'anello sono una perplessità dichiarata — ridurli a **4
outfit "generici" da cadavere** (desaturati, fango+sangue), con una
**transizione outfit-specifico → outfit-generico giocata nella fase MESH del
decedente** (è lì che l'occhio guarda; allo swap in impostor il corpo è già
convergito al generico). Taglia l'albedo dell'anello a ¼ (~19 MB), leva il
clamp `GL_MAX_TEXTURE_SIZE` dal percorso critico e prepara i design-cadavere
dedicati della pipeline outfit. Richiede: 4 righe outfit "corpse" nelle
diffuse (o un grading a runtime nella fase mesh) + mappa outfit→generico.

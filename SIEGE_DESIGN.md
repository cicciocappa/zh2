# Siege — distruzione di muri e barriere: design tecnico

> **SENSORE IMPLEMENTATO** (`simp_wall_pressure` / `simp_wall_cell`, `test_siege`
> PASS). Il lato gioco (HP delle strutture, timer d'attacco, crollo) è dimostrato
> dentro `test_siege.c` ma vive nel codice di gioco, non nel core. Design della
> meccanica d'assedio: gli zombie che premono contro un muro/barriera del
> giocatore lo *attaccano* (attacchi discreti), ne erodono gli HP e alla fine lo
> fanno crollare; il varco aperto re-instrada l'orda da sola. Risolve due
> questioni aperte di `TODO.md`: «drain → attacca la struttura» e il punto M6
> «stuck contro muro dovrebbe attaccare».

Principio guida (come M3): **il core resta gameplay-agnostico.** HP, strutture,
denaro, timer d'attacco e politica di danno vivono nel codice di gioco. Il core
espone solo il **sensore**: chi sta premendo contro un muro, contro quale cella,
e con quanta forza *intenzionale* (steering verso il muro, non spinto a forza
dalla folla). Tutto piatto, mappabile su compute shader.

Meccanica sorella — gli **hazard ambientali** (albero/pilone che il fiume di
zombie fa cadere di lato) — è lo stesso primitivo con un segnale diverso
(reazione *laterale* accumulata invece di pressione *frontale*): design separato,
fuori scope qui. Vedi §7.

---

## 1. Il segnale: contatto di muro per-agente

Tutto il dato necessario è già calcolato e **buttato** ogni step dentro
`wall_projection` (`sim_particles.c`). Quando un agente compenetra un muro:

```c
float d = simp_sample_sdf(s, px, py);   /* distanza al muro (<0 dentro)     */
if (d < r) {
    sdf_grad(s, px, py, &gx, &gy);      /* (gx,gy) = NORMALE (via dal muro)  */
    float push = r - d;                 /* PENETRAZIONE corretta questo step */
    px += gx * push;  py += gy * push;  /* ...e scartata                     */
}
```

Da qui ricaviamo i tre dati dell'assedio a costo quasi nullo (siamo già nel loop
caldo, leggiamo già `sdf`/grad):

- **Penetrazione** `push = r - d` → l'intensità del contatto (m).
- **Normale** `(gx, gy)` → da che lato preme; la cella muro bersaglio è quella
  verso cui punta `-grad`: `cell = nav_cell(px - gx*d, py - gy*d)`.
- **Intenzione** `into_wall = -(flow·normal)`, con `flow` = direzione desiderata
  (`flow_x/flow_y`, già campionabile): `into_wall > 0` ⇔ l'agente *steera dentro*
  il muro (vuole il goal oltre il muro = assedio), `≤ 0` ⇔ tangente o in
  allontanamento (sfioramento, NON assedio — è la meccanica hazard di §7).

**Pressione d'assedio** = `push · max(into_wall, 0)`. Filtrando per `into_wall>0`
escludiamo automaticamente gli zombie schiacciati di traverso contro un muro che
non vogliono attraversare: solo l'assedio *voluto* conta. I flyer sopra `wall_h`
sorvolano e non generano contatto (corretto: niente attacco dall'alto).

## 2. API del core

Due array piatti paralleli, riempiti in `wall_projection`, indicizzati per indice
denso come gli altri SoA. Zero malloc nello step; due float/agente.

```c
/* Contatto di muro risolto QUESTO step, per agente a terra. Indicizzati per
 * indice denso: come ogni dato per-agente vanno convertiti subito in slot/handle
 * (l'indice denso cambia a ogni kill — vedi M3.1/M3.4). Resettati a ogni step. */
const float *simp_wall_pressure(const SimP *s);  /* push * max(into_wall,0); 0 = nessun assedio */
const int   *simp_wall_cell(const SimP *s);      /* indice cella nav assediata, -1 se nessuna  */
```

- `wall_pressure[i] == 0` (e `wall_cell[i] == -1`) per chi non è in contatto, o è
  in contatto ma *non* sta steerando dentro il muro (sfioramento).
- **Misurati sulla penetrazione PRE-correzione** dell'ultima iterazione di
  `wall_projection`: dopo la proiezione `d ≥ r` (l'overlap è annullato), quindi
  `push` va letto *prima* di applicare la spinta. È l'unico punto in cui `push` è
  ancora significativo. Coerente con M3.4 (sensori a fine step).
- **Cella assediata** = la cella SOLIDA nell'intorno dell'agente meglio allineata
  con `-grad` (massimo `dot(centrocella − pos, −grad)`). Una scansione di una
  finestrella (il muro è entro `r` dal centro ⇒ entro ~1 cella) garantisce che la
  cella sia *esattamente* solida, dove un singolo passo cieco lungo `-grad` può
  mancare il bersaglio agli angoli o sui muri di una cella.
- `simp_kill` (swap-and-pop) sposta anche `wall_pressure`/`wall_cell` come gli
  altri SoA, così restano coerenti con gli indici densi dopo il drain.
- Niente nuovo stato persistente nel core: sono un fotogramma del passo corrente.
  La *memoria* dell'assedio (timer, HP) sta nel gioco.

## 3. Modello d'attacco discreto (lato gioco)

Scelta di design: **attacchi discreti**, non drain continuo di HP. Il campo di
pressione è il *sensore* (chi/dove); il gioco fa girare un timer d'attacco
**per-slot** e applica colpi a cadenza fissa. Più leggibile (vedi gli zombie
picchiare il muro), si aggancia al timing dell'animazione, e fa contare i tipi
(un «brute» d'assedio colpisce più forte o più spesso).

Per tipo di nemico (tabella di gioco, indicizzata per slot come HP/bounty in M3.5):

```c
float attack_period;   /* s tra un colpo e il successivo (es. 0.8)        */
float attack_damage;   /* HP tolti per colpo (es. 5)                      */
```

Per slot (stato di gioco): `attack_timer[slot]`. Ogni step di gioco:

```c
for (int i = 0; i < n; i++) {
    int slot = simp_slot_of(s, i);
    /* ATTACK_MIN_P: soglia di contatto. Uno sfioramento tangente lascia filtrare
     * una pressione quasi nulla (into_wall ~ 0); senza un floor anche quella
     * scheggerebbe il muro col tempo. Gate fuori. (Misurato: ~0.004 separa la
     * frangia tangente dagli assedianti frontali; vedi test_siege §6.) */
    if (wall_pressure[i] >= ATTACK_MIN_P && wall_cell[i] >= 0) {
        int struct_id = cell_to_structure[wall_cell[i]];   /* mappa di gioco */
        attack_timer[slot] += dt;
        if (attack_timer[slot] >= type[slot].attack_period) {
            attack_timer[slot] -= type[slot].attack_period;
            structure_hp[struct_id] -= type[slot].attack_damage;   /* un colpo */
        }
    } else {
        attack_timer[slot] = 0.0f;   /* perso il contatto: niente colpo "gratis" */
    }
}
```

> **Nota sulla scala di `wall_pressure`.** `push` (penetrazione corretta) è sempre
> piccolo (il PBD lo riassorbe ogni iterazione): la pressione vive in ~0.003–0.07,
> e il vero discriminante frontale↔tangente è il fattore `into_wall` ∈ [0,1]. Il
> floor va quindi tarato in *unità di pressione*, non a occhio. Se servisse un
> segnale più netto, esporre `into_wall` separato è un'opzione (tenuto per ora
> nel prodotto `push·into_wall` come da progetto).

- `cell_to_structure[]` è del gioco: il giocatore piazza muri/barriere e sa quali
  celle nav formano quale struttura (il modello 3D ha la sua *base* su quelle
  celle, §4). Il core non conosce le «strutture», solo celle solide.
- DPS effettivo emergente = `(#assedianti su quella struttura) · damage / period`.
  Più zombie premono → crollo più rapido, senza accumulatori dedicati.
- Reset del timer alla perdita di contatto evita il colpo gratis di chi si è
  appena affacciato; chi assedia stabilmente ha un ritmo regolare (buono anche
  per l'animazione).

## 4. Crollo e reroute

Quando `structure_hp[struct_id] <= 0` il gioco:

1. **Apre il varco**: `simp_set_wall(s, cx, cy, false)` su tutte le celle della
   struttura. Una qualsiasi modifica del terreno **forza il commit completo della
   nav** (phi + flow + SDF) al prossimo `simp_step` — l'orda si re-instrada da
   sola attraverso la breccia, che diventa subito un funnel. Gratis dal motore.
2. **Macerie (opzionale, consigliato)**: piazza qualche cadavere
   (`simp_corpse_add`) sull'impronta della struttura → barricata *parziale*
   emergente (come il pennello KILL), così il varco non è pulito ma ingombro; il
   TTL dei cadaveri lo libera col tempo. La deviazione del flusso attorno alle
   macerie emerge dal solo PBD (i cadaveri non sono nella nav, M3.3).
3. **Scoppio (opzionale)**: `simp_apply_impulse` verso l'esterno dal centro della
   breccia → sbalza la prima linea di assedianti. Feedback leggibile del crollo.

Nessuna di queste tocca il core: sono tre chiamate API esistenti.

## 5. Animazione (sprite layer)

`sprite_layer.c` ha già lo stato STUCK (EMA velocità con isteresi). Estensione:

- Un agente con `wall_pressure[i] > 0` → stato **ATTACKING** → sheet d'attacco
  (riusa il meccanismo multi-sheet del walk/idle). Priorità su STUCK.
- **Heading inchiodato verso il muro** mentre attacca: usa `-grad` (= direzione
  verso il muro), non l'EMA della velocità (che a contatto è ~0 e rumorosa).
  Risolve direttamente il piroettare-negli-ingorghi per gli assedianti.
- La cadenza dell'animazione d'attacco dovrebbe combaciare con `attack_period`
  del tipo (un colpo = un ciclo), così il danno «cade» sul frame dell'impatto.
- Il layer può leggere `simp_wall_pressure` direttamente (come legge già le
  posizioni), oppure il gioco glielo passa via un `sprite_layer_set_attacking`
  analogo a `sprite_layer_set_stuck`. Preferibile la seconda: tiene il layer
  ignaro del gameplay (§7 di GFX_DESIGN, «il core non sa nulla»).

## 6. Verifica — `test_siege.c` (headless, PASS)

Due metà.

**A) Selettività (micro-test deterministico).** Un muro orizzontale con un blocco
di agenti subito a sud. In un caso il goal è sigillato a NORD oltre il muro (flow
*dentro* il muro ⇒ assedio); nell'altro un goal alto a est tira il flow di lato
(flow *tangente* ⇒ sfioramento). Stesso muro, stesso blocco, solo la direzione del
goal cambia. Misura il picco di `wall_pressure` (saltando il transitorio di spawn).
Risultato: **into-wall 0.033 vs tangente 0.0037** (sotto il floor d'attacco). Si
isola così il filtro `into_wall` dalla geometria della folla — una folla confinata
fabbrica sempre del contatto frontale da qualche parte (estremità, angoli,
convergenza al goal), quindi una scena «di solo sfioramento» emergente è inaffidabile
come controllo.

**B) La meccanica (folla).** Muro che sigilla il goal a cx=80; l'orda (~1365)
preme una banda distruttibile (cy 50..70). Gira il modello ad attacchi discreti
del §3 (timer per-slot, floor `ATTACK_MIN_P`). Assert verificati:

1. **Attribuzione cella**: ogni `wall_cell[i]` con pressione > 0 è solido (0
   violazioni in tutte le scene).
2. **Attacchi → crollo**: la banda accumula 400 colpi e crolla (step ~2721).
3. **Crollo → reroute → drain**: liberate le celle e ricommessa la nav, l'orda
   drena attraverso la breccia (>50% degli spawnati, da 0 prima).
4. **Muro indistruttibile (controllo)**: stessa scena con la banda non
   distruttibile → 0 drain, assedio eterno (comportamento M3 base, invariato).
5. **Determinismo** (due run identiche: stesso step di crollo, stessi colpi,
   stesso drain) e **zero NaN**.

## 7. Note, trappole, relazione con gli hazard

- **Indici densi instabili**: `wall_pressure`/`wall_cell` sono per indice denso e
  valgono per un solo step — convertire subito in slot per il timer (M3.1/M3.4).
- **Sfioramento ≠ assedio**: lo zombie schiacciato di traverso contro un muro che
  *non* vuole attraversare ha `into_wall ≤ 0` → niente attacco. Quel contatto
  laterale è invece il segnale della meccanica **hazard ambientale** (albero/palo
  che il fiume fa cadere): lì il carico è la **reazione laterale accumulata**
  (somma vettoriale, con EMA + decadimento) e la soglia fa *cadere* l'ostacolo
  *a valle* lungo la direzione del flusso — design separato, stesso primitivo
  (carico → soglia → edit del terreno → reroute).
- **Toolkit del giocatore già presente**: dirigere il fiume verso un muro debole o
  un hazard si fa con `simp_add_cost` — `w<0` (richiamo screamer) = esca, `w>0`
  (paura/fango) = repellente — più le barriere fisiche. Già nel core (M3.5).
- **Cella nav vs struttura**: una struttura = più celle; il raggruppamento e gli
  HP sono del gioco. Celle condivise tra strutture adiacenti: il gioco decide a
  chi attribuire (es. `cell_to_structure` univoco per cella).
- **Costo**: due float per agente scritti nello stesso loco di `wall_projection`,
  più un `simp_sample_flow` + un dot per agente *in contatto* (pochi). Trascurabile.
  Mappa su GPU come due array piatti scritti nel kernel di wall projection.
- **L'assedio sostenuto richiede una folla** (scoperto in verifica). Un singolo
  agente piazzato dentro un muro NON resta a premere: la `wall_projection` lo
  espelle e il recupero di velocità (`v=(x−x_prev)/dt`) lo trasforma in una
  velocità d'uscita (anche ~9 m/s) che lo allontana; il flow non lo riporta in
  tempo. La pressione frontale sostenuta nasce solo dalla folla che spinge da
  dietro (o dal flow che pompa l'intera massa contro il muro). Corretto: lo
  sfioramento *non* deve danneggiare, e senza spinta normale non c'è assedio.
- **Caveat «cosa conta come dentro il muro»**: `recompute_flow` sceglie il vicino
  a phi minimo *ignorando* il costo d'arco, quindi con un goal che copre l'intera
  sezione (es. una colonna-goal a tutta altezza) i vicini E/NE/SE vanno in
  pareggio e l'ordine di scansione vince → un flow con bias diagonale (SE) che
  punta *genuinamente* dentro un muro a valle. Non è un bug del sensore (riporta
  fedelmente flow·normale): è il flow field. Tenerne conto disegnando i livelli /
  ragionando su dove l'orda «preme». I goal piccoli/mirati lo evitano.

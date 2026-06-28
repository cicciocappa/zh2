# Draggable — oggetti trascinati dalla folla & barricate sfondabili: design tecnico

> **STATO: IMPLEMENTATO** (`simp_drag_*`, `test_drag` PASS). Terza categoria di
> corpo PBD. Il core resta gameplay-agnostico: HP, denaro, trasformazione
> struttura→detrito vivono nel gioco; il core espone solo la **fisica** del
> disco trascinabile a massa finita con momento + attrito.

Principio guida (come M3 / SIEGE): **il core non sa cosa sia un cassonetto.** Sa
solo simulare un disco passivo che (a) ha massa finita e quindi viene **spinto**
dalla folla, (b) conserva il **momento** e scivola, (c) è frenato da un
**attrito** che lo ferma quando la spinta cessa, (d) collide con muri e con gli
altri oggetti. Il gioco ci costruisce sopra cassonetti, auto, e — accostandone
parecchi — **barricate** che l'orda sfonda a forza.

---

## 1. Dove si incastra: il parente è il cadavere, non l'agente

L'architettura ha già due categorie di corpo nella griglia di collisione PBD:

1. **Agenti** (`[0, count)`): steering verso il flow, drenati ai goal, handle
   stabili, query, volo. Tutto il macchinario di gioco.
2. **Cadaveri-ghost** (`[count, count+corpse_count)`): dischi passivi appesi in
   coda alla SoA prima del binning. Il kernel PBD li tratta come dischi normali
   (nessun caso speciale nel loop caldo); invisibili a steering, drain, query,
   impulsi, nav grid. Massa finita §3 (`corpse_weight`) → la folla li shova.

Il **draggable** è una TERZA categoria, ghost come il cadavere ma con due
differenze sostanziali:

- **Ha velocità propria + momento.** Il cadavere è statico: si muove solo MENTRE
  la folla lo preme (le posizioni spinte dal PBD sono riscritte nel pool, ma non
  conserva inerzia tra uno step e l'altro). Il draggable INTEGRA la sua velocità
  ogni step (`pos += vel·dt`), recupera la velocità dallo spostamento PBD (come
  un agente, `v=(x−q)/dt`) e la porta avanti → un cassonetto spinto **continua a
  scivolare** anche se la folla davanti si dirada, e si ferma per attrito.
- **Collide con i muri.** Il cadavere è piazzato dal gioco in posizione valida e
  non si muove di sua iniziativa, quindi la collisione muro non serve. Il
  draggable si MUOVE → un'auto spinta contro un palazzo deve fermarsi al muro.
  Wall projection SDF dedicata sui ghost draggable (serie, sono pochi: detriti =
  decine, non migliaia).

Layout ghost nella SoA, per step (`rebuild_grid`):

```
[0 .. count)                          agenti       (invm = 1/(m·r0²))
[count .. count+corpse_count)         cadaveri     (invm = corpse_weight finito o 0)
[count+corpse_count .. +drag_count)   draggable    (invm = 1/(mass·r0²), per-oggetto)
```

`grid_drag0 = count + corpse_count` = primo indice ghost draggable.

---

## 2. Il loop dello step

Aggiunte minime a `simp_step`, tutte gated su `drag_count > 0` (zero overhead se
non ci sono draggable; con 0 draggable il comportamento è bit-identico a prima):

1. **Integrazione** (prima di `rebuild_grid`): per ogni draggable salva
   `dq = pos`, poi `pos += vel·dt`. Niente gravità (piano 2D).
2. **rebuild_grid**: copia il pool draggable nei ghost slot `grid_drag0+k`, con
   `invm = dinvm[k]` per-oggetto; registra `grid_drag0`.
3. **PBD** (loop `pbd_iters`): il kernel processa già tutti i ghost. UNICO
   ritocco al loop caldo: lo skip "coppia ghost-ghost" diventa skip **solo
   cadavere-cadavere** (un mucchio di cadaveri sovrapposti non deve esplodere),
   mentre draggable-draggable e draggable-cadavere COLLIDONO — è la collisione
   reciproca che fa da "muro" quando li accosti (§4).
4. **Wall projection draggable** (dopo le iterazioni PBD, serie): per ogni ghost
   draggable, se `sdf < r` spingi fuori lungo il gradiente; clamp ai bordi mondo.
5. **Recupero + attrito + writeback** (3b', prima del drain — i ghost vivono a
   indici che `count` cambierebbe): `vel = clamp((pos − dq)/dt, v_clamp)`, poi
   `vel *= drag_factor` (attrito), scrivi `dpx/dpy/dvx/dvy`.

`drag_factor = clamp(1 − drag_damp·dt, 0, 1)`, come il `damping` degli agenti ma
forte: il default `drag_damp` è alto perché un oggetto pesante su asfalto perde
in fretta la velocità residua quando la folla smette di spingere. Manopola.

**Impulsi (esplosioni):** `simp_apply_impulse[_ex]` shova anche i draggable —
un kick radiale al pool (`dvx/dvy`) con falloff lineare, **scalato per 1/massa**
(detriti leggeri volano, il cassonetto pesante si sposta poco) → una granata
scompagina la barricata. Planare (i draggable non hanno asse z: niente lancio
verticale, `up_ratio` ignorato per loro). Il kick atterra su `dvx/dvy` e
l'integrazione (§2.1) lo porta come momento, esattamente come per gli agenti.
Brute-force sul pool (è piccolo), indipendente dalla griglia.

**Limite v1 annotato:** la wall projection draggable gira una volta a fine PBD
(non dentro ogni iterazione). Per oggetti pesanti e lenti è invisibile; un
agente schiacciato tra draggable e muro si risolve allo step dopo.

---

## 3. Nav: i draggable NON sono nel Dijkstra (è voluto)

Come i cadaveri, i draggable non timbrano la nav grid. Conseguenza: il flow field
NON li vede, l'orda ci flue contro come se non ci fossero e li **preme** (PBD)
invece di aggirarli. Per una barricata **sfondabile** è esattamente ciò che
serve: vuoi che la folla spinga e passi, non che reroutea attorno. Se in futuro
servisse un oggetto-ostacolo che devia la nav (un masso che resta lì), si
modella come struttura (`simp_set_wall` + `wall_cost`), non come draggable.

L'approccio **Hybrid** della nota di design (struttura intatta → detriti al
crollo) resta valido e si compone GIÀ da pezzi esistenti, lato gioco: barricata
intatta = `def_add_structure` (nav solid + `wall_cost` + sensore d'assedio + HP);
al crollo il gioco chiama `simp_set_wall(false)` sui suoi segmenti e `simp_drag_add`
per spargere i detriti. Ma il caso che chiedeva l'utente — «barricate costituite
da più oggetti accostati» — è più diretto e **non richiede la fase struttura**:
la barricata È la fila di draggable accostati (§4).

---

## 4. La barricata = fila di draggable accostati

Una barricata è semplicemente N draggable disco messi a contatto a sbarrare un
corridoio. Niente di nuovo nel core oltre al pool:

- **Tiene** perché i dischi collidono tra loro (§2.3): la fila trasmette la
  spinta lateralmente e resiste come un corpo unico finché la pressione è bassa.
- **Cede** perché dischi rotondi in un canale non tassellano: sotto pressione la
  folla (dischi piccoli) trova i varchi tra i dischi grandi e tra disco e muro,
  e la massa li **shova** a valle (rapporto di massa: tanti walker da massa 1
  contro pochi oggetti da massa M). Più pesanti i pezzi → più a lungo tiene; più
  numerosa la folla → prima sfonda. Emergente, niente soglia hard.
- **Si trascina**: i pezzi sfondati scivolano via col flusso (momento) e si
  fermano per attrito ai lati → il varco resta aperto, reroute naturale.

La differenza con un muro vero (`simp_set_wall`): il muro è in nav (l'orda lo
aggira o lo assedia a HP) ed è indistruttibile finché il gioco non lo toglie; la
barricata-draggable non è in nav (l'orda ci preme dritta) e cede per pura fisica,
senza HP. Due strumenti per due sensazioni: muro = struttura difensiva del
giocatore; barricata = oggetti d'ambiente che la folla travolge.

---

## 5. API (`sim_particles.h`)

```c
/* Aggiunge un disco draggable a (x,y), raggio, massa in unità walker (1.0 =
 * agente default; un cassonetto pesante ~10-40). Ritorna l'indice pool (NON
 * stabile, niente handle: il gioco indicizza i suoi dati per ordine d'inserimento
 * o ne tiene una mappa propria). -1 se pool pieno. */
int  simp_drag_add(SimP *s, float x, float y, float radius, float mass);
void simp_drag_remove(SimP *s, int i);    /* swap-and-pop */
void simp_drag_clear(SimP *s);            /* svuota il pool */
int  simp_drag_count(const SimP *s);
const float *simp_drag_px(const SimP *s); /* + py/vx/vy/rad: instance buffer / query gioco */
...
```

Manopola attrito globale: `SimPParams.drag_damp` (s⁻¹).

---

## 6. Verifica (`test_drag.c`)

1. **Momento + attrito**: un draggable colpito da un impulso di folla scivola e
   si ferma (velocità → ~0) in un tempo coerente con `drag_damp`.
2. **Mass-ratio**: stesso shove, oggetto pesante si sposta molto meno del leggero.
3. **Barricata sfondata**: corridoio sbarrato da una fila di draggable; l'orda
   preme; il drain passa da ~0 (muro solido di controllo) a >X% (la folla shova i
   pezzi e passa); pezzi leggeri sfondano prima dei pesanti.
4. **Collisione muro**: un draggable spinto contro un muro non lo penetra (sdf≥−ε).
5. **Determinismo** (due run identiche bit-a-bit) **+ no-NaN/out-of-bounds**.
6. **Esplosione**: un blast scaglia i draggable per 1/massa (leggero vola lontano,
   pesante quasi fermo) e poi si fermano per attrito.

---

## 7. Mappatura compute (vincolo architetturale)

I ghost draggable sono dischi normali nel counting-sort + PBD a tile colorati: la
mappa a compute shader non cambia. Integrazione/recupero/attrito sono per-oggetto
(pochi) e per ora seriali; banalmente parallelizzabili se mai i draggable
diventassero migliaia. Nessun momento angolare esplicito: un draggable singolo è
un disco. L'**auto** (§8) è due dischi tenuti da un vincolo di distanza rigido —
la rotazione emerge senza stato angolare, restando dentro lo stesso PBD.

---

## 8. Auto = due dischi + giunto rigido (STATO: IMPLEMENTATO, 2026-06-28; `test_car` PASS)

L'oggetto lungo (auto, furgone, bara, tronco) non si modella bene con un disco:
un cerchio non blocca un corridoio come fa una carcassa di traverso. La soluzione
leggera concordata è **due draggable collegati da un vincolo di distanza rigido**
(un "rod"). Niente corpo rigido vero, niente momento angolare, niente rotazione
esplicita: la rotazione **emerge** perché i due dischi possono avere velocità
diverse (la folla che preme una sola estremità fa perno sull'altra).

### 8.1. Verifica prestazioni (FATTA — via libera)

Prima di progettare ho misurato il costo marginale dei dischi draggable in scena
affollata (~10k agenti, multi-thread, scena tipo `test_particles`):

```
baseline (0 draggable)   3.4 ms/step
+60 dischi  (~30 auto)    dentro il rumore (~0.3–0.6 ms)
+200 dischi (~100 auto)   ~0.4 ms
+1000 dischi (~500 auto)  ~0.5–1.0 ms
```

Il disco è la parte cara (collisione PBD nella griglia); il giunto è O(n_auto)
per iterazione, **più economico** della collisione disco-disco → i numeri sopra
sono un tetto superiore al costo per-auto. A conteggio realistico (decine di auto
per mappa) il costo è **gratis**, perso nella varianza di scheduling. Motivo
strutturale: i draggable sono già ghost che il kernel PBD processa senza casi
speciali; le auto raddoppiano i dischi (2 per auto) e aggiungono un constraint
seriale su un pool piccolo — niente tocca il loop caldo sui 13k–100k agenti.
**Conclusione: le prestazioni non sono un ostacolo.**

### 8.2. Pool di giunti

Un terzo pool nel core, accanto a quello dei draggable:

```c
typedef struct { int a, b; float rest; } DragLink;  /* indici draggable + lunghezza a riposo */
```

`a`/`b` sono **indici nel pool draggable** (gli stessi non-stabili di §5).
`rest` = distanza voluta tra i due centri, fissata alla creazione (tipicamente
`r_a + r_b` per dischi a contatto, o più per un'auto allungata). Pool fisso come
gli altri (`LINK_CAP`).

### 8.3. Risoluzione: vincolo di distanza rigido nel writeback draggable

Il giunto si risolve in fase **3b'** di `simp_step` (la sezione draggable già
esistente: dopo le iterazioni PBD agente, prima del recupero velocità). Per ogni
giunto, proiezione PBD di distanza pesata per massa inversa, sulle posizioni
**ghost** `s->px/py[grid_drag0 + a|b]` (quelle appena spinte dalla folla):

```
d   = |p_b − p_a|                      (con epsilon-guard se d≈0)
n   = (p_b − p_a) / d
C   = d − rest                          (errore, +=troppo lontani, −=troppo vicini)
wa  = dinvm[a], wb = dinvm[b]           (massa inversa per-oggetto, già nel pool)
p_a += n · C · wa/(wa+wb)
p_b −= n · C · wb/(wa+wb)
```

Iterato `link_iters` volte (default ~4): le auto sono pesanti (massa 20–40), la
folla le deforma poco, quindi poche iterazioni bastano a tenere il rod **rigido**
sotto pressione. Si risolve DOPO la collisione PBD e DOPO la wall projection
draggable, sulle stesse posizioni ghost, così il recupero velocità (`v=(p−dq)/dt`
già in 3b') raccoglie sia lo shove della folla sia la correzione del rod →
momento e rotazione emergono dal solito recupero, zero codice nuovo nella fisica
di velocità/attrito. (Ordine: PBD agente → wall draggable → **giunti** → recupero
+ attrito + writeback nel pool.)

**Limite v1 annotato** (coerente con la wall projection §2): giunti e collisione
non sono co-iterati nello stesso loop. Un'auto schiacciata tra folla densa e muro
può vedere il rod allungarsi di qualche cm per uno step prima di richiudersi.
Invisibile ai pesi previsti; se servisse, si alza `link_iters` o si intreccia il
giunto nelle iterazioni PBD (salto di complessità, fuori v1).

### 8.4. Stabilità degli indici (l'unico punto delicato)

I draggable usano swap-and-pop (`simp_drag_remove`, §5): rimuovere il disco `i`
ci sposta dentro l'ultimo. I giunti puntano per indice → vanno **riparati**:

- ogni giunto che referenzia `i` (il disco rimosso) viene **invalidato** (l'auto
  perde una ruota = non è più un'auto): swap-and-pop anche del giunto;
- ogni giunto che referenzia `last` (l'indice che si è spostato in `i`) viene
  **rimappato** `last → i`.

Alternativa scartata per v1: handle stabili sui draggable (come gli agenti M3.1).
Sovradimensionato — i draggable sono decine e il gioco già tiene una sua mappa
(§5). Il fixup lineare su `LINK_CAP` piccolo basta.

### 8.5. API

```c
/* Collega due draggable esistenti con un rod rigido; rest = distanza CORRENTE
 * tra i loro centri al momento della chiamata. Ritorna l'indice del giunto
 * (non stabile) o -1 se il pool è pieno o un indice non è valido. */
int  simp_drag_link(SimP *s, int i, int j);
void simp_drag_unlink(SimP *s, int k);     /* swap-and-pop del giunto k */
int  simp_drag_link_count(const SimP *s);
```

Helper lato gioco (NON nel core, vive nell'host che conosce "l'auto"):
crea due `simp_drag_add` alla distanza voluta + un `simp_drag_link`, salva la
coppia di indici draggable nella sua mappa per HP / render / despawn. Il render
di un'auto pesca i due centri (`simp_drag_px/py`) e disegna lo sprite orientato
lungo `p_b − p_a` (l'orientamento è gratis dai due dischi). `simp_drag_remove` di
un disco-auto va sempre accompagnato dall'unlink lato gioco (o ci pensa il fixup
§8.4 invalidando il giunto orfano).

### 8.6. Verifica (`test_car.c`, deterministico headless)

1. **Rigidità**: auto a riposo, folla che preme un'estremità → distanza tra i due
   centri resta ≈ `rest` (entro qualche % a `link_iters` default) per tutto il
   transitorio; nessuna deriva cumulativa.
2. **Rotazione emergente**: folla che colpisce UN solo disco → l'auto **ruota**
   (l'angolo di `p_b−p_a` cambia di >X°) facendo perno, senza momento angolare
   esplicito.
3. **Scivola come barricata**: un'auto di traverso in un corridoio blocca più di
   un disco singolo della stessa massa (drain minore); sotto folla sufficiente
   viene comunque shovata via (drain > 0) → reroute.
4. **Collisione muro**: auto spinta contro un muro, nessuno dei due centri penetra
   (sdf ≥ −ε per entrambi); il rod non "spara" il disco oltre il muro.
5. **Esplosione**: un blast scaglia l'auto (entrambi i dischi via `1/massa`) e il
   rod la tiene insieme mentre vola; poi attrito → ferma.
6. **Fixup indici**: aggiungi/collega/rimuovi draggable in ordine vario, verifica
   che ogni giunto vivo punti ai dischi giusti e nessuno punti a un indice morto
   (contro shadow map brute force, come `test_handles`).
7. **Determinismo** (re-run bit-identico) **+ no NaN / out-of-bounds**.

**Verificato** (`test_car`, 2026-06-28): rod **bit-rigido** sotto piena pressione
della folla (deviazione worst 0.0% in tutti gli scenari — `link_iters` 4 basta e
avanza per masse 5–20); rotazione emergente 90°→54° con folla su un solo disco
(perno, zero stato angolare); penetrazione muro worst 1.7 cm (rientro lo step
dopo, dentro il limite v1); blast → 1.07 m di volo, rod intatto, fermo per
attrito (0.002 m/s); fixup indici consistente con shadow model su 400 op miste;
determinismo bit-identico. Zero NaN. `test_drag`/`test_hybrid` invariati (nessuna
regressione sui draggable singoli).

### 8.7. Mappatura compute

Il giunto è una proiezione per coppia su un pool piccolo: un dispatch seriale (o
un kernel a una thread per giunto) accanto al recupero draggable. I dischi-auto
restano dischi normali nel counting-sort + PBD a tile: la mappa GPU del loop
caldo non cambia. Resta valido il vincolo "niente stato che rompa il dispatch a
compute" del core.

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
diventassero migliaia. Niente joint, niente rotazione, niente momento angolare:
un oggetto = un disco (un'auto "vera" rigida sarebbe il salto di costo descritto
nella nota di fattibilità, fuori scope).

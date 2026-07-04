# Classificazione unificata degli oggetti — design tecnico

> **STATO: v1 IMPLEMENTATA (2026-07-03).** L'asse C è nel core
> (`simp_set_opacity` + `simp_ray_transmit`, §4), le torrette acquisiscono a
> soglia `T_ACQ` e scalano il danno per la trasmittanza (`defense.c`; il colpo
> pesante accumula `hheat += transmit`, per cui dietro una cancellata servono
> ceil(1/transmit) colpi), il catalogo prop parsa le colonne §6
> (`props.c`, campi `solid/height/hp/cost_mult/opacity/mass` + footprint
> `fw/fd` dalla colonna `WxD` §8.5). Verifica: `test_cover` (piano §7
> completo) + `test_props` (§6). **APPLICAZIONE HOST FATTA (2026-07-04)**:
> `prop_world.c` (`prop_world_apply`, chiamato da `build_world` prima del
> prefill) rasterizza il footprint W×D ruotato dello yaw (scanline condivisa
> `scene_raster_cells`), hp finiti → struttura assediabile `def_add_structure`
> con tier `cost_mult`, hp inf/0 → muro permanente a tier palazzo, opacità
> per-cella (default 1 sui solidi). Verifica: `test_prop_world`. Loader glb
> dei prop (colonna mesh, EDITOR_PLAN E5) in `vat_horde` (`load_prop_models`):
> triangle soup pos+nrm+baseColorFactor, fallback placeholder, scurimento col
> danno e sparizione al crollo; libreria `blend/props.blend` →
> `gfx/props_export_glb.py` → `assets/models/props/*.glb`. Banco:
> `assets/scenes/props_demo.scn`. Restano fuori v1: asse D (prop a massa
> finita → corpo draggable) e opacità dei non-solidi (fumo/siepe, §8.2).
>
> Nata dalla riflessione in `considerazioni.txt`
> dopo il line-of-sight delle torrette: le mura bloccano zombie E proiettili,
> ma una CANCELLATA blocca gli zombie e lascia passare (parte de)i proiettili;
> un BUS blocca tutto ma non si distrugge né si trascina; i prop leggeri
> scoppiano al passaggio. Obiettivo: aggiungere un oggetto nuovo = modello 3D
> + una riga di catalogo con le sue caratteristiche.
> Questo doc fissa gli ASSI, l'unica API core mancante e lo schema catalogo.

## 1. Principio

Ogni oggetto del gioco è un punto in uno spazio di **assi ortogonali**, e quasi
ogni asse ESISTE GIÀ nel motore come meccanismo indipendente. La novità non è
implementare la matrice — è (a) riconoscerla come schema unificante del
catalogo, (b) aggiungere l'UNICO asse oggi mancante: l'opacità ai proiettili
separata dal solido.

Regola guida: **assi come PARAMETRI, non booleani**. "Indistruttibile" = HP
infiniti, "non trascinabile" = massa infinita, "blocca i proiettili" = opacità
1.0. Il bus non è un caso speciale: è una riga con due ∞ e un 1.0. Meno enum
di tipo, più manopole — come i tier di `wall_cost` (barricata vs palazzo).

## 2. Gli assi (e dove vivono già)

| # | Asse | Parametro | Meccanismo | Stato |
|---|------|-----------|------------|-------|
| A | Blocca gli zombie | solido sì/no (+ altezza) | cella nav solida + SDF (`simp_set_wall`), `poly solid` | FATTO |
| B | Attaccabile/distruttibile dall'orda | HP + `cost_mult` | sensore d'assedio + `def_add_structure`; tier `wall_cost` decide DOVE l'orda preme | FATTO |
| C | Blocca i proiettili (riparo) | **opacità 0..1** | **NUOVO nel core** — oggi coincide con A (griglia `solid`) | §4 |
| D | Trascinabile | massa (kg walker) | corpo draggable PBD (`DRAG_DESIGN`; leggero/pesante/auto) | FATTO |
| E | Distrutto al passaggio | raggio innesco + stile FX | `destruct.c` one-shot (topple + burst, svanisce) | FATTO |
| F | Effetto nav non solido | peso costo ± | `simp_add_cost` (paura/fango w>0, richiamo w<0) | FATTO |
| G | Altezza | metri | `wall_h` (flyer sorvolano sotto 2 m), altezza render dei `poly` | FATTO |
| H | Piazzabile dal giocatore | budget + veto | `place.c` (PLACEMENT_DESIGN) | FATTO |

Deciso altrove e NON in questo doc: il danneggiamento LENTO al passaggio
(alberi/semafori erosi) è stato TAGLIATO (DESTRUCT_DESIGN, 2026-06-28: i prop
leggeri sono one-shot). Resta in GAME_PLAN solo il "filo spinato v2" come
hazard passabile opzionale (fase D).

## 3. Matrice degli archetipi

Parametri: A=solido, B=HP/cost_mult, C=opacità, D=massa drag, E=crush.

| Oggetto | A | B (HP) | C | D | E | Note |
|---------|---|--------|---|---|---|------|
| Muro/barricata giocatore | sì | finiti, mult basso | 1.0 | — | — | oggi |
| Palazzo/roccia | sì | ∞, tier alto | 1.0 | — | — | oggi |
| **Cancellata** | sì | finiti (assediabile) | **~0.3** | — | — | il caso nuovo: DPS torrette ridotto attraverso |
| **Bus di traverso** | sì | ∞ | 1.0 | ∞ (no) | — | = palazzo con mesh diversa: GIÀ esprimibile |
| Auto/cassonetto | no (corpo PBD) | — | 0 (v1) | finita | — | draggable di oggi; C>0 = riparo mobile, vedi §5 |
| Tavolino/sedia/cartello | no | — | 0 | — | sì | destruct di oggi |
| Bidone in fiamme | no | — | 0 | — | — | solo asse F (paura): l'orda lo evita |
| **Fumo / siepe** | **no** | — | **>0** | — | — | inverso della cancellata: riparo per l'ORDA senza ostacolo. Meccanica futura (screamer dietro copertura) |

La riga bus dimostra il valore dello schema: NON serve codice nuovo, solo la
riga di catalogo. Cancellata e fumo sono gli unici che chiedono l'asse C.

## 4. L'asse C nel core: opacità ai proiettili per-cella

Unica aggiunta al core. Simmetrica a `wall_cost[]` (per-cella, accanto a
`solid`), stessa filosofia: la collisione (A) e l'occlusione (C) diventano
indipendenti, come già collisione e costo nav.

    /* per-cell bullet opacity in [0,1]; default: 1 for solid cells, 0 for
     * open ones (bit-compatible with today). Set AFTER simp_set_wall. */
    void  simp_set_opacity(SimP *s, int cx, int cy, float opacity);

    /* transmittance along the ray: product of (1 - opacity) over the cells
     * crossed, each weighted by the length of ray inside the cell (m) over
     * a reference thickness (1 cell). 1 = clear, 0 = fully blocked.
     * simp_wall_ray resta com'è (= transmittance che tocca 0). */
    float simp_ray_transmit(const SimP *s, float ox, float oy,
                            float dx, float dy, float maxdist);

Uso in `defense.c` (turret_update):

- **Acquisizione**: bersaglio valido se `transmit >= T_ACQ` (default ~0.05):
  la torretta vede e ingaggia attraverso la cancellata, ignora chi è dietro
  un muro pieno. Sostituisce il check binario `simp_wall_ray` di oggi (che
  resta come fast-path quando non ci sono celle semi-trasparenti).
- **Danno**: `dmg *= transmit` al colpo. DETERMINISTICO: niente estrazione
  "questo proiettile passa/non passa" — il moltiplicatore ha lo stesso DPS
  atteso della lotteria per-proiettile ma zero stato RNG in più, coerente con
  l'etica del core. (Il tracer di render può comunque fermarsi sulla
  cancellata una frazione delle volte: fiction visiva, non stato di gioco.)
- **`simp_query_ray`**: i dischi restano occlusi solo dal solido "vero"
  (transmit==0). Un proiettile attraverso la cancellata COLPISCE, ma ferisce
  meno.

Implementazione: il DDA esiste già (`wall_ray_t`); la variante accumula
`log`-prodotti invece di fermarsi alla prima cella solida. Early-out a
transmit < epsilon. Costo: identico a oggi sulle mappe senza celle
semi-trasparenti (flag globale "has_opacity" per saltare tutto).

Interazione con B (assedio): la cancellata è una struttura assediabile
normale (`def_add_structure` + tier `wall_cost` basso = bersaglio preferito).
Al crollo, `simp_set_wall(false)` azzera anche l'opacità. Nessun caso nuovo.

## 5. Interazioni tra assi — da DECIDERE a tavolino

1. **D + C (riparo mobile)**: un'auto trascinata dall'orda con opacità > 0
   diventa uno scudo semovente contro le torrette. Feature potente (e bella)
   ma da bilanciare; v1 propone D→C=0 (i draggable non fanno riparo), si
   accende dopo. I draggable inoltre NON vivono nella griglia nav (sono corpi
   PBD): dare loro opacità richiede rasterizzarli per-step o testare i dischi
   nel raggio — costo da valutare, non gratis come le celle.
2. **B + C (riparo che degrada)**: opacità che scala con gli HP (cancellata
   sfondata a metà = più trasparente)? v1 NO: opacità costante finché la
   struttura vive, 0 al crollo. Un solo salto, leggibile.
3. **Esplosioni attraverso i muri**: `simp_apply_impulse` oggi ignora il
   solido (shova anche oltre la parete). Caso limite noto, NON parte di
   questo design; se mai serve, è lo stesso `simp_ray_transmit` applicato al
   falloff radiale.
4. **C degli oggetti alti vs flyer**: un proiettile passa SOPRA una jersey
   bassa? v1 NO: l'opacità è 2D come tutto il resto del combat. L'asse G
   resta solo nav (flyer) e render.

## 6. Catalogo unificato

Estensione RETRO-COMPATIBILE di `assets/props/catalog.txt` (stesso pattern dei
token distruttibili: assente = default inerte). Colonne nuove opzionali:

    # key    mesh  scale label   [trig debris topple] [solid H] [hp mult] [opac] [mass]
    fence    -     1.0   Fence   -    -      -        solid 1.8  350 0.6   0.3    -
    bus      -     3.0   Bus     -    -      -        solid 2.6  inf -     1.0    inf
    wreck    -     1.4   Wreck   -    -      -        -          -   -     0.5    120

`solid H` marca il footprint del prop nella nav all'instanziazione (come i
`poly solid` di scena, ma data-driven dal catalogo — oggi il footprint statics
è già rasterizzato dall'exporter, qui diventa attributo del TIPO); `hp mult`
lo registra come struttura assediabile; `opac` = asse C; `mass` = draggable
(inf = non trascinabile). I `wall`/`turret` di scena restano come sono; a
regime anche loro possono convergere su questo schema, ma NON in v1.

## 7. Piano di verifica

- `test_cover` (o caso nuovo in `test_defense`): torretta + bersaglio dietro
  cancellata (opac 0.3) → il DPS misurato scala ~×0.7 vs campo aperto; dietro
  muro pieno → zero danno e la torretta NON ingaggia (regression del LoS
  2026-07-03); acquisizione preferisce un bersaglio più lontano ma in chiaro
  rispetto a uno vicino dietro muro pieno, ma NON rispetto a uno dietro
  cancellata (transmit sopra soglia). Determinismo + no-NaN.
- `simp_ray_transmit`: celle note attraversate in diagonale → trasmittanza
  attesa a mano; mappa senza opacità → bit-identico a `simp_wall_ray`.
- Crollo cancellata → opacità 0 → DPS pieno (riusa la meccanica di
  `test_turret_siege`).

## 8. Decisioni aperte (per l'utente)

1. Valore di gioco della cancellata: opac 0.3 (torrette ancora utili
   attraverso) o più punitivo (0.6)? Da provare in sandbox.
2. Il fumo/siepe (C>0, A=no) entra in v1 o resta nel cassetto finché non c'è
   la meccanica che lo usa (screamer/copertura)?
3. Riparo mobile (auto trascinata): mai, dopo, o subito?
4. Nome definitivo dell'asse nel codice: `opacity` (proposto) vs `cover`.
   → v1 implementata con `opacity` (2026-07-03).
5. (emersa in implementazione) Footprint dei prop `solid`: lo schema §6 ha
   solo l'altezza H — per marcare la nav in `build_world` servono anche le
   dimensioni in pianta. Colonna `WxD` nel catalogo, o footprint derivato
   dal bbox del glb (come fa l'exporter per gli statics)?
   → **DECISA (2026-07-03, input utente): colonna `WxD` nel catalogo.**
   → **IMPLEMENTATA (2026-07-04)**: token opzionale `WxD` dopo `solid H`
   (`props.c`, campi `fw/fd`; omesso = una cella nav), applicata da
   `prop_world_apply` (`prop_world.c`, testata in `test_prop_world`).
   Tutti i prop-ostacolo sono RETTANGOLI in pianta: muri/cancellate/barriere
   = SEGMENTI singoli tileabili (es. 0.5×2 o 0.5×3 m) da affiancare per
   ostacoli più lunghi; bus e cassonetto = rettangoli anch'essi. Lo spessore
   minimo utile è la cella nav (0.5 m) anche se la mesh è più sottile: la
   piccola discrepanza vista/collisione è accettata. Implementazione
   naturale: rettangolo W×D ruotato dello yaw dell'istanza → rasterizzato
   come i `poly solid` (scanline convessa già esistente); a 0°/90° su
   posizioni multiple di cella mappa 1:1 sulle celle. NOTA: vale per i prop
   STATICI (bus = celle nav); i prop TRASCINABILI (cassonetto, mass finita)
   non vivono nella griglia — il rettangolo è solo visivo, la collisione è
   il corpo PBD (disco, o 2 dischi+rod come l'auto).

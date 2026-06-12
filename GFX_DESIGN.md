# Grafica — design e direzione artistica

Decisioni prese nella sessione di direzione artistica (giugno 2026). Questo
documento fissa il *cosa*; lo stack tecnico di rendering (SDL_GPU vs OpenGL,
formati dei buffer, compute) è la sessione successiva e parte da qui.

Principio guida: la simulazione resta metrica 2D in metri; tutto ciò che è
"profondità" è un trucco di disegno, mai una proiezione. Il renderer legge gli
SoA del core (`simp_px/py/vx/vy/radius/flags/z`) come instance buffer e non
chiede al core nulla che rompa la mappatura su compute shader.

---

## 1. Vista e proiezione

**Ortografico top-down con sprite in 3/4 ("alla Warcraft 2").** Nessuna
prospettiva reale: mondo→schermo è una moltiplicazione uniforme (px/m), il
picking è banale, il raggio di una torretta è un cerchio. La profondità
percepita viene da tre trucchi:

- **Sprite in 3/4**: il modello 3D è ripreso da una camera inclinata
  (~40–45° dall'orizzonte), quindi si vede il fronte dei personaggi, non solo
  il cranio. La mappa sotto resta piatta vista dall'alto.
- **Y-sort**: gli sprite si disegnano in ordine di `y` mondo crescente (chi è
  più in basso copre chi è più in alto). Serve solo a zoom tattico; ai tier
  inferiori l'ordine è irrilevante.
- **Offset z per il volo**: un agente `SIMP_FLYING` si disegna a
  `y_schermo − z·k_z` (k_z ≈ scala verticale del 3/4, ~0.7–1.0) con
  un'**ombra ellittica** che resta alla posizione di terra. La parabola
  balistica di M3.2 diventa leggibile gratis. Vale anche per i gib (vedi §5).

Perché NON la prospettiva vera: lettura delle distanze distorta (in un TD è
il gameplay), sorting e scale per-istanza su 100k sprite, picking non banale.

## 2. Scale, zoom, viewport

Unità mondo: metri (come il core). Scala di riferimento: **32 px/m nativi**
(zoom tattico). Cella nav 0.5 m = 16 px; "tile" visivo 1 m = 32 px.

Tier di zoom discreti, passi ×2, ciascuno con la sua rappresentazione (LOD
di rendering):

| Tier        | px/m | Schermo @1080p | Agenti a schermo (max geom.) | Rappresentazione |
|-------------|------|----------------|------------------------------|------------------|
| Tattico     | 32   | ~60×34 m       | ~5–6k                        | sprite 16 dir animati (~20–24 px) |
| Medio       | 16   | ~120×68 m      | ~22k                         | sprite ridotti, metà frame |
| Strategico  | 8    | ~240×135 m     | ~90k                         | quad tintati 4–5 px |
| Mappa       | —    | tutto il livello | tutti                       | campo di densità colorato |

- Il tier Mappa e la **minimappa** sono lo stesso renderer: visualizzazione
  diretta di `rho` (e in futuro del campo del core continuo). Il LOD di
  rendering lontano e il LOD di simulazione lontano condividono la
  rappresentazione: un solo concetto, un solo confine.
- **Viewport scorrevole** (edge pan + WASD/frecce + click sulla minimappa).
  La schermata singola è esclusa dall'aritmetica: 100k zombie impaccati
  occupano ~36.000 m², un livello credibile è 250–500 m di lato.
- Lo zoom NON è continuo: tier discreti, transizione con crossfade breve.
  Evita il pumping degli sprite e rende espliciti i cambi di rappresentazione.

## 3. Dimensioni dei livelli

Cella nav 0.5 m invariata. Tre taglie:

| Taglia | Metri      | Celle nav  | Note |
|--------|-----------|------------|------|
| S      | 128×96    | 256×192    | arena compatta, tutorial |
| M      | 256×192   | 512×384    | taglia standard |
| L      | 512×384   | 1024×768   | tetto massimo |

A taglia L il Dijkstra gira su ~790k celle: il ricalcolo throttled
(`flow_period`) andrà spostato su un thread dedicato — è già asincrono per
natura, non è un problema architetturale. La zona attiva particellare resta
comunque legata al viewport (il lontano è del core continuo, vedi CLAUDE.md).

Il terreno di un livello L a 32 px/m sarebbe 16k×12k px: niente immagine
unica. Terreno = tileset + blending per chunk + layer decal (§5).

## 4. Sprite: stile e pipeline

**Stile: realistico-caricato prerenderizzato, piena profondità di colore.**
Niente estetica pixel-art dichiarata. Riferimenti: Diablo 2, Commandos, AoE.

- **Avvertimento chiave**: il realismo proporzionato legge male a 20 px.
  Correzioni da fare NEL MODELLO, non in post: proporzioni leggermente
  caricate (testa e mani più grandi), silhouette forti, rim light baked che
  stacca la figura dal terreno. Coerente col tono gore-comico.
- **Pipeline: prerender da Blender** (scelta fissata). Un modello low-poly
  per tipo, animazioni, e uno script che sputa le sprite sheet:
  **16 direzioni** × N frame × stati. Le direzioni costano solo render time:
  16 invece di 8 perché l'orda che gira attorno agli ostacoli si vede.
- **Un solo rig di luce per TUTTI gli asset** (personaggi, torrette, decoro):
  luce chiave da nord-ovest stile RTS classico, camera unica a ~45°. La
  coerenza di scena è gratis per costruzione. Da fissare in un .blend
  template prima di produrre il secondo asset.
- Stati animazione minimi per zombie: walk (8 frame), tumble/volo (4),
  attack (4, per il futuro stato attacking), rialzata da landing (4).
  Selezione stato dal renderer: flags del core (FLYING, DORMANT) + soglie di
  velocità (sopra soglia → tumble: ogni schizzo di fisica residuo diventa
  azione leggibile, non glitch).
- **Varietà senza asset**: tinta e scala per hash dello slot (già in TODO),
  ±10% scala, variazione hue/sat contenuta. Tipi (walker/runner/tank)
  = modelli/palette distinti.
- **Facing**: il core non memorizza un heading e la velocità istantanea è
  sporcata dal PBD. Heading = EMA della velocità per slot (può viverlo il
  renderer, indicizzato per slot; ~0.2–0.3 s di costante di tempo), poi
  quantizzato a 16 settori. La quantizzazione assorbe il rumore residuo.
- Budget VRAM: 16 dir × 8 frame × 64×64 RGBA ≈ 2 MB per animazione per tipo.
  Anche con 4 tipi × 4 stati siamo nell'ordine delle decine di MB: non è un
  vincolo. Atlas unico per tier di zoom (il tier Medio usa mip dedicate
  pre-ridotte, non il minify automatico, per controllare la leggibilità).

## 5. Gore (sistema, non decorazione)

Tono: esagerato fino al comico. Tre sottosistemi, tutti già appoggiati a
meccaniche esistenti del core:

1. **Gib volanti**: alle morti esplosive, spawn di "particelle frattaglia"
   renderer-side lanciate con la stessa balistica di M3.2 (parabola + ombra
   a terra). Non entrano nella sim (niente collisioni): vita breve, solo
   visuali, atterrando stampano un decal.
2. **Decal persistenti**: layer render-to-texture per chunk di terreno su
   cui ogni morte stampa schizzi che NON si cancellano. A 100k morti il
   campo diventa progressivamente un quadro dell'orrore comico. Costo ~zero.
   (Tetto di sanità: clamp di saturazione per chunk, non rimozione.)
3. **Cadaveri-ostacolo**: già nel core (M3.3). Sprite dedicato per i corpse
   (pool 4096), mucchi visibili su cui l'orda inciampa e che barricano i
   varchi: il gore È meccanica. Al TTL: dissolvenza nel decal layer.

Nota di design: il tono comico ha una funzione tecnica — copre gli artefatti
di simulazione. Lo zombie espulso a velocità ridicola è un highlight, non un
bug report.

## 6. Asset ambientali e muri obliqui

Decisione: **doppio regime**, che scioglie il dilemma griglia-vs-obliquo.

- **Costruzioni del giocatore: snap a griglia** (perpendicolari alla WC2).
  È la scelta giusta a prescindere dalla tecnica: chiarezza di piazzamento,
  costo per segmento, distruttibilità per cella (muri con HP, in TODO).
- **Decoro del livello (statico, autorato): forma libera a qualunque
  angolo**, rasterizzata nella griglia nav al piazzamento. Il punto tecnico
  che lo rende possibile: gli agenti collidono con l'**SDF** (campo continuo,
  campionato bilineare), non con le celle — la scaletta di rasterizzazione
  produce un'isolinea già smussata, e il flow field a 8 vicini digerisce le
  diagonali (45° nativi; angoli arbitrari frastagliati ma nascosti da
  smoothing + inerzia dello steering). Fisicamente funziona già oggi.
- Il disallineamento visivo bordo-sprite vs bordo-collisione è ≤ mezza cella
  (~25 cm, ~8 px a zoom tattico). Regola di art direction che lo nasconde:
  **barriere "grasse" e organiche** (sacchi di sabbia, macerie, carcasse
  d'auto, barricate di assi) la cui silhouette irregolare perdona tutto.
  Da evitare: recinzioni sottili e nitide ad angoli arbitrari.
- Manopola di riserva se l'occhio lo chiedesse: cella nav a 0.25 m (4× costo
  Dijkstra/SDF). Da valutare solo allora, insieme alla questione aperta
  sull'unificazione celle nav/collisione per la GPU.

## 7. Cosa serve al core (poco)

- Niente: heading EMA, stato animazione, gib e decal vivono nel renderer
  (indicizzati per slot dove serve persistenza).
- Unica candidata futura: esporre `phi`/`rho` già c'è (`simp_density_arr`,
  `simp_jam_arr`); per il tier Mappa servono così come sono.
- Vincolo da rispettare: gli SoA restano gli instance buffer. Qualunque dato
  per-agente nuovo nel renderer si indicizza per SLOT (`simp_slot_of`),
  stabile per la vita dell'agente.

## 8. Decisioni rimandate / aperte

- **Stack tecnico** (prossima sessione): SDL_GPU API vs OpenGL 4.3. Per il
  rendering qui descritto sono equivalenti; decide il lato compute (M5) e la
  portabilità. Questo documento è l'input di quella scelta.
- **VAT (vertex animation textures) come alternativa al prerender**, da
  rivalutare alla sessione stack. Conti fatti (giugno 2026): una sheet
  16 dir × 16 frame @ 64 px = 4.2 MB → ~50 MB per modello completo di
  stati; fino a 2–3 modelli base il prerender sta comodo nel budget anche
  senza compressione (BC7 = ÷4). Il VAT vince su memoria (~100 KB/clip per
  uno zombie da 1k vertici), direzioni continue (niente quantizzazione a
  16), luci dinamiche e smembramenti per-bone; perde perché riapre la
  decisione estetica di §4 (lowpoly illuminato realtime ≠ prerenderizzato
  alla Diablo 2), presuppone lo stack GPU custom, e NON elimina gli
  sprite: ai tier lontani (90k agenti da 5 px) servono comunque impostor
  2D → due pipeline da mantenere. Trigger per riaprirla: servono direzioni
  continue, illuminazione dinamica sui personaggi, o un numero di
  modelli×animazioni che sfonda i ~500 MB di atlas. Prospettiva lunga:
  l'eventuale salto 2D→3D "alla Warcraft 2→3" è un progetto di rendering
  nuovo, non un'evoluzione di questo — il core di simulazione, che è
  renderer-agnostico, sopravvive identico.
- La varietà dell'orda si governa con gli assi ECONOMICI, già
  moltiplicativi tra loro: N walk × 8 tinte × scala ±15% × fase
  indipendente = migliaia di zombie distinti con UN modello. I modelli
  base aggiuntivi sono per i TIPI (walker/runner/tank, già previsti
  sopra), dove il costo vero è l'authoring, non la VRAM.
- **Palette/mood del mondo**: notturno classico zombie vs terroso diurno
  alla WC2. Da decidere col primo tileset; non blocca nulla di tecnico.
- Crossfade tra tier di zoom: durata e curva, da tarare a occhio.
- Formato sheet/atlas definitivo e tool di packing: con lo stack.

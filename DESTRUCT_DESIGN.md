# Prop decorativi distruttibili — design tecnico

> **STATO: IN CORSO (2026-06-28).** Tavolini/sedie che si frantumano in schegge,
> cartelli/semafori che vengono abbattuti e fatti a pezzi quando l'orda li
> raggiunge. **Puramente visivi**: i core di simulazione non li vedono (come
> tutti i prop di decoro, EDITOR_DESIGN §10 stadio 5b). Si compone da pezzi che
> già esistono — prop catalog + FX particellari + query spaziali — più un
> piccolo modulo di gioco core-agnostico (come `defense.c`).

## 1. Principio

Un prop di decoro distruttibile è un oggetto **render-only** che, quando un
agente dell'orda gli arriva abbastanza vicino, **si distrugge una volta sola**:
gli oggetti bassi (tavolino, sedia) scoppiano subito in detriti; quelli alti
(cartello, semaforo) prima **si abbattono** (inclinazione di ~0.3-0.4 s nella
direzione di spinta dell'orda), poi scoppiano. I detriti sono un **burst di
particelle** (`fx_particles`) che vola via; dopo, l'oggetto **svanisce** — niente
residuo fisico, niente effetto su nav/collisione (l'orda ci cammina sopra).

Decisioni (2026-06-28, utente): **topple poi pezzi** per gli alti; **solo FX,
svanisce** (nessun detrito fisico — quello è il dominio dei draggable, `DRAG_DESIGN.md`).

## 2. Dove si incastra (niente tocca i core)

- **Catalogo prop** (`props.h/.c`): la distruttibilità è un attributo del TIPO,
  data-driven nel catalogo testo. Retro-compatibile: assente = decoro inerte
  (comportamento di oggi).
- **Modulo di gioco `destruct.h/.c`** (nuovo, core-agnostico come `defense.c`):
  possiede lo STATO runtime per-prop, rileva il contatto via `simp_query_circle`,
  e a rottura avvenuta emette un **evento one-shot** via callback (come
  `DefEventFn`). Deterministico — legge solo posizioni/velocità degli agenti.
- **Host `vat_horde`**: mappa lo stile detriti → preset `FxEmitterDef` ed emette
  il burst alla callback; salta i prop distrutti dalla mesh e inclina quelli in
  abbattimento; chiama `destruct_update` dopo `simp_step`.

Il sim core resta intatto: nessuna API nuova nel core (riusa `simp_query_circle`,
`simp_vx/vy`).

## 3. Catalogo (`props.h` + `props/catalog.txt`)

Campi nuovi in `PropDef` (default = decoro inerte):

```c
int   destructible;     /* 0 = decoro puro (default), 1 = si frantuma */
float trigger_radius;   /* m: un agente entro questo raggio dal prop lo innesca */
char  debris[16];       /* stile FX ("wood" / "metal" / "glass"...), host->preset */
float topple;           /* s di abbattimento prima dello scoppio (0 = istantaneo) */
```

Formato file esteso (posizionale, retro-compatibile):

```
# key   mesh  scale  label   [trig  debris  topple]
bench   -     1.0    Bench                              # 4 token = inerte (oggi)
table   -     1.0    Table   1.2   wood    0.0          # 7 token = distruttibile
sign    -     1.0    Sign    1.2   metal   0.35         # alto: si abbatte
```

4 token → inerte. 7 token → `destructible=1` con trig/debris/topple. Parsing
best-effort come il resto del loader.

## 4. Modulo `destruct.h/.c`

Stato per-prop (indicizzato come `Scene.prop[]`, indici stabili: la scena non
rimuove prop a runtime):

```c
enum { DESTRUCT_INERT=0, DESTRUCT_ALIVE, DESTRUCT_TOPPLING, DESTRUCT_GONE };
```

- `INERT` = tipo non distruttibile → render normale, mai gestito.
- `ALIVE` = distruttibile intatto → render normale, sotto sorveglianza.
- `TOPPLING` = abbattimento in corso → render inclinato (progresso 0..1).
- `GONE` = distrutto → non renderizzato.

`destruct_update(d, sim, scene, catalog, dt, on_burst, ud)`:
- per ogni `ALIVE`: `simp_query_circle(prop.x, prop.y, trigger_radius)`. Se ≥1
  agente: cattura la **direzione di spinta** = media delle velocità degli agenti
  nel raggio (fallback `prop.rot` se ferma); poi se `topple>0` → `TOPPLING`
  (timer = topple), altrimenti scoppio immediato (`on_burst`, → `GONE`).
- per ogni `TOPPLING`: `timer -= dt`; a `≤0` → scoppio (`on_burst`) → `GONE`.
- ritorna se c'è stato un cambio di stato (l'host re-uploada la mesh).

`on_burst(prop_index, debris_style, x, y, dir, ud)` — l'host crea l'FX.
`destruct_topple_t(d, i)` → progresso 0..1 dell'inclinazione (per il render).
`destruct_state(d, i)`, `destruct_animating(d)` (qualcuno in `TOPPLING`).

**Determinismo:** `simp_query_circle` è esatta tra due step, velocità
deterministiche, timer deterministico → le DECISIONI (quale prop cade, quando)
sono deterministiche e testabili. L'FX (RNG visivo) vive nell'host, fuori dal
test (scelta di progetto: gli FX non hanno test deterministico).

**Stantio:** `destruct_update` va dopo `simp_step`. Se il gioco ha già fatto
`simp_kill` (torrette) la griglia è stantia e la query cade su brute-force — più
lenta ma **corretta** (pochi prop). Nel test la chiamiamo subito dopo lo step.

## 5. FX detriti (host `vat_horde`)

Due preset `FxEmitterDef` (come `BLOOD_DEF`), mappati da `debris`:
- **wood**: ~14 schegge marroni piccole, gravità, `ground_stop`, vita breve.
- **metal/glass**: ~20 frammenti grigi + qualche variante ciano (vetro), un po'
  più grandi e veloci.

Emessi con `fx_emit(fx, origin, def, dir, half_angle)`: `dir` = direzione di
spinta catturata, cono ~35° → i pezzi **schizzano via** nel verso in cui spinge
l'orda. `origin.z` = `terrain_z(x,y)` + offset di mezza-altezza dell'oggetto.

## 6. Render abbattimento (host)

La mesh placeholder dei prop viene ri-costruita quando `destruct` segnala un
cambio o c'è un `TOPPLING` in corso (pochi prop, costo trascurabile). Per un
`TOPPLING`: il corpo-box viene **inclinato** ruotando la faccia superiore attorno
alla base nel piano verticale di `dir` (top spostato di `h·sinθ` lungo `dir`,
abbassato a `h·cosθ`, con `θ = t·θmax`, `θmax≈80°`). `GONE` → saltato. Con la
mesh `.glb` reale (futuro) l'inclinazione diventa una rotazione del nodo.

## 7. Verifica

`test_destruct.c` (deterministico, headless — la LOGICA, non l'FX):
1. **Contatto istantaneo**: prop `topple=0` nella traiettoria della folla →
   esattamente UN burst, allo step in cui il primo agente entra nel raggio;
   nessun secondo evento; stato finale `GONE`.
2. **Abbattimento**: prop `topple>0` → entra in `TOPPLING` al contatto, scoppia
   `round(topple/dt)` step dopo (non subito).
3. **Inerte**: un prop non distruttibile (o fuori portata) non scoppia mai.
4. **Direzione**: `dir` catturata ≈ direzione di marcia della folla.
5. **Determinismo**: due run → stessi step/ordine di burst.

I visivi (schegge, abbattimento) si verificano A OCCHIO in `vat_horde` (preset
FX, cono, tilt), come da metodo per le modifiche di rendering.

## 8. Mappatura / vincoli

Nessun impatto su core o compute: è tutto host-side + un modulo di gioco che
legge la sim in sola lettura. Costo runtime: una `simp_query_circle` per prop
distruttibile vivo per step (decine al massimo) → trascurabile.

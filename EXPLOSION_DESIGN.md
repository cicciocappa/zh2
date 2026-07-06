# Esplosioni × mondo — design tecnico

> **STATO: CORE FATTO (2026-07-06), fiction host da fare.** Fase 1 (parte
> deterministica, `test_blast` PASS): `def_blast` + `def_damage_agent` in
> defense.c (§3.1–3.3 + danno diretto agenti), `destruct_force` in destruct.c
> (§4/§5 burst programmatico), colonne catalogo `resist`/`burn` in props
> (§5). NOTA: `def_blast` vaporizza la PILA di cadaveri (`simp_corpse_clear`:
> mass/pack/height) ma NON il campo `danger` (blood-fear): non esiste una
> primitiva core per azzerare `danger` in un raggio e il design vieta nuove
> API core → il sangue resta (e decade da solo), coerente con la fiction. Fase
> 2 DA FARE (visiva, in `vat_horde`): `host_blast` (archetipi prop §4, FX §6,
> scorch §7), pool decal, drain `simp_landed`→`def_damage_agent` (§3.4),
> migrazione E/RMB/`VAT_HORDE_BLAST`.
>
> Decisioni §10 sciolte dall'utente il 2026-07-04. Nato dall'arrivo dei prop solidi
> (ENTITY_DESIGN §6+§8.5 applicati, `prop_world.c`): ora che il mondo è
> popolato di oggetti, serve fissare come reagiscono a un'esplosione. Questo
> doc definisce la **primitiva di blast condivisa** che useranno l'`E`/RMB del
> sandbox (oggi solo impulso), le **mine** (GAME_PLAN fase D) e
> **mortaio/bombardamento** (fase F): quelle fasi decideranno *quando e dove*
> esplode; qui si decide *cosa succede* quando esplode.

## 1. Principio

Un'esplosione è UN evento con quattro parametri — `(x, y, raggio R,
danno D0)` più i già esistenti `strength/up_ratio` dell'impulso — e il mondo
risponde **per archetipo, non per caso speciale**: ogni categoria di oggetto
reagisce col meccanismo che GIÀ possiede (impulso, HP, burst, decal), e il
catalogo aggiunge solo due manopole nuove (`resist`, `burn`). Il danno scala
col falloff lineare `D(d) = D0·(1−d/R)`, lo stesso dell'impulso: una sola
geometria per fisica e danno, leggibile.

**Friendly fire pieno**: barricate, torrette e prop del giocatore nel raggio
prendono danno come tutto il resto. È LA meccanica di skill delle armi ad
area — mina sotto la propria barricata = barricata persa.

## 2. Cosa esiste già (inventario)

| Pezzo | Dove | Stato |
|---|---|---|
| Impulso radiale + lancio in aria | `simp_apply_impulse_ex` (falloff lineare, up_ratio) | FATTO, wired su E/RMB |
| Volo + atterraggio | M3.2, buffer `simp_landed` (handle drain-safe) | core FATTO; **il danno da caduta NON è mai consumato dall'host** — buco noto, si chiude qui |
| Cadaveri: rimozione ad area | `simp_corpse_clear(x,y,r)` — nato come "fuoco/acido" | FATTO, mai chiamato dal gioco |
| Strutture con HP + crollo + reroute | `def_add_structure`/`collapse_structure` (+ detriti Hybrid `def_struct_set_debris`) | FATTO; **manca un'API pubblica di danno** (oggi solo l'assedio interno) |
| Torrette distruttibili | struttura 1-cella (`def_turret_make_destructible`) | FATTO → il danno-strutture le copre gratis |
| Prop assediabili / indistruttibili | `prop_world.c` (§6: hp finiti → struttura; inf → muro palazzo) | FATTO → idem |
| Prop one-shot (tavolini…) | `destruct.c` (topple+burst, FX debris) | FATTO; **manca il trigger programmatico** (oggi solo contatto orda) |
| Corpi draggable | ricevono già l'impulso (`drag_impulse` dentro `simp_apply_impulse`) | FATTO |
| Particle system generale | `fx_particles` (burst `fx_emit` + emitter continui `fx_start_emitter` con durata) | FATTO; mancano i preset fuoco/fumo/esplosione |
| Decal persistenti a terra | pool sangue + pool corpse-decal (pattern instanziato) | FATTO; manca il pool bruciature |
| Campo paura che decade | `simp_add_danger` (oggi timbrato dal sangue) | FATTO, generico |

Il core sim NON chiede nessuna API nuova: è tutta orchestrazione game-side.

## 3. Architettura: `def_blast` + fiction host

Due livelli, come per il resto del gioco (defense = stato, host = fiction):

    /* defense.c — la VERITÀ di gioco, deterministica, testabile headless */
    void def_blast(DefGame *g, float x, float y, float r,
                   float dmg, float strength, float up_ratio);

`def_blast` fa, nell'ordine:
1. **agenti**: danno `D(d)` a ogni agente nel raggio via il path di danno
   esistente (`apply_damage`: ferite/gib/morte emettono i soliti eventi
   `DEF_EV_*` → il layer VAT fa gib e sangue gratis); poi
   `simp_apply_impulse_ex` (i morti diventano cadaveri… che un secondo
   blast può spazzare);
2. **strutture**: per ogni struttura con almeno una cella nel raggio, danno
   `D(d_min)` (d_min = cella più vicina al centro) al pool HP condiviso —
   crollo → `collapse_structure` esistente (celle libere, reroute, detriti
   Hybrid). Copre muri di scena, barricate piazzate, torrette, prop
   assediabili: UN path;
3. **cadaveri**: `simp_corpse_clear(x, y, r)` — vaporizzati, i campi nav si
   azzerano (la pila non fa più paura/costo);
4. **danno da caduta** (chiude il buco M3.2): a fine step l'host drena
   `simp_landed` e applica `dmg_fall = k_fall·max(0, v_impatto−v_safe)` via
   defense. Vale per OGNI lancio, non solo blast. (v1: costante ragionevole,
   si tara in sandbox.)

L'host (`host_blast` in `vat_horde`) aggiunge la fiction, nell'ordine:
5. **prop**: risposta per archetipo (§4) — burst forzati, incendi, scorch;
6. **FX**: flash + fireball + colonna di fumo (preset §6), shake camera
   (se/quando ci sarà);
7. **decal bruciatura**: disco scuro persistente al centro (pool §6) — anche
   sul punto dove c'erano i cadaveri vaporizzati resta il segno.

`E`/RMB del sandbox e `VAT_HORDE_BLAST` migrano da `simp_apply_impulse_ex`
nudo a `host_blast`: il banco visivo è gratis.

## 4. Risposta per archetipo

| Oggetto | Reazione | Meccanismo |
|---|---|---|
| Zombie | vola + gib/ferite + danno da caduta | esistente (§3.1+3.4) |
| Cadavere | **vaporizzato**, resta la bruciatura | `simp_corpse_clear` + decal |
| Muro/barricata (hp finiti) | danno al pool → crollo+reroute (+detriti) | `def_blast` §3.2 |
| Torretta | idem (struttura 1-cella) → silenziata | `def_blast` §3.2 |
| Prop assediabile (fence…) | idem; il render già scurisce/spegne | `def_blast` §3.2 |
| Prop one-shot (table, trafsign…) | burst immediato con direzione dal centro (topple saltato: l'onda non fa cadere lentamente) | `destruct_force` (API nuova §5) |
| Decor inerte (bench, bin, crate…) | **sparisce** con burst detriti (stile dal materiale) se `D(d) > resist` | `destruct_force` su prop non-destructible = burst generico |
| Prop indistruttibile (bus, building) | resta; **scorch procedurale** lato blast (§7) + eventuale incendio | stato scorch per-prop + re-stamp mesh |
| Statics (palazzi fusi nel livello) | v1: niente (mesh unica bakata) — annotato §9 | — |
| Draggable (auto, cassonetto) | shovato via (già così) | esistente |
| Prop `burn` (catalogo) | si incendia: fuoco+fumo per `burn_s` secondi, poi risposta base | `fx_start_emitter` ×2 con durata |

## 5. Catalogo: le due colonne nuove

Dopo `mass` (ENTITY_DESIGN §6, stesso pattern retro-compatibile):

    # key   … mass  [resist] [burn]
    bench   … -     -        -        # decor: sparisce a qualunque blast
    crate   … -     -        4        # brucia 4 s, poi sparisce
    bus     … -     -        6        # indistruttibile ma si incendia
    hydrant … -     80       -        # resiste sotto D=80 (mina piccola no, mortaio sì)

- `resist` = soglia di danno: `D(d) <= resist` → il prop ignora il blast
  (default 0: il decor leggero salta sempre). Per i prop CON hp la soglia non
  serve: c'è già il pool (resist si ignora, "-").
- `burn` = secondi d'incendio (default 0): allo scoppio parte una coppia di
  emitter continui fuoco+fumo ancorati al prop. Un prop che brucia E ha una
  risposta distruttiva la esegue *a fine incendio* (il tavolino brucia, POI
  scoppia in tizzoni); un indistruttibile brucia e basta, e resta lo scorch.
- Niente colonna "reazione": deriva dagli assi che la riga HA già
  (destructible → burst col suo debris; hp → danno; hp inf → scorch). Meno
  enum, più manopole — coerente con ENTITY_DESIGN §1.

**Fuoco → paura (DECISA, §10.1)**: un prop in fiamme timbra
`simp_add_danger` a peso basso sulle celle vicine per la durata
dell'incendio (stesso campo del sangue, decade da solo): l'orda scansa gli
incendi, il giocatore usa i bidoni come area denial temporanea. Zero API
nuove.

## 6. FX e decal (fiction, niente test)

Preset `FxEmitterDef` nuovi (data-only, stile BLOOD/SPARK esistenti):
- `EXPL_FLASH_DEF` — burst additivo bianco-giallo, vita brevissima, scala ↑;
- `EXPL_FIREBALL_DEF` — burst arancio→rosso scuro, drag alto, alpha;
- `EXPL_SMOKE_DEF` — burst lento grigio→trasparente, vento sì, vita lunga;
- `FIRE_LOOP_DEF` + `SMOKE_LOOP_DEF` — per `fx_start_emitter` (incendi §5,
  rate basso, ground_stop off, salgono).

Decal bruciatura: pool dedicato piccolo (64, ring buffer come i
corpse-decal), disco scuro con bordo irregolare via shader (riuso del
pattern `decal.vs/fs` con colore nero-marrone). Persistente per la partita.

## 7. Danno procedurale agli edifici (il building annerito)

Due stadi, il primo subito:

- **v1 — scorch direzionale**: per ogni prop indistruttibile colpito si
  accumula uno stato `(dir_blast, intensità 0..1)` per-prop (host-side,
  accanto a `gPropW`). `build_prop_mesh` già ri-stampa i vertici del glb a
  ogni cambiamento: allo stamp, i vertici la cui normale guarda il blast
  (`dot(n, −dir) > 0`) e sotto l'altezza della fireball (~R/2) vengono
  scuriti proporzionalmente. Risultato: la facciata investita è annerita, il
  retro intatto — ZERO asset nuovi, costa un dot per vertice al re-upload
  (evento raro).
- **v2 — convenzione nodi negli asset** (da fissare ORA per chi modella,
  attiva quando serviranno gli stati di danno "veri"): nel glb del prop i
  nodi con prefisso `glass_*` sono le parti fragili (finestre): un blast nel
  raggio li RIMUOVE dallo stamp (vetri infranti, si può accompagnare con un
  burst di schegge); i nodi `dmg_*` sono la variante danneggiata: nascosti
  da integri, mostrati al posto del nodo omonimo senza prefisso quando il
  prop è danneggiato. Stesso principio dei nodi `base`/`gun` delle torrette:
  convenzione di NOME, zero formati nuovi.

## 8. API nuove (tutte game-side)

| API | Dove | Note |
|---|---|---|
| `def_blast(g, x,y, r, dmg, strength, up)` | defense.c | §3; emette gli eventi esistenti |
| `def_damage_struct(g, id, dmg)` | defense.c | mattone di def_blast, utile anche a fase F |
| `destruct_force(d, i, dir)` | destruct.c | burst programmatico (salta il topple); su prop non-destructible = burst generico detriti |
| drain `simp_landed` → danno caduta | vat_horde (fixed step) | chiude M3.2; costante `k_fall` da tarare |
| preset FX + pool decal scorch | vat_horde | §6, solo fiction |
| stato scorch per-prop + darkening allo stamp | vat_horde | §7 v1 |

Core sim: **zero API nuove** (impulso, corpse_clear, danger, drag già bastano).

## 9. Piano di verifica

- `test_blast.c` (headless, deterministico, niente FX): (1) agenti — morti
  entro il raggio col falloff atteso, feriti oltre, lanciati in volo e danno
  da caduta all'atterraggio; (2) strutture — barricata nel raggio perde
  `D(d_min)` HP, due blast la crollano e l'orda reroute (riuso del pattern
  test_siege); torretta silenziata; (3) cadaveri — pila spazzata,
  `corpse_mass/danger` azzerati; (4) prop — one-shot burstati, decor sparito,
  `resist` alto sopravvive, assediabile danneggiato, indistruttibile intatto;
  (5) determinismo bit-identico su due run.
- Banco visivo: `props_demo.scn` + tasto E — bus annerito lato blast,
  cancellate crollate, cratere-decal, incendio su un prop `burn`.

## 10. Decisioni — TUTTE SCIOLTE (2026-07-04, input utente)

1. **Fuoco → danger: SÌ**, a peso basso — gli incendi timbrano
   `simp_add_danger` per la loro durata (area denial temporanea, zero API).
2. **Propagazione del fuoco: NO in v1** — catena di incendi = bilanciamento
   difficile; il `burn` è scenografia + denial, non un sistema.
3. **Statics: immuni in v1** — i palazzi fusi nel livello non si
   anneriscono; se il level design li espone spesso alle mine, il darkening
   §7-v1 si estende al glb statics (stesso trucco, mesh più grossa).
4. **Numeri di partenza accettati come default da tarare in sandbox**:
   mina `R=6 D0=250` (uccide walker al centro, ferisce al bordo), mortaio
   `R=4 D0=150`, granata/RMB sandbox `R=8 D0=180`. La taratura vera è
   compito delle fasi D/F.
5. **Scorch PERMANENTE** — memoria visiva della battaglia, come i decal di
   sangue.

# FX_LAB — laboratorio effetti visivi

> **STATO (2026-06-24):** PIANO concordato, non ancora implementato. Si parte
> dalla prossima sessione, dallo scheletro (step 0). Questo doc fissa scopo,
> principi e ordine degli step così non si perde il filo.

## Scopo

Un eseguibile **separato da `vat_horde`** per studiare e testare gli effetti
visivi **uno alla volta**, **senza la simulazione fisica**: niente orda, niente
director, niente nav. Solo un agente *scriptato* su cui si innescano a comando gli
effetti (cammina, ferito, mutilato, gibbato, morto, decal, accumulo…). Quando un
effetto **soddisfa a occhio**, migra in `vat_horde` e si chiude.

Perché: iterazione veloce e deterministica (premi un tasto → ferisci/uccidi
*quel* corpo *lì*, invece di aspettare il director), e scenari **scriptabili
headless** (screenshot da env, come `VAT_HORDE_SHOT`) così anche Claude può
giudicare i frame da solo.

## Principi (NON negoziabili)

1. **Condividere il codice, non forkarlo.** Il lab fa girare gli **stessi
   moduli** del gioco — `vat_layer.c`, `vat_gl.h`, gli shader, i pool decal/gib,
   il bake atlante sagome (CORPSE_DESIGN §10.7) — solo con un **driver diverso**
   (un agente scriptato al posto della sim). "Integrare" deve voler dire *accendere
   un flag in `vat_horde`*, non riscrivere l'effetto. Il rischio numero uno di una
   sandbox separata è la divergenza: il lab bello, il gioco diverso. Evitarlo per
   costruzione.
2. **Scope = RENDER.** Niente pathfinding/Dijkstra nel lab. Il confronto peso
   nav-cadaveri vs barricata resta dove già vive: `test_jam`, `test_breakthrough`,
   `test_corpse_pile` (core, headless) + `vat_horde` (in azione). Nel lab
   l'accumulo si testa come **visivo**, guidato da un `corpse_height` **dipinto a
   mano** (niente agenti che muoiono).
3. **Hot-load degli asset.** I modelli li fa l'utente: il lab carica gli asset VAT
   che gli si puntano, con **scelta body + outfit da tastiera**, così si droppa un
   modello nuovo e si vede subito hit/maim/gib/morte/decal su quello.
4. **Disciplina "uno alla volta + graduazione".** Ogni step si chiude migrando in
   `vat_horde`. Il lab resta un banco di prova, non un secondo gioco.

## Build / convenzioni

- Target `make fxlab` (nome provvisorio), un nuovo `.c` che riusa i moduli `vat/`.
  Fuori dai target di default.
- Controlli a tasti (bozza): `H` hit · `M` maim/mutilazione · `O` cambio outfit ·
  `G` gib/schizzi · `K` morte · `F` freeze→decal · `B` bake in texture cadaveri ·
  `N` toggle normal/POM · `[`/`]` body/outfit · camera con mouse (come vat_horde).
- Headless: env stile `FXLAB_SHOT="<frame>[,...]"` → N frame, screenshot, esci.

## Ordine degli step (dipendenze)

0. **Scheletro.** Livello vuoto + un VAT che cammina + selezione body/outfit.
   Tutto il resto ci si appende sopra.
1. **Ferimento.** Animazione `hit` + cambio outfit (insanguinato) **oppure**
   mutilazione (arto monco / crawler). Riusa la FSM di `vat_layer` e gli outfit
   atlas.
2. **Gib / schizzi.** Burst gore (pool gib esistente) o particelle di sangue;
   decal-macchia a terra.
3. **Morte → cadavere → decal.** Freeze dell'ultimo frame (decedente) → a TTL la
   sagoma-cadavere (CORPSE_DESIGN §10.7, già fatta) → **bake del pool in una
   texture "cadaveri sul suolo"** world-aligned (la RTT-accumulation rinviata in
   §10.7: il lab è il posto giusto per prototiparla in sicurezza).
4. **Normal map + POM.** Esperimenti di rilievo di superficie sotto la nostra
   camera (azimuth ruotante, elevazione fissa): vedere l'effetto, decidere se vale
   per cadaveri/terreno/mound (vedi nota POM in CORPSE_DESIGN).
5. **Accumulo cadaveri (ibrido).** Heightfield bulk da `corpse_height` (griglia
   mondo fissa, stabile→niente flicker) + pochi cadaveri "hero" persistenti su
   ancore a seed fisso. Guidato da un campo dipinto a mano. Decisione di design
   in CORPSE_DESIGN §10 (cupola procedurale SCARTATA: flicker strutturale).

## Vedi anche

- `CORPSE_DESIGN.md` §10 — rendering cadaveri (singoli/decal/mucchi), §10.7
  sagoma-cadavere + nota RTT-accumulation, decisione heightfield+hero.
- `GFX_DESIGN.md` — direzione artistica (realistico finale; placeholder ora).

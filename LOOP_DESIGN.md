# LOOP_DESIGN — salvare il loop d'assalto (diagnosi + proposte)

**STATO: DECISO (2026-07-15), in implementazione — D FATTA.** Nato dal primo
playtest di livello col bilanciamento nuovo: assalto giudicato noioso
(l'utente aspetta che le torrette generino biomassa per il mortaio, il
mortaio genera altra biomassa, e colpire un blob di zombie non è avvincente).
Diagnosi + pacchetto di proposte ORDINATE, discusse e decise con l'utente
(§4). SOLDIER_DESIGN.md è la proposta F.

## 1. Diagnosi: quattro cause radice

1. **Manca il circolo virtuoso dei TD.** Nei tower defense la tensione è:
   *la minaccia cresce → la mia difesa attuale non basterà → devo convertire
   i kill in potere PRIMA della prossima ondata*. Da noi mancano ENTRAMBE le
   metà: la minaccia non cresce in modo percepibile durante l'assalto (il
   director ha una rampa, ma non è LEGGIBILE come escalation), e il potere
   del giocatore non può crescere (piazzamento chiuso fuori PREP, upgrade
   solo al debrief). Restano i verbi di manutenzione (ripara/ricarica/
   regola), che sono decisioni piccole.

2. **Cecità informativa.** PREP alla cieca (il giocatore non sa da dove
   arriva l'orda), nessun annuncio di cosa sta per arrivare, e — il peggio —
   il comportamento più sofisticato del motore (reroute su sangue/ingorghi/
   costi, l'asta d'assedio sul muro più economico) è INVISIBILE. Senza
   informazione non c'è pianificazione; senza pianificazione non c'è
   strategia, solo reazione.

3. **Il mortaio è una slot machine truccata.** Bersaglio banale (un blob
   lento e enorme), feedback positivo (i kill ripagano il colpo), nessuna
   ragione di mira. Qualunque azione dominante e senza skill divora il
   resto del gameplay.

4. **Lo sweet spot dei tre verbi è stretto** (già diagnosticato in
   SOLDIER_DESIGN): perché ripara/ricarica/regola diventino *necessari*
   serve un equilibrio fine della tabella numeri; fuori equilibrio il
   giocatore guarda.

**L'asset non sfruttato.** L'orda non è una fila di creep su un binario:
è un FLUSSO adattivo che si riprezza le rotte in tempo reale (densità, jam,
sangue→paura, asta sul muro più debole, lure dal fuoco). Nessun TD ce l'ha.
Il gameplay distintivo di questo gioco è il **duello giocatore-vs-flusso**:
io lo incanalo, lui trova la mia debolezza. Oggi quel duello non si vede e
non si gioca. Le proposte B, D, E servono a metterlo al centro.

## 2. Proposte

### A. Ondate a escalation + annunci (la curva di minaccia)

Struttura d'assalto a ONDATE dichiarate invece del rubinetto continuo:

- Ogni ondata = (composizione, direzione/exit, intensità), con escalation
  scriptata per livello: walker → +runner → +tank → +screamer, rate su.
- **Annuncio**: banner + indicatore di direzione ("ONDATA 3 — NORD-EST —
  runner + 2 tank — 15 s"). L'anticipazione È la tensione TD.
- **Pausa fra ondate** (10–20 s): il momento decisionale in cui la biomassa
  si spende (ripara, ricarica, costruisci — proposta C). L'attesa passiva
  di oggi diventa una finestra di scelte sotto pressione di tempo.
- Implementazione: estensione del director (lista di fasi per exit:
  `delay, count, mix` — il campo `pool`/`start_delay` di mission §A è già
  metà del lavoro), formato `.scn` `wave …` per exit, evento → HUD.

### B. Intelligence: le rotte previste (l'occhio sul flusso)

- **Marker delle exit** sempre visibili (sono già nel `.scn`).
- **Overlay delle rotte**: streamline dal centro di ogni exit alla LZ
  integrando il flow field già calcolato dal core (~centinaia di passi di
  campionamento bilineare → polyline; roba da poche decine di righe
  host-side, zero API core se il flow è leggibile, altrimenti un getter).
- **Live in PREP**: piazzi un muro → `nav_dirty` → la rotta si sposta
  davanti agli occhi. PREP smette di essere alla cieca e diventa il puzzle
  strategico: *guardo dove passeranno, sbarro, guardo dove passano adesso,
  decido dove costruire il killzone*. È il gameplay di Creeper World
  applicato al nostro motore, quasi gratis.
- **In assalto** (a toggle, o sempre): rende visibile il reroute da
  sangue/jam — il giocatore VEDE l'orda cambiare idea e capisce perché il
  suo killbox ha smesso di funzionare. Il comportamento emergente più
  costoso del progetto diventa finalmente gameplay leggibile.

### C. Costruire in assalto a biomassa (il circolo virtuoso, metà giocatore)

GAME_PLAN "Questione aperta 1": `place.c` funziona già a runtime, è una
policy di `mission.c`. Proposta:

- In ASSAULT si piazza un SOTTOINSIEME del catalogo (barricate, torretta
  leggera, mine — le cose "da campo"; le pesanti restano da PREP), pagato
  in **biomassa** (colonna `bio_cost` in balance.cfg), più caro
  dell'equivalente in budget PREP.
- Chiude il circolo TD: kill → biomassa → più difese → regge l'ondata
  dopo. E dà alla biomassa un pozzo STRATEGICO (oggi solo manutenzione).
- Allarga lo sweet spot di bilanciamento con lo stesso argomento del
  soldato (l'agency del giocatore assorbe gli errori della tabella) ma
  SENZA spostare il genere.
- Costo-opportunità già pronto: ogni torretta comprata in corsa è biomassa
  che non diventa upgrade al debrief (§7 BIOMASS_DESIGN).

### D. Mortaio: da farm a strumento di rotta

Tre viti, tutte piccole:

1. **Resa ridotta sui kill da mortaio**: manopola `bio.mortar_yield`
   (default proposto 0.25, anche 0 è difendibile). Fiction perfetta:
   l'esplosione POLVERIZZA la biomassa. Spegne il feedback positivo.
2. **Il cratere timbra il terreno**: `simp_add_danger` forte (+ eventuale
   `cost_user` temporaneo) al punto d'impatto → il mortaio diventa lo
   strumento per CHIUDERE una strada per ~30 s e deviare l'orda sulla
   rotta fortificata. Con l'overlay B l'effetto si VEDE. La mira torna a
   contare: non "dove è più fitto" ma "quale strada nego".
3. Cooldown: già presente, resta.

Il mortaio smette di essere la slot machine e diventa il verbo del duello
col flusso.

### E. Nemici speciali che chiedono attenzione (la curva, metà nemico)

Ognuno una ragione per guardare l'ORDA invece della barra:

- **Screamer**: emette un lure negativo mobile (pattern `def_set_fire_lure`
  al contrario, API `simp_add_cost` w<0 già nel core) → trascina un pezzo
  d'orda verso il fianco che hai lasciato debole. DEVE morire in fretta →
  nasce il bersaglio prioritario (click sullo screamer = le torrette in
  range lo prioritizzano; o colpo di mortaio su bersaglio PICCOLO e in
  movimento — finalmente un tiro che richiede skill).
- **Tank**: c'è già (sfonda-barricate, massa 10). Va solo messo in scena
  dalle ondate A.
- **Obeso**: alla morte scoppia → sangue/danger extra proprio nel tuo
  killzone (auto-sabotaggio accelerato, contro-gioco al camping).

Da introdurre UNO per volta, screamer per primo (è quello che crea
decisioni spaziali).

### F. Soldato giocabile (SOLDIER_DESIGN.md) — valutazione

- **Pro**: la rete di sicurezza più forte sul divertimento (la skill del
  giocatore colma i buchi di bilanciamento), prototipo da un pomeriggio,
  l'orda fisica che ti shova è spettacolare e nessun twin-stick ce l'ha.
- **Contro**: sposta il genere (decisione 7 del doc). Se diventa il
  pilastro, A–E diventano contorno.
- **Raccomandazione**: prototiparlo DOPO il playtest di A–D (o in
  parallelo, è indipendente), come modalità/opzione. Se A–D bastano, resta
  un'arma in più; se non bastano, è pronto a salire di grado. Le
  raccomandazioni tecniche del doc (HP+knockback, agente ad alta massa,
  aggro locale solo se il drift è molle) restano valide.

### Parcheggiate (con motivo)

- **Squadra di droni**: via di mezzo costosa (unit AI + UI di squadra) che
  fa peggio del soldato sul feel e peggio delle torrette sulla strategia.
  Riaprire solo se il soldato piace ma il contatto diretto è troppo letale.
- **Ruoli invertiti (giocare l'orda)**: il motore ci mapperebbe benissimo
  (dipingere costi/lure = dirigere il fiume, Creeper World al contrario),
  ma è UN ALTRO GIOCO. Ottima modalità bonus futura, non il salvataggio di
  questo. Da riesumare a gioco finito.
- **Rete di potenza** (torrette alimentate da relay collegati alla LZ,
  l'orda può tagliare la rete): idea grossa, molto Creeper World, che
  creerebbe strategia spaziale profonda. Candidata v2 se dopo A–E manca
  ancora profondità; troppo carico adesso.

## 3. Ordine raccomandato

1. **D** — mezza giornata, ferma subito l'exploit del mortaio. **FATTA
   (2026-07-15)**: `mortar.bio_yield`/`fear`/`fear_radius` in balance,
   `gBioYieldMul` + timbro danger al cratere in vat_horde.
2. **B** — 1 sessione, trasforma PREP e rende leggibile il motore.
3. **C** — 1 sessione, chiude il circolo TD (policy + prezzi).
4. **A** — 1–2 sessioni, la curva di minaccia (director a ondate + HUD).
5. **Playtest** → decidere E (screamer primo) ed F (soldato).

D+B+C+A insieme sono la risposta diretta alla diagnosi: minaccia che cresce
(A+E), potere che cresce (C), informazione per decidere (B), niente azione
dominante (D). Tutto dentro l'identità TD, quasi tutto su mattoni esistenti.

## 4. Decisioni PRESE (utente, 2026-07-15)

1. **Resa mortaio: 0.1**, manopola `mortar.bio_yield` in balance.cfg (un
   colpo oggi può fruttare 50+ bio — a 0.1 ne rende ~5). **FATTA**: resa
   scalata solo sui kill dentro la host_blast del colpo (i lanciati che
   muoiono d'atterraggio step dopo pagano piena — leak minore annotato);
   cratere → `simp_add_danger` (`mortar.fear` 16 = 2× saturazione ≈ strada
   negata ~30 s, `mortar.fear_radius` 8).
2. **Catalogo d'assalto a SBLOCCO PROGRESSIVO**: si parte con SOLO torretta
   leggera + barricata (il muro più debole); le altre voci si sbloccano come
   upgrade (pannello debrief §7 BIOMASS_DESIGN, quando ci sarà). Sovrapprezzo
   **+50%** sul prezzo PREP. **Valuta: BIOMASSA diretta** (raccomandazione
   accettata): budget $ e biomassa restano non convertibili (principio
   BIOMASS_DESIGN §1) — il $ è la dote di missione, la biomassa è l'unica
   valuta GUADAGNABILE in assalto, e il circolo TD (kill → potere) chiude
   solo se si costruisce con quella. Prezzo bio = ceil(1.5 × costo $),
   leggendo 1 $ ≈ 1 bio (scala già coerente: light 100 $ → 150 bio, con
   serbatoio 500 e resa ~1/kill è una decisione vera).
3. **Pausa fra ondate: "chiama la prossima"** con bonus biomassa
   proporzionale al tempo risparmiato (alla Kingdom Rush), entro un tempo
   massimo di preparazione.
4. **Overlay rotte: solo PREP.** In assalto niente overlay: il giocatore
   VEDE dove vanno gli zombie (sono migliaia, sono loro l'overlay).
5. **Soldato: dopo** il playtest di A–D.
6. **Bersaglio prioritario: click sul nemico**, con lo screamer reso BEN
   visibile — es. lampeggiante (glow/pulse sul render, non una barra).

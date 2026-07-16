# Soldato giocabile — v1 implementata, in lavorazione

**Stato: v1 IMPLEMENTATA e committata (2026-07-16), lavoro in corso.**
Il playtest delle ondate annunciate (LOOP_DESIGN A) è passato; il soldato è
la F del pacchetto. **PROSSIMA FEATURE (dal primo playtest): lo
SCAVALCAMENTO dei muri.** Oggi il soldato è correttamente bloccato dalla
collisione SDF come tutti — ma così resta confinato dentro la cinta della
base: deve poter scavalcare (vault sui muri bassi del giocatore, wall_h
2 m — piste: hop balistico host-side sopra la cella muro quando preme
contro, o uno stato "flying" per i draggable sul modello di SIMP_FLYING).

Com'è fatto: modulo game-side `soldier.h/.c` (zero-dep sul core, verificato da
`test_soldier`: drive/knockback, mitra con occlusione/trasmittanza, contatto→
HP→DOWN→lockout, lure mobile bit-esatto alla rimozione, determinismo); unica
aggiunta core `simp_drag_set_vel`; manopole `soldier.*` in `assets/balance.cfg`
(HP 120, speed 4.2, mitra 35 HP/0.10 s/16 m gratis, granata 30 bio,
lockout 8 s). Host cablato in `vat_horde` (GAME_SHELL, solo ASSALTO):
card **SOLDATO / tasto F** nella barra dei verbi (esclusiva con
mortaio/ripara/regola/costruisci; in lockout la card conta i secondi), deploy
sul primo punto libero ad anelli attorno alla base, **WASD** relativo allo
schermo, mouse mira, **LMB** mitra (tracer + `def_damage_agent`, resa bio
`soldier.bio_yield`), **RMB** granata (una in aria, arco fiction come il
mortaio → `host_blast`), **camera follow** esponenziale (il pan manuale vince
durante il drag), barra HP world-space transitoria (verde→rossa sotto 40%),
al down burst di sangue + rientro in modalità RTS. Render placeholder: sagoma
in piedi verde militare sul disco draggable (`build_drag_mesh`).

Decisioni prese con l'utente (2026-07-16):

1. **Contatto = HP + knockback** (lo shove del PBD arriva gratis dal corpo
   fisico; morte secca scartata come ingiusta con un'orda fisica).
2. **Camera follow** sul soldato in modalità, ritorno al pan RTS all'uscita.
3. **Modalità esclusiva**: card/hotkey entra-esce; dentro WASD muove, mouse
   mira, LMB mitra, RMB granata. Mortaio/ripara/regola restano fuori modalità.
4. **Economia: mitra gratis, granata a biomassa** (costo in balance.cfg).
   I kill del soldato fruttano bio a resa piena (`soldier.bio_yield` 1.0,
   tarabile come `mortar.bio_yield` se il farm degenera).
5. **Corpo = DRAGGABLE, non agente** — deviazione MOTIVATA dalla
   raccomandazione "agente ad alta massa" della decisione 3 storica (sotto):
   un agente vero viene BERSAGLIATO dalle torrette, DRENATO dalle celle goal
   (la LZ lo ucciderebbe al primo passaggio) e CONTATO dalla missione (CLEAR
   non scatterebbe mai). Il draggable è già il ghost giusto: invisibile a
   query/impulsi/nav/missione, collide con orda e muri, viene shovato con
   momento reale (knockback gratis), frenato da drag_damp. Unica aggiunta
   core: `simp_drag_set_vel` (setter di velocità); il drive è un blend
   host-side (accelera verso la velocità voluta, MAI sovrascrivere secca:
   sovrascrivere cancellerebbe lo shove appena ricevuto).
6. **Aggro locale (decisione 1 storica) RIMANDATO al playtest**: si parte
   col lure a costo-nav (drift morbido); se risulta troppo molle si valuta
   l'override di steering nel core.

Il resto del doc è il materiale originale (2026-07-15), valido come contesto.

## L'idea

Aggiungere la possibilità per il giocatore di **controllare direttamente un
soldato** che si muove liberamente per il livello e attacca gli zombie con
**mitra** e **granate**, stile Vampire Survivors / twin-stick survivor. Le
difese passive (torrette, muri, mortaio) diventano **supporto** al gameplay
attivo invece dell'elemento principale.

Regole di base proposte:
- Il soldato è un **punto d'attrazione per l'orda** entro un certo raggio: gli
  zombie vicini lo puntano.
- Se **toccato** dagli zombie → conseguenza negativa (game over secco *oppure*
  HP + spinta — vedi decisioni).

## Contesto / perché (il problema che risolve)

Al momento, piazzate le difese, al giocatore restano tre verbi in assalto:
**MORTAIO** (spara), **RIPARA** (struttura→HP), **REGOLA** (facing torrette).
Non è poco e può nascere un gameplay interessante — **ma solo se si trova lo
sweet spot** in cui queste azioni sono davvero *necessarie* per sopravvivere.
Fuori da quell'equilibrio il giocatore può stare a guardare senza fare nulla.

Rendere gli zombie più forti spinge verso la necessità di quelle azioni, ma lo
sweet spot è stretto. Il soldato giocabile è la **rete di sicurezza**: sposta
parte del carico di bilanciamento dalla tabella dei numeri alla skill del
giocatore (molto più tollerante). Se le difese non bastano, l'agency del
giocatore colma il buco; se sono troppo forti, il soldato è opzionale.

## Fattibilità: i pezzi esistono già

Quasi tutto ciò che serve esiste nel core in altre vesti:

- **Soldato = attrazione per l'orda** → è esattamente `def_set_fire_lure`: cono
  di `cost_user` negativo attorno a una posizione, rimosso/riapplicato quando si
  sposta. C'è già la meccanica *e* la trappola (clamp a −0.8 → leggere il delta
  di ritorno da `simp_user_cost`, mai sottrarre alla cieca). Da attaccare a una
  posizione mobile.
- **Mitra** → torretta mobile: `simp_query_nearest` / `simp_query_circle` + kill
  a rateo. Logica già in `defense.c`.
- **Granata** → `simp_apply_impulse` (già usato dalle esplosioni) + kill/danno
  nel raggio. La primitiva unificata `def_blast` (`EXPLOSION_DESIGN.md`) è ancora
  da implementare; per un prototipo bastano impulso + query.
- **Game over al contatto** → `simp_query_circle` attorno al soldato ogni step.
- **Perf** → un agente in più + una query: rumore.

Un prototipo giocabile è materia di un pomeriggio, tutto host-side in
`vat_horde`, senza toccare il core (salvo l'aggro locale — vedi decisioni).

## Decisioni da prendere

1. **Attrazione globale vs aggro locale.** Il lure a costo-nav fa *driftare*
   l'orda ma passa dal Dijkstra throttled (`flow_period` 0.5s, drenato su ~6
   step): risposta morbida e in ritardo. Per il feel "lo sciame ti si stringe
   addosso" serve che gli agenti entro raggio R sterzino **direttamente** verso
   il soldato bypassando il flow field → override di target locale nello
   steering = **l'unico vero cambiamento al core**. Decidere se serve subito o
   solo se il drift risulta troppo molle alla prova.

2. **Contatto: morte secca vs HP + spinta.** Instant-death con un'orda *fisica*
   (il PBD ti shova dentro i nemici con momento reale) è brutale e ingiusto:
   Vampire Survivors ha HP e i-frame apposta. Opzioni:
   - morte secca (semplice, punitivo);
   - **HP + knockback** (consigliato per prototipo);
   - morte solo sopra soglia di contatto *sostenuto* (riusa la logica del
     sensore d'assedio `simp_wall_pressure`).

3. **Soldato = agente nel sim vs posizione host-side.** Farlo **agente ad alta
   massa** (`simp_spawn_desc`) lo fa spingere fisicamente dall'orda (momento
   reale, tematicamente perfetto) senza ribaltarlo, gratis. Alternativa: solo
   posizione host + query, niente shove. Consigliato: agente ad alta massa.

4. **Camera.** Oggi pan RTS a tier discreti (32/16/8 px/m). Un avatar vuole
   camera che lo segue. Cambio reale ma contenuto. Decidere: follow, o pan
   libero con l'avatar dentro.

5. **Input.** WASD + mouse per mira? Twin-stick con gamepad? Il resto del gioco
   è mouse-driven (piazzamento, mortaio) → conflitto di schema di controllo da
   risolvere (modalità? il soldato "prende" il mouse?).

6. **Munizioni / economia.** Mitra e granate consumano biomassa? Munizioni
   raccolte dai kill? O illimitate? Va agganciato a `bio.h` o resta separato.

7. **Identità del gioco (la decisione di fondo, non tecnica).** Questo **sposta
   il genere**: da TD fisico con orde massive verso twin-stick survivor con TD a
   contorno. Va bene *se è la direzione voluta*. Raccomandazione: tenere il
   soldato come **modalità/opzione**, non come pilastro, almeno finché non lo si
   prova — così non si compromette l'identità TD prima di sapere se è divertente.

## Piano prototipo minimo (quando/se si parte)

Tutto host-side in `vat_horde`, per sentire se è divertente prima di toccare il
core:
1. Soldato come agente ad alta massa (`simp_spawn_desc`), movimento WASD.
2. Lure mobile riusando il pattern di `def_set_fire_lure`.
3. Mitra = query nearest + kill a rateo; granata = impulso + kill in raggio.
4. Contatto → HP + knockback (non morte secca) per la prima prova.
5. Aggro locale (decisione 1) SOLO se il drift da flow field è troppo molle.

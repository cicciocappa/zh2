# Soldato giocabile — idea parcheggiata

**Stato: IDEA, non implementata (2026-07-15).** Da sviluppare SOLO se il
gameplay con le sole difese passive non risulta interessante e/o impossibile
da bilanciare. Vedi "Contesto / perché" sotto.

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

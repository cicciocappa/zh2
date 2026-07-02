# GAME_APP_DESIGN — l'eseguibile del gioco (shell applicativa)

Design della SHELL che trasforma `vat_horde` da demo tecnica a GIOCO:
title screen → menu (new game / continue / settings / exit) → briefing del
livello → PREP (l'elicottero atterra, il giocatore fortifica) → ASSAULT →
debrief (vittoria = livello successivo, sconfitta = riprova) → campagna.
Richiesto dall'utente il 2 lug 2026. Compagno di `GAME_PLAN.md` (le fasi
A–H riempiono di sostanza le fasi di gioco che questa shell inquadra) e di
`EDITOR_PLAN.md`. Tutto con PLACEHOLDER: niente arte, niente asset audio.

## 0. Decisioni chiave

1. **L'eseguibile `game` È `vat_horde.c` compilato con `-DGAME_SHELL`**
   (target `make game`). Niente fork del monolite: un solo host visivo
   (GAME_PLAN §1 punto 3), la shell è un layer additivo dietro `#ifdef`.
   `make vat_horde` resta il dev tool identico a prima (parte dritto in
   PLAY, ESC esce, nessun menu). Se in fase G la UI nuklear chiederà uno
   split vero, la logica sarà già tutta nei moduli sotto.
2. **La logica di flusso è PURA e headless** (`app.h/app.c`, zero SDL):
   macchina a stati che riceve input astratti (`APP_IN_*`) e restituisce
   AZIONI che l'host esegue (`APP_ACT_LOAD_LEVEL`, `START_ASSAULT`,
   `QUIT`…). Testata da `test_app.c` in `make test`, come ogni modulo.
3. **Campagna data-driven** (`campaign.txt`): lista ordinata di livelli
   (scena `.scn`, nome, testo di briefing, condizioni). La v1 usa le
   mappe-banco `arena_*` come livelli placeholder.
4. **Audio = miniaudio** (single-header, C puro — coerente con l'etica
   librerie leggere). Il download lo fa L'UTENTE (`vat/miniaudio.h`); il
   Makefile lo rileva con `wildcard` e definisce `HAVE_MINIAUDIO`. Senza
   header il backend è NULLO (il gioco gira muto, zero errori). Gli SFX
   v1 sono PROCEDURALI (PCM generati a init: blip menu, esplosione,
   stinger win/lose) — zero file `.wav` da procurare.
5. **Animazioni "meccaniche"** (`anim.h/anim.c`): pool di envelope
   one-shot chiavati `(kind, id)` — fire → valore 1→0 in `dur` secondi.
   v1: rinculo torrette (`t->fired` → nudge del pilastrino contro la
   direzione di tiro). NON sostituisce niente di esistente: VAT = agenti,
   `fx_particles` = particelle, gib/decal = gore; anim è per i meccanismi
   (torrette, e in futuro porte, elicottero, radar…).

## 1. Stati e flusso (app.c)

```
TITLE ──confirm──▶ MENU ──new game──────▶ BRIEFING(0)
                    │ ▲──back(ESC)◀──┐      │ (host carica il livello,
                    │ continue──▶ BRIEFING(unlocked)  app_level_ready)
                    │ settings─▶ SETTINGS   │ confirm (a caricamento fatto)
                    │ exit────▶ QUIT        ▼
                    │                     PREP ──confirm──▶ ASSAULT
                    │◀────back────────────┘  ▲               │
                    │◀────back───────────────┼───────────────┤
                    ▼                        │        app_report_result
                  (world resta caricato)   BRIEFING ◀─confirm─ DEBRIEF
                                            (won: cur+1 | lost: stesso cur)
```

- **TITLE**: qualsiasi tasto = confirm → MENU.
- **MENU**: 4 voci — `NEW GAME` (cur=0), `CONTINUE` (cur=unlocked,
  disabilitata se unlocked==0), `SETTINGS`, `EXIT`. Selezione con ↑/↓,
  wrap; le voci disabilitate si saltano.
- **SETTINGS**: volumi SFX/MUSIC 0..10 con ←/→ (→ `APP_ACT_APPLY_
  SETTINGS`), voce BACK / tasto back → MENU + `APP_ACT_SAVE`.
  Fullscreen resta su F11 (host). v1 non ha altro.
- **BRIEFING**: mostra nome livello + testo; l'host esegue
  `APP_ACT_LOAD_LEVEL` (v1: caricamento SINCRONO al primo frame — il
  punto d'aggancio per il precaricamento vero/audio narrato/video è
  QUESTO stato, annotato in §6) poi chiama `app_level_ready`. Confirm
  è ignorato finché non è ready ("CARICAMENTO…" → "INVIO PER INIZIARE").
- **PREP**: la sim gira (mondo vivo) ma i director NON emettono; il
  giocatore piazza difese (`place.c`, tasto P, budget). Confirm → ASSAULT
  (GAME_PLAN fase A: `prep_s` 0 = prep illimitata chiusa dal giocatore;
  quando arriverà `mission.c` il timer opzionale entra qui).
- **ASSAULT**: director attivi, timer di sopravvivenza. WIN al timer
  (`survive <s>` del livello), LOSE se `def_lost` (core crollato).
  L'host chiama `app_report_result(won)` → DEBRIEF (+`APP_ACT_SAVE` se
  avanza il progresso).
- **DEBRIEF**: esito + confirm → BRIEFING del prossimo (won) o dello
  stesso (lost). Vinto l'ultimo livello → `campaign_done` → MENU.
- **BACK in PREP/ASSAULT** (ESC) → MENU: abbandona la missione (il mondo
  resta com'è dietro il menu, si rientra solo via new/continue = reload).

`mission.c` (GAME_PLAN fase A) NON è anticipato: la shell v1 tiene la
condizione win/lose minima (timer + `def_lost`) nell'host; quando fase A
arriva, PREP/ASSAULT/esiti diventano suoi e la shell si limita a
inoltrare (`app_report_result` resta il confine).

## 2. Campagna (`campaign.txt`) e progressi (`progress.txt`)

```
# campaign.txt — un blocco per livello, `level` apre il blocco
level scenes/arena4.scn
name  Perimetro
survive 90                      # WIN: sopravvivi N secondi (v1)
core 800                        # HP del core alla LZ (0 = nessun core)
brief La zona di atterraggio e' calda.
brief Fortifica il perimetro e resisti.
```
- `brief` ripetibile (una riga di testo ciascuna, concatenate).
- `core <hp>`: l'host costruisce un CORE `is_core` (anello di celle
  attorno al centroide dei goal) → `def_lost` = sconfitta. È il
  placeholder della LZ/elicottero di GAME_PLAN fase A (`lz x y`): quando
  l'entità `lz` esisterà nel formato scena, `core` sparisce da qui.
- Campi FUTURI già previsti dal parser (ignorati se assenti): `voice
  <file>` (narrazione audio del briefing), `video <file>` — non
  complichiamo ora, il formato non cambierà.
- `progress.txt` (gitignored): `unlocked N` + volumi. CONTINUE riparte
  dal livello `unlocked`. Salvato a ogni vittoria e all'uscita dai
  settings.

## 3. Moduli nuovi (tutti game-side, core intoccato)

| File | Cosa | Test |
|---|---|---|
| `app.h/.c` | macchina a stati flusso + campagna + progressi | `test_app.c` |
| `anim.h/.c` | pool envelope one-shot (kind,id), no malloc | `test_anim.c` |
| `audio.h/.c` | API `au_*`; backend nullo ↔ miniaudio | no (side-effect only, come gli FX) |
| `vat/font8.h` | font bitmap 5×7 (A-Z 0-9 punteggiatura), header-only | dump ASCII in `test_app` |
| `vat/ui.fs` | fragment unlit per overlay/testo (alpha in `aNormal.x`) | visiva |
| `campaign.txt` | campagna placeholder (3 livelli `arena_*`) | parse in `test_app` |

### app.h (API)
```c
AppState  app_state;  // TITLE MENU SETTINGS BRIEFING PREP ASSAULT DEBRIEF QUIT
void      app_init(App*);
int       app_campaign_load(App*, const char *path);
int       app_load_progress(App*, const char *path);   // file opzionale
int       app_save_progress(const App*, const char *path);
AppAction app_input(App*, AppInput in);   // l'host ESEGUE l'azione tornata
void      app_level_ready(App*);          // host: caricamento finito
AppAction app_report_result(App*, int won);
```

### anim.h (API)
```c
void  anim_init(AnimSys*);
void  anim_fire(AnimSys*, int kind, int id, float dur);  // (ri)parte 1→0
void  anim_update(AnimSys*, float dt);
float anim_value(const AnimSys*, int kind, int id);      // 0 se spento
```
Pool fisso (256 canali), pieno = rimpiazza il canale più vicino alla
fine. Lineare 1→0: la CURVA la dà l'host (rinculo = value², ecc.).

### audio.h (API)
```c
int  au_init(void);              // ok anche senza miniaudio (backend nullo)
void au_shutdown(void);
void au_play(AuSound id);        // SND_MENU_MOVE/SELECT, SND_ASSAULT,
void au_set_volume(float sfx, float music);   // SND_WIN, SND_LOSE, SND_BOOM
```
Backend miniaudio: `ma_device` playback f32/48k/stereo + mixer proprio
(voci one-shot su buffer PCM procedurali) — niente `ma_engine`, niente
file. La musica (vol salvato, non usato) arriva con i primi asset veri.
NOTA: il codice miniaudio è scritto "alla cieca" (l'header lo scarica
l'utente): alla prima build con `HAVE_MINIAUDIO` può servire un ritocco.

## 4. Integrazione nell'host (`vat_horde.c`, `#ifdef GAME_SHELL`)

- **Init**: `argv[1]` = campagna (default `campaign.txt`), non più la
  scena; scena iniziale = livello `unlocked` (mostrata dietro il TITLE).
  `au_init`, volumi dai progressi.
- **Input**: negli stati UI i tasti diventano `APP_IN_*` (frecce/WASD,
  INVIO/SPAZIO=confirm, ESC=back) e NON raggiungono i gestori esistenti;
  in PREP/ASSAULT passano tutti (camera, P, E…) tranne INVIO (confirm)
  ed ESC (back→MENU invece di quit). TAB/editor resta disponibile anche
  in `game` (comodo per ritoccare i livelli; si toglierà alla release).
- **Cambio livello** (`APP_ACT_LOAD_LEVEL`): stesso percorso del reload
  editor — `scene_free`+`scene_load`, `free_world`+`build_world`,
  `destruct_init`, re-upload mesh ostacoli/prop — più: core della LZ da
  `core <hp>` (anello al centroide dei goal), camera ricentrata, timer
  azzerati, `app_level_ready`. Terreno/statici `.glb` restano quelli di
  startup (le `arena_*` non ne hanno): il reload per-livello del terreno
  è annotato in §6.
- **Gating sim**: la sim steppa solo in PREP/ASSAULT; `def_director_
  update` solo in ASSAULT. In ASSAULT: `survive_t += dt`; win/lose →
  `app_report_result` → `app_save_progress`.
- **Rinculo torrette**: dopo `def_update`, per ogni torretta con
  `fired` → `anim_fire(RECOIL, id, 0.12s)`; `build_turret_mesh` riceve
  l'AnimSys e trasla il pilastrino di `0.18·v²` m contro la direzione di
  tiro. Attivo anche nel dev tool `vat_horde` (stessa build, no #ifdef).
- **Overlay UI**: dopo il render 3D, negli stati UI: quad scuro
  semi-trasparente fullscreen + testo `font8` (quad per pixel, VBO
  dinamico, `flat.vs`+`ui.fs`, orto in pixel, depth off). In PREP/ASSAULT
  solo una riga di stato in alto (fase, budget, countdown). Lingua:
  voci del menu in inglese (come richiesto), resto in italiano.

## 5. Verifica

- `test_app`: parse campagna (nomi/brief/survive/core), flusso completo
  title→menu→settings→new game→briefing(gate ready)→prep→assault→
  win→debrief→next / lose→retry, continue da progressi, roundtrip
  save/load, exit, campaign_done. Tutto a stati e azioni, deterministico.
- `test_anim`: fire/value/decay/scadenza, restart, indipendenza per id,
  rimpiazzo a pool pieno.
- Visiva (utente): `make game && ./game` — title/menu/briefing/partita
  su arena4, INVIO in prep scatena l'assalto, rinculo torrette, esito.

## 6. Rimandato deliberatamente (punti d'aggancio pronti)

1. **Preload asincrono nel briefing** + barra di avanzamento vera: oggi
   il load è sincrono (<1 s). Quando i livelli avranno terreni `.glb`
   pesanti: caricamento a step nel frame loop dello stato BRIEFING
   (l'host già distingue ready/not-ready).
2. **Terreno/statici per-livello**: il reload dei `.glb` al cambio
   scena (oggi startup-only). Serve insieme ai livelli veri di fase H.
3. **Narrazione audio (`voice`) e video del briefing**: campi già nel
   formato; l'audio arriva col primo asset registrato, il video è
   fuori scopo per ora (decisione utente 2 lug 2026).
4. **Musica** e SFX campionati (`.wav` via miniaudio): quando ci sono
   gli asset; l'API `au_*` non cambia.
5. **Pausa in partita** (oggi ESC = abbandono al menu; una PAUSA vera
   con resume è banale da aggiungere in app.c quando serve).
6. **mission.c** (GAME_PLAN fase A) assorbirà win/lose/prep-timer; la
   shell resta identica (il confine è `app_report_result`).

# Outfit zombie — pipeline texture a proiezione (piano d'azione)

> **STATO: DECISO, DA IMPLEMENTARE** (discussione 2026-07-10, sessione
> biomassa). Obiettivo: smettere di dipingere gli outfit A MANO nello spazio
> UV scomodo del modello e passare a un **bake di proiezione**: si collagia
> su una sagoma fronte/retro leggibile, uno script Blender headless proietta
> e baka nella UV vera del modello. La UV map "scomoda" smette di essere un
> problema di authoring (diventa solo il BERSAGLIO del bake) e lo stesso
> design si baka su TUTTI i corpi in batch.

## 1. Stato dell'arte (cosa esiste, 2026-07-10)

- Modello base: umanoide low-poly da Sketchfab (al limite del riggabile in
  Mixamo), UV originali simmetriche poi DE-simmetrizzate a mano (gli outfit
  zombie vogliono sporco/strappi asimmetrici). Isole UV a scala non
  uniforme, scomode da dipingere.
- Corpi derivati dal base: maschio, femmina, femmina+gonna, femmina obesa,
  maschio obeso, children, tank (+ varianti maimed: senza gambe / senza un
  braccio, che SOSTITUISCONO il modello del colpito).
- Outfit fatti A MANO (collage GIMP da foto di vestiti): **16 maschio**
  (uniti in una diffuse unica + versioni insanguinate), **16 femmina**,
  **16 femmina gonna**. Gli outfit "veri" sono **14**: gli indici 14 e 15
  sono corpo CARBONIZZATO e corpo CORROSO dall'acido (status elementali,
  ~uguali per tutti i corpi).
- IN CORSO e **SOSPESO in favore del tool**: i 16 della femmina obesa
  (sarebbe il lavoro che il bake azzera).
- Problema di igiene dati scoperto: il `.blend` della femmina obesa ha la
  UV map SBAGLIATA (quella simmetrica originale); l'FBX rigged è giusto.
  ⇒ la **fonte di verità delle UV è l'FBX che entra nel bake VAT**
  (`vat/bake_zombie.sh`), non i `.blend`.

## 2. Decisioni prese (2026-07-10)

1. **NIENTE remodel** del base: alla scala di gioco (poche decine di px) il
   deform migliore non si vede, e rimodellare = rifare rig, bake VAT di
   tutte le clip, varianti maimed/crawler, riverifica scala mesh/armatura.
2. **NIENTE rifacimento a mano della UV map**: con il bake di proiezione la
   UV bersaglio è irrilevante per l'authoring, e rifarla invaliderebbe i
   3×16 outfit già dipinti (che restano validi così come sono).
3. Per i corpi futuri NON serve più de-simmetrizzare le UV: l'asimmetria
   arriva dal collage proiettato. Va bene qualunque UV senza overlap.
4. **Maimed**: stessi 14 design AGLI STESSI INDICI anche sui modelli
   maimed (bakati dalle stesse immagini sorgente) + un layer di sangue
   extra bakato sopra. Così lo swap normale→maimed conserva l'indice
   outfit per slot e lo zombie "verde" resta verde: la continuità di
   COLORE è ciò che il giocatore traccia; la discrepanza di silhouette/
   genere a 30 px è invisibile. SCARTATA la regola "solo il maschio può
   diventare maimed" (impoverisce la meccanica di azzoppamento per un
   problema cosmetico risolto qui).
5. **Varietà normale/obeso**: design condivisi + QUALCHE design specifico
   per corpo (es. camicia XXL sull'obeso — si legge già in silhouette),
   non 16 set interamente nuovi. La tinta per-agente esistente moltiplica
   le varianti percepite (meglio con capi tendenti al desaturato).
6. **Grading finale a script, non a mano**: gli outfit attuali sono "troppo
   puliti e vividi" — la correzione (desatura/scurisci/curva) è una passata
   batch su tutte le diffuse FINALI, ripetibile e regolabile dopo averli
   visti in gioco. I collage sorgente restano vividi e leggibili.

## 3. I pezzi da costruire

1. **Audit UV** (FATTO, esito 2026-07-10): `fem_version_obese_uvfix.blend`
   è la versione corretta della femmina obesa; tutti gli altri `.blend` da
   texturizzare (man obese, children, i 2 maimed) hanno la UV map corretta.
   Gli FBX rigged restano la fonte di verità (coincidono col bake VAT):
   il tool parte dagli FBX.
2. **Canale UV "proj" per modello** — RISOLTO: generato AL VOLO da
   `outfit_bake.py` (`gfx/outfit_common.py`, layer `proj_front`/`proj_back`
   da coordinate world in rest pose), niente lavoro manuale per corpo.
   L'inquadratura (`Framing`) è condivisa con `outfit_template.py`: quel
   che dipingi sul template atterra sul corpo pixel-per-pixel.
3. **`gfx/outfit_template.py`** — FATTO (v1, 2026-07-10): render Workbench
   fronte+retro (rest pose, cavity+outline, sfondo alpha, default 1024²).
   Il retro è "il personaggio girato": la manica dipinta a sinistra in
   entrambe le viste resta sullo stesso braccio.
4. **`gfx/outfit_bake.py`** — FATTO (v1 + batch, 2026-07-10): UV proj al
   volo, maschera per normale con overlap sfumato (`--soft`, default 0.25),
   Emit-bake Cycles sul canale UV vero (bake a 2× + downscale, margin
   dilation), `--blood-front/--blood-back` = layer sangue alpha-over per i
   maimed e le varianti +16, `--preview` = render fronte/retro col tile
   bakato (verifica a occhio). Verificato su femmina obesa E children con
   collage sintetico a bande: colori giusti fronte/retro, marker sul
   braccio al pixel giusto, stesso design → stesse altezze relative sui
   due corpi. PRIMO COLLAGE VERO (tuta blu, femmina obesa) verificato
   dall'utente: convincente. **Batch** (`--designs dir --out-dir dir`, un
   solo lancio Blender per corpo): scansiona `NN_fronte.png`+`NN_retro.png`
   (NN = indice tile 00..13) → `outfit_NN.png`; se la cartella ha anche
   `sangue_fronte.png`+`sangue_retro.png` (RGBA, dipinto una volta sul
   template) baka anche la variante insanguinata `outfit_NN+16.png`.
   Tile bit-identici al bake singolo (regressione verificata). I collage
   SORGENTE vivono versionati in `gfx/outfit_src/<corpo>/` (gfx/out/ è
   gitignored). Atlas finale: `vat/atlas_tiles.py join
   gfx/out/outfits_<corpo> assets/zombies/zombie_<corpo>_diffuse.png`.
   Tile 14/15 (carbonizzato/corroso): come design `14_`/`15_` oppure tile
   fatti a mano copiati nella out-dir prima del join (i loro insanguinati
   sono 30/31). Blender portatile: `~/Scaricati/blender-5.1.0-linux-x64/`
   (su questo pc; sull'altro verificare il path).
5. **`gfx/outfit_grade.py`** (PIL/ImageMagick): grading globale finale su
   tutte le diffuse (desaturazione X%, scurimento Y%). La "manopola del
   mood" si tara guardando il gioco, non prima.

## 4. Difetti accettati (e perché non contano)

- **Cucitura laterale** dove fronte e retro si incontrano: sugli zombie si
  legge come sporco; overlap sfumato nella maschera la ammorbidisce.
- **Stretching** sui poligoni quasi paralleli alla direzione di proiezione
  (fianchi di braccia/gambe): invisibile alla risoluzione di gioco.

## 5. Ordine di lavoro alla ripresa

1. ~~Audit UV~~ · ~~`outfit_bake.py` su femmina obesa~~ · ~~template +
   primo collage vero verificato dall'utente~~ — FATTI (2026-07-10).
   Precisazione utente: i design NON si condividono tra corpi — ogni
   corpo avrà i suoi 14 collage (il batch resta capace di riusarli).
2. L'utente crea i restanti 13 collage della femmina obesa
   (`gfx/outfit_src/fem_obese/NN_fronte/retro.png`) + il layer
   `sangue_fronte/retro.png`; ritocchi finali (macchie, strappi) a mano
   sui collage o sui tile. Batch → 28 tile → +14/15 speciali → join atlas.
3. Stessa trafila per gli altri corpi (man obese, children, tank, maimed).
4. Grading globale (§3.5) per ultimo.

## 6. Questioni aperte

- ~~Dove vivono gli FBX rigged e quali hanno le UV corrette~~ → audit
  chiuso (§3.1): FBX rigged in `blend/`, tutti a posto.
- ~~UV "proj" salvata nei file vs generata al volo~~ → al volo (§3.2).
- I 3 set già fatti a mano (maschio/femmina/gonna) restano in spazio UV
  nativo: convivono col nuovo flusso senza modifiche; eventualmente si
  RI-generano col tool solo se un giorno serve uniformarli.
- Formato sorgente collage: fronte/retro interi bastano (la divisione
  sopra/sotto che immaginava l'utente è solo un layer in più nel template
  GIMP, non serve al tool).

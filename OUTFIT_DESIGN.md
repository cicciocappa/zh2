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

1. **Audit UV** (LO FA L'UTENTE, in corso): per ogni corpo verificare quale
   file ha le UV giuste; ricordare che conta l'FBX del bake VAT. Annotare
   qui l'esito per corpo.
2. **Canale UV "proj" per modello** (una tantum, ~10 min/corpo, in
   Blender): secondo UV layer — facce rivolte in avanti (per normale) →
   Project From View da camera orto frontale; facce posteriori → idem dal
   retro. Salvato nel file del modello (o creato al volo dallo script §3.4
   via proiezione per normale: da decidere all'implementazione).
3. **`gfx/outfit_template.py`** (headless): renderizza fronte+retro orto
   del corpo come outline/silhouette PNG → il template su cui si collagia
   in GIMP (torso/gambe leggibili invece di isole UV sparse).
4. **`gfx/outfit_bake.py`** (headless, il cuore): input = modello (FBX
   rigged, la fonte di verità) + `fronte.png` + `retro.png`; materiale che
   campiona i collage via UV "proj" con maschera per normale (overlap
   sfumato fronte/retro ai fianchi), **Emit-bake** sul canale UV vero →
   `outfit_XX.png`. Bake a 2× + downscale, margine/dilation attivo.
   Batch: stesso design → tutti i corpi in un colpo (+ modelli maimed con
   layer sangue). Blender portatile: `~/Scaricati/blender-5.1.0-linux-x64/`
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

1. Esito audit UV dell'utente (§3.1) → si sa quali corpi sono a posto.
2. `outfit_bake.py` con banco di prova la **femmina obesa** (il corpo che
   serve adesso, e quello col .blend divergente: bakare dall'FBX lo prova).
3. Template (§3.3) e primo design condiviso proiettato su 2-3 corpi →
   **verifica visiva dell'utente** (regola di progetto).
4. Batch dei 14 design sui corpi mancanti (obesi, children) + maimed con
   sangue (§2.4).
5. Grading globale (§3.5) per ultimo.

## 6. Questioni aperte

- Dove vivono esattamente gli FBX rigged per corpo (in `blend/`?) e quali
  hanno le UV corrette → esito audit utente.
- UV "proj" salvata nei file vs generata al volo dallo script (§3.2).
- I 3 set già fatti a mano (maschio/femmina/gonna) restano in spazio UV
  nativo: convivono col nuovo flusso senza modifiche; eventualmente si
  RI-generano col tool solo se un giorno serve uniformarli.
- Formato sorgente collage: fronte/retro interi bastano (la divisione
  sopra/sotto che immaginava l'utente è solo un layer in più nel template
  GIMP, non serve al tool).

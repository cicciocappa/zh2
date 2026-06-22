#!/usr/bin/env python3
# Atlante outfit zombie <-> tile singoli, per ritoccare le texture col texture
# painting di Blender (un outfit alla volta) e poi ricomporre l'atlante.
#
# Layout (deve combaciare con vat/vat.vs): griglia 4 colonne x 8 righe di outfit
# 256x256 in un atlante 1024x2048. Indice outfit i -> col=i%4, row=i//4, riga 0
# IN CIMA, ordine di lettura. Le righe 0..3 (outfit 0..15) sono i 16 outfit
# NORMALI, le righe 4..7 (outfit 16..31) gli STESSI outfit INSANGUINATI: ferire
# uno zombie = outfit += 16 (vedi vat_layer.c). La V-flip e' gestita nello
# shader: sul PNG i tile stanno in coordinate immagine normali (0,0 =
# alto-sinistra), niente flip.
#
#   vat/atlas_tiles.py split  <atlas.png> <out_dir>  # 1024x2048 -> 32x outfit_NN.png
#   vat/atlas_tiles.py join   <in_dir> <atlas.png>   # 32x outfit_NN.png -> atlante
#   vat/atlas_tiles.py double <atlas.png>            # 1024x1024 (4x4) -> 4x8:
#         meta' alta = outfit reali; meta' bassa = placeholder rosso sangue.
#
# split: i tile mancanti/trasparenti restano tali (puoi ritoccarne uno solo).
# join : preserva i tile esistenti dell'atlante se un outfit_NN.png manca, cosi'
#        puoi ricomporre dopo aver editato solo alcuni outfit (passa --atlas-base
#        per partire dall'atlante attuale invece che da trasparente).
import sys, os
from PIL import Image

NX = 4               # colonne
NY = 8               # righe (4 normali + 4 insanguinate)
TILE = 256           # px per tile
SIZE_X = NX * TILE   # 1024
SIZE_Y = NY * TILE   # 2048
NTILE = NX * NY      # 32
BLOOD = (120, 20, 20, 255)   # placeholder meta' insanguinata

def tile_box(i):
    col, row = i % NX, i // NX
    x, y = col * TILE, row * TILE
    return (x, y, x + TILE, y + TILE)

def split(atlas_path, out_dir):
    atlas = Image.open(atlas_path).convert("RGBA")
    if atlas.size != (SIZE_X, SIZE_Y):
        sys.exit("atlante atteso %dx%d, trovato %s" % (SIZE_X, SIZE_Y, atlas.size))
    os.makedirs(out_dir, exist_ok=True)
    for i in range(NTILE):
        p = os.path.join(out_dir, "outfit_%02d.png" % i)
        atlas.crop(tile_box(i)).save(p)
        print("  ->", p)
    print("split: %d outfit -> %s" % (NTILE, out_dir))

def join(in_dir, atlas_path, base=None):
    if base:
        atlas = Image.open(base).convert("RGBA")
        if atlas.size != (SIZE_X, SIZE_Y):
            sys.exit("base attesa %dx%d, trovata %s" % (SIZE_X, SIZE_Y, atlas.size))
    else:
        atlas = Image.new("RGBA", (SIZE_X, SIZE_Y), (0, 0, 0, 0))
    used = 0
    for i in range(NTILE):
        p = os.path.join(in_dir, "outfit_%02d.png" % i)
        if not os.path.isfile(p):
            print("  skip outfit %02d (manca %s)" % (i, p))
            continue
        t = Image.open(p).convert("RGBA")
        if t.size != (TILE, TILE):
            t = t.resize((TILE, TILE), Image.LANCZOS)
            print("  ! outfit %02d ridimensionato a %dx%d" % (i, TILE, TILE))
        atlas.paste(t, tile_box(i)[:2])
        used += 1
    atlas.save(atlas_path)
    print("join: %d outfit -> %s" % (used, atlas_path))

def double(atlas_path):
    src = Image.open(atlas_path).convert("RGBA")
    if src.size == (SIZE_X, SIZE_Y):
        print("gia' 4x8, niente da fare:", atlas_path); return
    if src.size != (SIZE_X, SIZE_X):
        sys.exit("atteso 4x4 %dx%d, trovato %s" % (SIZE_X, SIZE_X, src.size))
    out = Image.new("RGBA", (SIZE_X, SIZE_Y), BLOOD)
    out.paste(src, (0, 0))   # outfit reali in alto, placeholder sangue sotto
    out.save(atlas_path)
    print("double: %s -> %dx%d (top reale + bottom placeholder)" % (atlas_path, SIZE_X, SIZE_Y))

def main():
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "split":
        split(a[1], a[2])
    elif len(a) >= 3 and a[0] == "join":
        base = None
        if "--atlas-base" in a:
            base = a[a.index("--atlas-base") + 1]
        join(a[1], a[2], base)
    elif len(a) >= 2 and a[0] == "double":
        double(a[1])
    else:
        sys.exit(__doc__)

if __name__ == "__main__":
    main()

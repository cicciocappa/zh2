#!/usr/bin/env python3
# Atlante outfit zombie <-> tile singoli, per ritoccare le texture col texture
# painting di Blender (un outfit alla volta) e poi ricomporre l'atlante.
#
# Layout (deve combaciare con vat/vat.vs): griglia 4x4 di outfit 256x256 in un
# atlante 1024x1024. Indice outfit i -> col=i%4, row=i//4, riga 0 IN CIMA
# all'immagine, ordine di lettura. La V-flip e' gestita nello shader: sul PNG i
# tile stanno in coordinate immagine normali (0,0 = alto-sinistra), niente flip.
#
#   vat/atlas_tiles.py split <atlas.png> <out_dir>   # 1024 -> 16x outfit_NN.png
#   vat/atlas_tiles.py join  <in_dir> <atlas.png>    # 16x outfit_NN.png -> 1024
#
# split: i tile mancanti/trasparenti restano tali (puoi ritoccarne uno solo).
# join : preserva i tile esistenti dell'atlante se un outfit_NN.png manca, cosi'
#        puoi ricomporre dopo aver editato solo alcuni outfit (passa --atlas-base
#        per partire dall'atlante attuale invece che da trasparente).
import sys, os
from PIL import Image

N = 4            # griglia NxN
TILE = 256       # px per tile
SIZE = N * TILE  # 1024

def tile_box(i):
    col, row = i % N, i // N
    x, y = col * TILE, row * TILE
    return (x, y, x + TILE, y + TILE)

def split(atlas_path, out_dir):
    atlas = Image.open(atlas_path).convert("RGBA")
    if atlas.size != (SIZE, SIZE):
        sys.exit("atlante atteso %dx%d, trovato %s" % (SIZE, SIZE, atlas.size))
    os.makedirs(out_dir, exist_ok=True)
    for i in range(N * N):
        p = os.path.join(out_dir, "outfit_%02d.png" % i)
        atlas.crop(tile_box(i)).save(p)
        print("  ->", p)
    print("split: %d outfit -> %s" % (N * N, out_dir))

def join(in_dir, atlas_path, base=None):
    if base:
        atlas = Image.open(base).convert("RGBA")
        if atlas.size != (SIZE, SIZE):
            sys.exit("base attesa %dx%d, trovata %s" % (SIZE, SIZE, atlas.size))
    else:
        atlas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    used = 0
    for i in range(N * N):
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

def main():
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "split":
        split(a[1], a[2])
    elif len(a) >= 3 and a[0] == "join":
        base = None
        if "--atlas-base" in a:
            base = a[a.index("--atlas-base") + 1]
        join(a[1], a[2], base)
    else:
        sys.exit(__doc__)

if __name__ == "__main__":
    main()

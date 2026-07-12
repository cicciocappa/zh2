# font_bake.py — bake a TTF/OTF into the game's .zfnt UI font atlas.
#
#   python3 gfx/font_bake.py regular.ttf assets/fonts/ui.zfnt [--bold bold.ttf] [--px 64]
#   python3 gfx/font_bake.py --default assets/fonts/ui.zfnt [--px 64]   (PIL builtin, test only)
#
# With --bold a second face is packed into the SAME atlas: its glyphs are
# keyed as codepoint | 0x10000 (face bit 16). One texture, one draw call;
# the runtime falls back to the regular glyph when a bold one is missing.
# Global metrics (cap height etc.) come from the regular face.
#
# Output format ZFN1 (little endian), loaded by ui_font_load in vat_horde.c:
#   char magic[4] = "ZFN1"
#   u32  atlas_w, atlas_h
#   f32  px_size          (bake size the metrics below are expressed in)
#   f32  cap_top, cap_h   ('H' ink box: top offset from line-top, height —
#                          the runtime scales text so cap_h maps to the old
#                          font8 glyph height, keeping UI layouts stable)
#   f32  ascent, descent
#   u32  white_x, white_y (center of a solid-white block: UV target for
#                          untextured UI quads — one shader, one draw call)
#   u32  nglyphs
#   per glyph (20 bytes, packed):
#     u32 codepoint; f32 advance;
#     u16 x, y, w, h        (ink rect in the atlas)
#     i16 ox, oy            (ink offset from pen: x right, y down from line-top)
#   u8   atlas[atlas_w*atlas_h]   (A8 coverage, row-major, top-down)
#
# Charset: ASCII 32..126 + Italian accents + degree sign. The A8 atlas is
# antialiased by PIL; the runtime samples it with mipmaps (text is drawn
# smaller than the bake size).
import struct, sys

from PIL import Image, ImageDraw, ImageFont

CHARSET = [chr(c) for c in range(32, 127)] + list("àèéìòù" "ÀÈÉÌÒÙ" "°")
PAD = 2            # blank ring around every glyph (mip/bilinear bleed guard)
WHITE_S = 4        # solid white block side


def bake(font, px, bold=None):
    # measure every glyph first (ink bbox + advance), then shelf-pack
    glyphs = []            # (cp, adv, w, h, ox, oy, image)
    faces = [(font, 0)] + ([(bold, 0x10000)] if bold else [])
    for face, bit in faces:
        for ch in CHARSET:
            adv = face.getlength(ch)
            bb = face.getbbox(ch)                  # (x0,y0,x1,y1), line-top origin
            if bb is None or bb[2] <= bb[0] or bb[3] <= bb[1]:
                glyphs.append((ord(ch) | bit, adv, 0, 0, 0, 0, None))   # blank
                continue
            x0, y0, x1, y1 = bb
            w, h = x1 - x0, y1 - y0
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-x0, -y0), ch, font=face, fill=255)
            glyphs.append((ord(ch) | bit, adv, w, h, x0, y0, img))

    hbb = font.getbbox("H")
    cap_top, cap_h = float(hbb[1]), float(hbb[3] - hbb[1])
    try:
        ascent, descent = (float(v) for v in font.getmetrics())
    except AttributeError:
        ascent, descent = float(px), 0.0

    # shelf packing, tallest first, fixed atlas width
    aw = 512
    order = sorted(range(len(glyphs)), key=lambda i: -glyphs[i][3])
    pos = [None] * len(glyphs)
    # white block claims the top-left corner (also makes texel (0,0) white:
    # any stray zero UV falls on solid coverage, not on a random glyph)
    shelf_x, shelf_y, shelf_h = WHITE_S + PAD, 0, WHITE_S + PAD
    for i in order:
        _, _, w, h, _, _, img = glyphs[i]
        if img is None:
            pos[i] = (0, 0)
            continue
        if shelf_x + w + PAD > aw:
            shelf_y += shelf_h
            shelf_x, shelf_h = 0, 0
        pos[i] = (shelf_x, shelf_y)
        shelf_x += w + PAD
        shelf_h = max(shelf_h, h + PAD)
    ah = 1
    while ah < shelf_y + shelf_h:
        ah *= 2

    atlas = Image.new("L", (aw, ah), 0)
    atlas.paste(Image.new("L", (WHITE_S, WHITE_S), 255), (0, 0))
    for i, (cp, adv, w, h, ox, oy, img) in enumerate(glyphs):
        if img is not None:
            atlas.paste(img, pos[i])

    out = bytearray()
    out += b"ZFN1"
    out += struct.pack("<II", aw, ah)
    out += struct.pack("<fffff", float(px), cap_top, cap_h, ascent, descent)
    out += struct.pack("<II", WHITE_S // 2, WHITE_S // 2)
    out += struct.pack("<I", len(glyphs))
    for i, (cp, adv, w, h, ox, oy, img) in enumerate(glyphs):
        x, y = pos[i]
        out += struct.pack("<If4H2h", cp, float(adv), x, y, w, h, ox, oy)
    out += atlas.tobytes()
    return bytes(out), aw, ah


def main():
    args = sys.argv[1:]
    px = 64
    bold = None
    if "--px" in args:
        k = args.index("--px")
        px = int(args[k + 1])
        del args[k:k + 2]
    if "--bold" in args:
        k = args.index("--bold")
        bold_path = args[k + 1]
        del args[k:k + 2]
        bold = True
    if args and args[0] == "--default":
        if len(args) != 2:
            sys.exit("usage: font_bake.py --default out.zfnt [--px N]")
        font = ImageFont.load_default(size=px)   # PIL >= 10.1 (test/placeholder)
        out_path = args[1]
        src = "PIL default"
        bold = None
    elif len(args) == 2:
        font = ImageFont.truetype(args[0], px)
        out_path = args[1]
        src = args[0]
        if bold:
            bold = ImageFont.truetype(bold_path, px)
            src += f" + {bold_path}"
    else:
        sys.exit("usage: font_bake.py font.ttf out.zfnt [--bold b.ttf] [--px N]  |  --default out.zfnt")

    blob, aw, ah = bake(font, px, bold)
    nfaces = 2 if bold else 1
    with open(out_path, "wb") as f:
        f.write(blob)
    print(f"{out_path}: {src} @ {px}px -> atlas {aw}x{ah}, "
          f"{len(CHARSET) * nfaces} glifi, {len(blob)} byte")


if __name__ == "__main__":
    main()

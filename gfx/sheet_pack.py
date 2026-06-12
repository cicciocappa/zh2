#!/usr/bin/env python3
# Packs the per-frame PNGs from sprite_render.py into sprite sheets:
# one sheet per action, 16 rows (directions) x N cols (frames), downscaled
# from the supersampled render resolution to the target sprite size.
#
#   python3 gfx/sheet_pack.py gfx/out/placeholder --out-px 64
#
# Reads <dir>/meta.json, writes <dir>/<action>_sheet.png and updates
# meta.json with sheet layout + rescaled anchor. The anchor (ground point
# of the character, world origin) is what the renderer pins to the agent's
# (x, y): anchor px is relative to each frame cell's top-left corner.

import argparse
import json
import os
import sys
from PIL import Image


def main():
    p = argparse.ArgumentParser()
    p.add_argument("dir", help="output dir of sprite_render.py")
    p.add_argument("--out-px", type=int, default=64,
                   help="final frame size in the sheet")
    args = p.parse_args()

    meta_path = os.path.join(args.dir, "meta.json")
    with open(meta_path) as fp:
        meta = json.load(fp)

    scale = args.out_px / meta["px"]
    dirs = meta["dirs"]

    for name, act in meta["actions"].items():
        n = act["frames"]
        sheet = Image.new("RGBA", (n * args.out_px, dirs * args.out_px))
        for k in range(dirs):
            for j in range(n):
                fp_png = os.path.join(args.dir, name, f"d{k:02d}_f{j:02d}.png")
                if not os.path.exists(fp_png):
                    sys.exit(f"missing frame: {fp_png}")
                img = Image.open(fp_png).convert("RGBA")
                img = img.resize((args.out_px, args.out_px), Image.LANCZOS)
                sheet.paste(img, (j * args.out_px, k * args.out_px))
        out = os.path.join(args.dir, f"{name}_sheet.png")
        sheet.save(out)
        act["sheet"] = os.path.basename(out)
        act["frame_px"] = args.out_px
        print(f"[sheet_pack] {out}  ({n}x{dirs} frames @ {args.out_px}px)")

    meta["sheet_anchor"] = [meta["anchor"][0] * scale,
                            meta["anchor"][1] * scale]
    meta["sheet_px_per_m"] = meta["px_per_m"] * scale
    with open(meta_path, "w") as fp:
        json.dump(meta, fp, indent=2)


main()

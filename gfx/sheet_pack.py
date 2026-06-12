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
#
# Each sheet is also written as <action>_sheet.zspr, a trivial binary the
# C sandbox can load without a PNG decoder (SDL3 has none):
#   "ZSP3" | u32: w h frame_px dirs frames | f32: anchor_x anchor_y
#   px_per_m k_z stride_m duration_s | w*h*4 raw RGBA bytes, rows top to
#   bottom. Little-endian. stride_m = meters one animation cycle covers
#   on the ground (0 for non-locomotion clips), duration_s = native clip
#   length; both measured by sprite_render.py. Locomotion plays by
#   distance/stride_m, time-based clips (idle) by time/duration_s.

import argparse
import json
import os
import struct
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
        zspr = os.path.join(args.dir, f"{name}_sheet.zspr")
        with open(zspr, "wb") as fp:
            fp.write(b"ZSP3")
            fp.write(struct.pack("<5I", sheet.width, sheet.height,
                                 args.out_px, dirs, n))
            fp.write(struct.pack("<6f", meta["anchor"][0] * scale,
                                 meta["anchor"][1] * scale,
                                 meta["px_per_m"] * scale, meta["k_z"],
                                 act.get("stride_m", 0.0),
                                 act.get("duration_s", 0.0)))
            fp.write(sheet.tobytes())
        print(f"[sheet_pack] {out} + .zspr  ({n}x{dirs} frames @ {args.out_px}px)")

    meta["sheet_anchor"] = [meta["anchor"][0] * scale,
                            meta["anchor"][1] * scale]
    meta["sheet_px_per_m"] = meta["px_per_m"] * scale
    with open(meta_path, "w") as fp:
        json.dump(meta, fp, indent=2)


main()

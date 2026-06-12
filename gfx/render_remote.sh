#!/bin/sh
# Render the sprite batch on the remote workstation (GPU via --gpu, with
# CPU fallback inside sprite_render.py) and pull the frames back, then
# pack the sheets locally. Run from the repo root.
#
#   gfx/render_remote.sh [out_dir] [name=file.fbx:frames ...]
#
# Defaults: out_dir = gfx/out/zombie, anims = the full 7-walk + attack
# batch. FBX file names are relative to the local pack dir, which is
# synced to the workstation on every run (rsync delta: cheap after the
# first time). Knobs via env:
#   ZH2_RENDER_HOST  (default oxhy@172.30.0.18)
#   ZH2_PACK         (default ~/Scaricati/Scary_Zombie_Pack)
#   ZH2_EXTRA        extra sprite_render.py args (e.g. "--max-tex 1024")
set -e

HOST=${ZH2_RENDER_HOST:-oxhy@172.30.0.18}
BLENDER=blender-5.1.2-linux-x64/blender
PACK=${ZH2_PACK:-$HOME/Scaricati/Scary_Zombie_Pack}
RDIR=zh2-render

OUT=${1:-gfx/out/zombie}
[ $# -gt 0 ] && shift
ANIMS=${*:-"walk=walk.fbx:16 walk2=walk2.fbx:16 walk3=walk3.fbx:16 \
walk4=walk4.fbx:16 walk5=walk5.fbx:16 walk6=walk6.fbx:16 \
walk7=walk7.fbx:16 attack=attack.fbx:4 idle=idle.fbx:16"}

args=""
for a in $ANIMS; do
    args="$args --anim ${a%%=*}=pack/${a#*=}"
done

echo "== sync to $HOST =="
rsync -a gfx/sprite_render.py "$HOST:$RDIR/"
rsync -a --exclude 'roll.fbx' "$PACK/" "$HOST:$RDIR/pack/"

echo "== render =="
# shellcheck disable=SC2086
ssh "$HOST" "cd $RDIR && \$HOME/$BLENDER --background --python sprite_render.py -- \
    --gpu --fbx pack/zombie.fbx $args --out out $ZH2_EXTRA" \
    | grep -E "GPU|WARNING|stride|texture|done|Error"

echo "== pull frames =="
mkdir -p "$OUT"
rsync -a --delete "$HOST:$RDIR/out/" "$OUT/"

python3 gfx/sheet_pack.py "$OUT" --out-px 64

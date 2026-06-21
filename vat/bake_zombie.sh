#!/usr/bin/env bash
# Bake di un asset VAT completo (13 clip) per UN modello, col workflow
# semplificato: il MODELLO riggato + le animazioni applicate al suo rig
# (stesso scheletro mixamorig). migrazione_3d.md.
#
#   vat/bake_zombie.sh <model.fbx> <out_prefix> [anim_dir]
#
# <model.fbx>  = modello lowpoly riggato in Mixamo (mesh+UV+skin, una T-pose
#                qualunque va bene: dell'animazione del file non importa nulla).
# <out_prefix> = es. vat/assets/zombie_man  -> _mesh.bin/_pos.raw/.../_meta.txt
# [anim_dir]   = cartella con gli FBX d'animazione (default: vat/source/, i
#                sorgenti versionati). Gli FBX d'anim possono essere "senza
#                skin": serve solo l'Action. (es. vat/source/fem_rigged.fbx)
set -e
BL=${BLENDER:-$HOME/Scaricati/blender-5.1.0-linux-x64/blender}
MODEL="$1"; PREFIX="$2"
ADIR="${3:-vat/source}"
# Flag extra passate a bake_vat.py (es. correzione orientamento):
#   EXTRA="--rotx -90" vat/bake_zombie.sh model.fbx out_prefix
EXTRA="${EXTRA:-}"
[ -z "$PREFIX" ] && { echo "uso: $0 <model.fbx> <out_prefix> [anim_dir]"; exit 1; }

# nome clip -> file anim. Adatta i nomi file se scarichi le anim separate.
declare -A A=(
 [walk]="Zombie Walk.fbx" [walk_2]="Zombie Walk 2.fbx" [walk_3]="Zombie Walk 3.fbx"
 [walk_4]="Zombie Walk 4.fbx" [walk_5]="Zombie Walk 5.fbx" [walk_6]="Zombie Walk 6.fbx"
 [run]="Zombie Running.fbx" [run_2]="Zombie Running 2.fbx"
 [idle]="Zombie Idle.fbx" [idle_2]="Zombie Idle 2.fbx"
 [attack]="Zombie Attack.fbx" [attack_2]="Zombie Attack 2.fbx"
 [scream]="Zombie Scream.fbx"
 [hit]="Zombie Reaction Hit.fbx" [dying]="Zombie Dying.fbx" [death]="Zombie Death.fbx"
)
ORDER="walk walk_2 walk_3 walk_4 walk_5 walk_6 run run_2 idle idle_2 attack attack_2 scream hit dying death"

# suffissi ESATTI: un glob "${PREFIX}"_*.bin cancellerebbe anche le varianti che
# condividono il prefisso (es. zombie_fem vs zombie_fem_obese).
rm -f "${PREFIX}"_mesh.bin "${PREFIX}"_pos.raw "${PREFIX}"_norm.raw "${PREFIX}"_meta.txt
first=1
for name in $ORDER; do
  fbx="$ADIR/${A[$name]}"
  [ -f "$fbx" ] || { echo "SKIP $name (manca $fbx)"; continue; }
  flag=""; [ $first -eq 0 ] && flag="--append"; first=0
  "$BL" --background --python vat/bake_vat.py -- --model "$MODEL" --anim "$fbx" "$PREFIX" "$name" $flag $EXTRA \
     2>&1 | grep -E "\[VAT\] OK|ERRORE|Traceback" || true
done
echo "=== $PREFIX ==="; tail -n +1 "${PREFIX}_meta.txt" | head -20

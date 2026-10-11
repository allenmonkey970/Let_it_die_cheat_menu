#!/usr/bin/env bash
# Client for the in-DLL control channel (lid_mod\rpc\cmd.txt -> result.txt). The game must be running.
# Usage:
#   rpc.sh ue3_status | ue3_diag | input_diag
#   rpc.sh inf_dur|inf_ammo|inf_stamina [on|off]     (no value = query)
#   rpc.sh add_currency <which 0-5> <amount>          (0 Kill Coins, 1 SPLithium, 2 Bank, 3 Tank,
#                                                      4 Bloodnium, 5 TDM Points)
#   rpc.sh spawn_part <PT_id> [Head|Arm_R|Arm_L|Body|Legs]   (temporary try-on)
GAME="${LID_GAME:?set LID_GAME to your LET IT DIE folder}"
RPC="$GAME/Binaries/Win64/lid_mod/rpc"
mkdir -p "$RPC"
IDF="$RPC/.id"
[ -f "$IDF" ] || echo 1 > "$IDF"
id=$(( $(cat "$IDF") + 1 )); echo "$id" > "$IDF"

op="$1"; shift
{
  echo "id=$id"
  echo "op=$op"
  case "$op" in
    spawn_part)                   echo "ptid=$1"; echo "site=${2:-Arm_R}" ;;
    inf_dur|inf_ammo|inf_stamina) echo "value=$1" ;;
    add_currency)                 echo "which=$1"; echo "value=$2" ;;
  esac
} > "$RPC/cmd.tmp"
mv "$RPC/cmd.tmp" "$RPC/cmd.txt"

for i in $(seq 1 40); do
  if [ -f "$RPC/result.txt" ] && head -1 "$RPC/result.txt" | grep -q "^id=$id$"; then
    cat "$RPC/result.txt"; exit 0
  fi
  sleep 0.5
done
echo "TIMEOUT waiting for id=$id (is the game running?)"

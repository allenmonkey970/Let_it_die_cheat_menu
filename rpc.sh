#!/usr/bin/env bash
# Tiny client for the in-DLL scanner RPC.
# Usage:
#   rpc.sh scan_new i32 150        # fresh scan for a value
#   rpc.sh scan_eq  i32 140        # keep addrs now equal to value
#   rpc.sh scan_dec                # keep addrs that decreased since last scan
#   rpc.sh scan_inc | scan_changed | scan_unchanged
#   rpc.sh list                    # show up to 60 current candidates + values
#   rpc.sh read <hexaddr>          # read i32/f32 at an address
#   rpc.sh set  <hexaddr> i32 9999 # one-time write
#   rpc.sh freeze <hexaddr> i32 9999
#   rpc.sh unfreeze_all
RPC="C:/Users/antho/lid-overlay-mod/rpc"
IDF="$RPC/.id"
[ -f "$IDF" ] || echo 1 > "$IDF"
id=$(( $(cat "$IDF") + 1 )); echo "$id" > "$IDF"

op="$1"; shift
{
  echo "id=$id"
  echo "op=$op"
  case "$op" in
    scan_new|scan_eq) echo "type=$1"; echo "value=$2" ;;
    set|freeze)       echo "addr=$1"; echo "type=$2"; echo "value=$3" ;;
    read)             echo "addr=$1" ;;
    spawn_part)       echo "ptid=$1"; echo "site=${2:-Arm_R}" ;;
    add_money|add_spirit|inf_dur|inf_ammo|inf_stamina) echo "value=$1" ;;
    vslot)            echo "cls=$1"; echo "off=$2" ;;
  esac
} > "$RPC/cmd.tmp"
mv "$RPC/cmd.tmp" "$RPC/cmd.txt"

for i in $(seq 1 40); do
  if [ -f "$RPC/result.txt" ] && head -1 "$RPC/result.txt" | grep -q "^id=$id$"; then
    cat "$RPC/result.txt"; exit 0
  fi
  sleep 0.5
done
echo "TIMEOUT waiting for id=$id (scan may still be running; re-read rpc/result.txt)"

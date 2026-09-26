#!/bin/sh
# 改良版1 (rtl_tcp + 高速スキャン) をビルドして OTA で書き込む
#   sh host/ota_fastscan.sh 2   -> 2号機 (10.5.2.21) で試す    firmware/build_fastscan2/
#   sh host/ota_fastscan.sh 1   -> 1号機 (10.5.2.20) に入れる  firmware/build_fastscan1/
# ビルドフォルダと送り先 IP は号機番号で必ず対になる (取り違え防止)
ID=${1:-}
case "$ID" in
  1|2|3|4|5|6|7|8) ;;
  *) echo "号機番号を付けてください: sh host/ota_fastscan.sh 2 (2号機で試す) / sh host/ota_fastscan.sh 1 (1号機)" >&2; exit 1 ;;
esac
IP=10.5.2.$((19 + ID))
HOST="$(cd "$(dirname "$0")" && pwd)"
B="$HOST/../firmware/build_fastscan$ID"
mkdir -p "$B" && cd "$B" || exit 1
if [ ! -f Makefile ]; then
  cmake -DCMAKE_BUILD_TYPE=Release -DBOARD_ID="$ID" -DFASTSCAN=1 .. > cmake.log 2>&1 || { tail -20 cmake.log; echo "cmake 失敗" >&2; exit 1; }
fi
if ! make -j4 > make.log 2>&1; then
  grep -E "error" make.log | head -20
  echo "ビルド失敗 (全体: $B/make.log)" >&2
  exit 1
fi
grep -E "Built target rtltcp" make.log
python3 "$HOST/ota_push.py" "$IP" rtltcp_rp2350.bin

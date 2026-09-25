#!/bin/bash
# flash.sh -- BOOTSEL モードで Mac に現れた RP2350 ドライブに uf2 をコピーする
#   1) RP2350-POE-ETH の BOOT ボタンを押しながら USB-C を Mac に挿す (または RESET)
#   2) Finder に "RP2350" ドライブが出たら ./flash.sh
set -e
cd "$(dirname "$0")"
UF2="build/rtltcp_rp2350.uf2"
[ -f "$UF2" ] || { echo "$UF2 がありません。先に ./build.sh を実行してください" >&2; exit 1; }
DEST=""
for d in /Volumes/RP2350 /Volumes/RPI-RP2; do
  [ -d "$d" ] && DEST="$d" && break
done
if [ -z "$DEST" ]; then
  echo "BOOTSEL ドライブ (/Volumes/RP2350) が見つかりません。BOOT を押しながら USB を接続してください" >&2
  exit 1
fi
cp "$UF2" "$DEST/"
echo "書き込みました -> $DEST  (ボードが自動で再起動します)"

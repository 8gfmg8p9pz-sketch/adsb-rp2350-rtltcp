#!/bin/sh
# 改良版1 の操作画面: コマンドを打てる + HIT / END を時刻付きで表示し ~/fastscan_log.txt に残す
#   sh host/fastscan_console.sh      -> 1号機 (10.5.2.20)
#   sh host/fastscan_console.sh 2    -> 2号機 (10.5.2.21)
# HELP でコマンド一覧、Ctrl+C で終了
ID=${1:-1}
IP=10.5.2.$((19 + ID))
LOG="$HOME/fastscan_log.txt"
echo "board $ID ($IP) TCP 1237 に接続  (HELP でコマンド一覧 / Ctrl+C で終了 / 記録: $LOG)"
cat | nc "$IP" 1237 | while IFS= read -r line; do
  printf '%s %s\n' "$(date '+%H:%M:%S')" "$line"
  case "$line" in
    HIT*|END*|TUNE*|SCAN*) printf '%s board%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$ID" "$line" >> "$LOG" ;;
  esac
done

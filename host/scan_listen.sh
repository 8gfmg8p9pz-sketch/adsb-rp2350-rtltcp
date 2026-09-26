#!/bin/sh
# スキャナーの音声を Mac で聞く (要: brew install ffmpeg)
#   sh host/scan_listen.sh        -> 2号機 (UDP 1242)
#   sh host/scan_listen.sh 3      -> 3号機 (UDP 1243)
ID=${1:-2}
PORT=$((1240 + ID))
echo "listening board $ID on UDP $PORT (Ctrl+C で終了)"
exec ffplay -hide_banner -loglevel warning -nodisp -fflags nobuffer -flags low_delay \
  -f s16le -ar 12500 -ch_layout mono "udp://0.0.0.0:${PORT}"

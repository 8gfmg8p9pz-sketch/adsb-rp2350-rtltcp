#!/bin/sh
# 改良版1 のスキャン音声を Mac で聞く (要: brew install ffmpeg)
#   sh host/fastscan_listen.sh      -> 1号機 (10.5.2.20, UDP 1241)
#   sh host/fastscan_listen.sh 2    -> 2号機 (10.5.2.21, UDP 1242)
# 10 秒ごとにボードへ HELLO を送り、音声の送り先をこの Mac にしてもらう
ID=${1:-1}
IP=10.5.2.$((19 + ID))
PORT=$((1240 + ID))
( while true; do echo HELLO | nc -u -w 1 "$IP" "$PORT" >/dev/null 2>&1; sleep 10; done ) &
HELLO=$!
trap 'kill $HELLO 2>/dev/null' EXIT INT TERM
echo "board $ID ($IP) の音声を UDP $PORT で再生 (Ctrl+C で終了)"
ffplay -hide_banner -loglevel warning -nodisp -fflags nobuffer -flags low_delay \
  -f s16le -ar 12500 -ch_layout mono "udp://0.0.0.0:${PORT}"

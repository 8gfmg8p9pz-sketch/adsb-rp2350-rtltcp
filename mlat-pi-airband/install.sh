#!/bin/bash
# MLATサーバー機(10.5.2.207)に airband_rx を入れる。画面(hdmi-radar/map4k)は変更しない
set -e
cd "$(dirname "$0")"
sudo apt-get install -y python3-numpy alsa-utils >/dev/null
install -m 755 airband_rx.py /home/pi/airband_rx.py
sudo install -m 644 airband-rx.service /etc/systemd/system/airband-rx.service
# 音声の出力先を HDMI に固定 (Pi 3 / 旧 ALSA: 0=自動 1=イヤホン 2=HDMI)
amixer -q cset numid=3 2 2>/dev/null || true
amixer -q sset PCM 100% 2>/dev/null || true
sudo systemctl daemon-reload
sudo systemctl enable --now airband-rx
sleep 6
systemctl is-active airband-rx
curl -s http://127.0.0.1:8091/api/status; echo
echo "スマホで http://10.5.2.207:8091/ を開いてください"

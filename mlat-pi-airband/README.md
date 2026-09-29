# mlat-pi-airband — MLATサーバー機でエアバンド音声を HDMI に載せる

- 対象機: adsb-mlat (Raspberry Pi 3, LAN 10.5.2.207 / Tailscale 100.118.233.68)
- 受信元: 5号機 rtl_tcp 10.5.2.24:1234 (R820T, 250 kS/s)
- 処理: Pi 上で AM 復調 → aplay で HDMI 音声に出力。画面 (hdmi-radar / map4k) は変更しない
- 操作: スマホで http://10.5.2.207:8091/ (周波数・プリセット・スケルチ・音量・ゲイン・ON/OFF)
- 設定保存: /home/pi/airband_rx.json
- サービス: airband-rx.service (自動起動)
- 注意: rtl_tcp は同時1クライアント。Mac の GQRX で 5号機を開くと Pi 側は切れる

## インストール / 更新 (Mac から)
    cd ~/rp2350-poe-airband-iq/mlat-pi-airband && scp airband_rx.py airband-rx.service install.sh pi@10.5.2.207:/tmp/ && ssh -t pi@10.5.2.207 'cd /tmp && bash install.sh'

## ログ
    ssh pi@10.5.2.207 'journalctl -u airband-rx -n 30 --no-pager'

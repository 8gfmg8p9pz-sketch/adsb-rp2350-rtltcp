# 1号機 復旧用データ

2d1a060  Keep the firmware currently running on board 1 (rtl_tcp, 2029ad0, crc32 da46ab0e)

| ファイル | 用途 |
|------|------|
| board1_rtltcp_2029ad0.uf2（189 KB） | USB 書き込み用：予備機づくり・1号機の復旧 |
| board1_rtltcp_2029ad0.bin（94 KB） | OTA 用：1号機を今の状態に戻すとき |

## いざというときの戻し方

**LAN 経由（1号機が動いているとき）**

    python3 ~/rp2350-poe-airband-iq/host/ota_push.py 10.5.2.20 ~/rp2350-poe-airband-iq/エアバンドIQ受信/board1_rtltcp_2029ad0.bin

**USB（予備機・起動しなくなったとき）**：PoE を抜き、ジャンパを外し、BOOTSEL を押しながら Mac に挿してから

    cp ~/rp2350-poe-airband-iq/エアバンドIQ受信/board1_rtltcp_2029ad0.uf2 /Volumes/RP2350/

これで、2号機の実験で何があっても、1号機はいつでも確認済みの状態に戻せます。

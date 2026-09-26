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

## このフォルダの中身（1号機 一式）

| ファイル | 内容 |
|------|------|
| board1_rtltcp_2029ad0.uf2 / .bin | 今動いているファームウェア（crc32 da46ab0e） |
| source_2029ad0.tar.gz | 上のファームウェアを作った時点のソース一式（Git タグ board1-2029ad0 と同じ） |
| gqrx_default.conf | GQRX の設定。`cp gqrx_default.conf ~/.config/gqrx/default.conf` で戻す |
| airband_launcher.applescript | デスクトップ起動アプリの元。`osacompile -o ~/Desktop/エアバンド受信.app airband_launcher.applescript` |
| ota_push.py / ota1.sh | LAN 経由の書き込み |

## 1号機の設定

| 項目 | 値 |
|------|------|
| ボード | Waveshare RP2350-POE-ETH |
| IP / MAC | 10.5.2.20 / 02:08:DC:11:09:01 |
| ポート | 1234 rtl_tcp、1235 OTA、1236 ログ |
| ドングル | NooElec（RTL2832U + R820T） |
| 受信 | 250 kS/s、488 kB/s、GQRX で AM 118.1 MHz |
| 給電 | PoE のみ。ピンヘッダ VSYS(31) と VBUS(32) をジャンパ |
| USB | ハブなしの USB-C オス → USB-A メス OTG アダプタ |

LED：赤 = USB 未検出、白 = 初期化中、紫 = チューナーエラー、黄 = LAN なし、青 = 待ち受け、緑 = 配信中

USB で書き込むときは、PoE を抜き、ジャンパを外してから BOOTSEL で Mac に接続する（同時給電しない）。

## ソースから作り直す場合

    cd /tmp && tar xzf ~/rp2350-poe-airband-iq/エアバンドIQ受信/source_2029ad0.tar.gz
    cd source_2029ad0/firmware && mkdir -p build && cd build
    cmake -DCMAKE_BUILD_TYPE=Release .. && make -j4

（または `git checkout board1-2029ad0`）

# RP2350-POE-ETH rtl_tcp server

Waveshare **RP2350-POE-ETH**（RP2350 + W6300 Ethernet + PoE）に RTL-SDR ドングルを USB でつなぎ、
IQ データを **rtl_tcp 互換**で LAN に配信するファームウェアです。PoE ケーブル1本でアンテナ直下に設置できます。

## 動作確認済み（2026-09）

- NooElec（RTL2832U + R820T）を USB ホストで認識・チューナー初期化
- **250 kS/s を 488 kB/s、USB エラー 0** で配信
- Mac の GQRX から接続し、**FM 放送とエアバンド（118.1 MHz、AM）を受信**
- OTA（LAN 経由のファームウェア更新）、ネットワークログ

## できないこと

RP2350 の USB は **USB 1.1（フルスピード）** のため、ADS-B（1090 MHz）のデコードに必要な約 2 MS/s は流せません。
RTL2832U は 300 kS/s の次が 900 kS/s なので、この構成の上限は約 300 kS/s（約 240 kHz 幅）です。
エアバンド音声・ACARS・rtl_433 などの狭帯域向けです。

## ネットワーク

| 項目 | 値 |
|------|------|
| IP | `10.5.2.20`（固定、`firmware/config.h` の `CFG_IP_ADDR`） |
| TCP 1234 | rtl_tcp（同時1クライアント） |
| TCP 1235 | OTA（ファームウェア更新） |
| TCP 1236 | ネットワークログ（printf 出力、起動以降 8KB） |

## ハードウェアの注意（重要）

PoE 給電時、PoE の 5V は VSYS には来ますが、ダイオード D1 に阻まれて **USB-C の VBUS には出ません**。
USB ホストでドングルに給電するため、ピンヘッダの **VSYS（31番）と VBUS（32番）をジャンパ**でつなぎます。

| 作業 | ジャンパ | 給電 |
|------|------|------|
| 通常運用 | 付ける | PoE のみ（USB-C は OTG アダプタで RTL-SDR） |
| USB で書き込み | 外す | PoE を抜いて USB-C を PC に（BOOTSEL） |

- OTG アダプタは **ハブを内蔵していない** 単純な USB-C オス → USB-A メスを使う
- ジャンパ付きで PoE と PC の USB を同時につながない（5V が PC に逆流する）

## LED

| 色 | 意味 |
|------|------|
| 白 | 起動中 / USB デバイス初期化中 |
| 赤 | USB デバイス未検出 |
| 紫 | チューナー初期化エラー |
| 黄 | LAN リンクなし |
| 青 | 待ち受け中 |
| 緑 | 配信中 |

## ビルド（macOS）

    brew install cmake gcc-arm-embedded libusb
    git clone --recursive https://github.com/raspberrypi/pico-sdk.git ~/.pico-sdk/pico-sdk
    export PICO_SDK_PATH=$HOME/.pico-sdk/pico-sdk
    cd firmware && mkdir -p build && cd build
    cmake -DCMAKE_BUILD_TYPE=Release ..
    make -j4

出力: `build/rtltcp_rp2350.uf2`（USB 書き込み用）、`build/rtltcp_rp2350.bin`（OTA 用）

## 書き込み

**OTA（通常）**

    python3 host/ota_push.py 10.5.2.20

更新後は PoE を抜き差しして、ドングルもリセットしてください（ソフト再起動では VBUS が切れず、ドングルが前の状態のまま固まることがあります）。

**USB（初回・復旧用）**: ジャンパを外し、BOOTSEL を押しながら USB-C を PC へ → ボリューム `RP2350` に `rtltcp_rp2350.uf2` をコピー。

## 受信（GQRX）

- Device string: `rtl_tcp=10.5.2.20:1234`
- Input rate: `250000`、Mode: AM、DC remove: オン
- 設定例: `host/gqrx_default.conf`

## ログ

    nc 10.5.2.20 1236

## ディレクトリ

| パス | 内容 |
|------|------|
| `firmware/src/` | main.c、usb_host.c、rtl2832u.c、net_server.c、ota.c、netlog.c |
| `firmware/tuner/` | R820T ドライバ（librtlsdr 由来） |
| `firmware/tinyusb_patch/` | RP2350 USB ホストドライバの修正版（Bulk を EPX で高速化） |
| `firmware/lib/` | W6300（WIZnet ioLibrary）、WS2812 |
| `host/` | ota_push.py、GQRX 設定、起動用 AppleScript |
| `simulator/` | 実機なしで試せるダミー rtl_tcp サーバー |

## 主な修正履歴

- USB ホスト: mount 処理をメインループへ移動、ディスクリプタをちょうどの長さで要求
- R820T: 初期化の待ち時間を復活、TinyUSB の詳細ログを無効化（割り込み遅延で I2C が失敗していた）
- RTL2832U の Bulk 最大パケット長をフルスピード用 64B（`CFG_RTL_EPA_MAXPKT 0x4000`）に → 60 → 488 kB/s

## 電波法について

受信は自由ですが、特定の相手方に対する通信（管制交信など）の内容を他人に漏らしたり利用したりすることは、電波法第59条で禁止されています。

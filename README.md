# RP2350-POE-ETH rtl_tcp サーバー (RTL-SDR を LAN 越しに使う)

Waveshare **RP2350-POE-ETH** の USB-C に RTL-SDR ドングルを挿し、PoE 給電のままアンテナポール上で
**rtl_tcp 互換サーバー**として動かすファームウェアです。
`firmware/config.h` の IP アドレスを書き換えてビルド → uf2 を書き込むだけで動きます。

```
 アンテナ ─ RTL-SDR ─(USB-C OTG)─ RP2350-POE-ETH ─(LAN/PoE)─ Pi / Mac の rtl_tcp クライアント
                                                         (SDR++, GQRX, readsb→tar1090 など)
```

> **重要な前提 (検証目的で作成)**
> RP2350 の USB ホストは **USB1.1 Full-Speed (12 Mbps)** です。実効で流せる IQ データは
> **最大 ~1 MB/s ≒ 500 kS/s** 程度で、ADS-B (readsb/dump1090) が必要とする 2.0〜2.4 MS/s
> (4〜4.8 MB/s) は流せません。高いレートを要求するとドングル内部 FIFO が溢れてサンプルが
> 抜けます（壊れはしません）。**ADS-B の実運用は成立しない見込み**で、250 kS/s 前後の
> 狭帯域受信 (航空無線 AM、FM、APRS 等) の遠隔 SDR としては実用になります。
> tar1090/dump1090 側は何も変更不要で、詰まるのは RP2350 の USB 転送レートだけです。

---

## 1. フォルダ構成

```
adsb-rp2350-rtltcp/
├── README.md                 ← このファイル
├── firmware/
│   ├── config.h              ★ IP アドレス等はここだけ編集
│   ├── build.sh              Mac でビルド → build/rtltcp_rp2350.uf2
│   ├── flash.sh              BOOTSEL ドライブに uf2 をコピー
│   ├── build/rtltcp_rp2350.uf2   (config.h 初期値 10.5.2.30 でビルド済み)
│   ├── CMakeLists.txt
│   ├── src/                  main.c / usb_host.c / rtl2832u.c / net_server.c / ring.c / leds.c
│   ├── tuner/                tuner_r82xx.c (librtlsdr から流用, GPL-2.0)
│   ├── tinyusb_patch/        RP2350 ホスト HCD のバルク高速化パッチ (下記)
│   └── lib/                  Waveshare の W6300 / WS2812 ライブラリ
└── host/
    ├── rtltcp_pipe.py        rtl_tcp クライアント (転送レート計測 / IQ を stdout に流す)
    └── rtltcp_to_readsb.sh   Pi 上で readsb にデコードさせ既設 readsb/tar1090 に Beast で流し込む
```

---

## 2. ハードウェア

| 部品 | 内容 |
|---|---|
| RP2350-POE-ETH | + PoE モジュール (SKU 35200) |
| RTL-SDR | R820T/R820T2/R860 または R828D (RTL-SDR Blog V3/V4 含む) ※他チューナは未対応 |
| USB-C OTG 変換 | USB-C(オス) → USB-A(メス)。**ドングル側に 5V が必要** (後述) |
| アンテナ | 1090 MHz 用など、SMA/MCX でドングルへ |
| (任意) USB-UART | GP0(TX)/GP1(RX) 115200bps でログ確認用。USB-C はホストに使うので PC には出ません |

### USB ホスト給電 (VBUS) について
RP2350-POE-ETH の USB-C は本来「電源入力」なので、PoE 給電時に USB-C の VBUS に 5V が
**出ない**可能性があります。ドングルは約 300 mA 必要です。
1. まず PoE で起動し、OTG 変換を挿してドングルの LED が点くか / 本機の LED が「赤(SDR 無し)」から
   変わるかを確認
2. 出ない場合は **給電付き OTG (Y ケーブル)** を使い、ボードの 5V ピン (VBUS/VSYS) から
   USB-A メス側の VBUS に 5V を供給する（GND 共通）

### LED (WS2812) の意味
| 色 | 状態 |
|---|---|
| 白 | 起動直後 |
| **赤** | RTL-SDR が見つからない (USB 未接続 / VBUS 無し / 列挙失敗) |
| **マゼンタ** | ドングルは見えたがチューナー初期化失敗 (未対応チューナー等) |
| **黄** | SDR OK、Ethernet リンク無し or IP 未取得 |
| **青** | 待受中 (クライアント未接続) |
| **緑** | クライアント接続中・IQ 送信中 |

---

## 3. 設定 → ビルド → 書き込み (Mac)

### 3-1. IP アドレスを設定
`firmware/config.h` を開いて自宅 LAN に合わせて書き換えます。

```c
#define CFG_USE_STATIC_IP     1              /* 0 にすると DHCP */
#define CFG_IP_ADDR           192, 168, 1, 150
#define CFG_IP_MASK           255, 255, 255, 0
#define CFG_IP_GATEWAY        192, 168, 1, 1
#define CFG_MAC_ADDR          0x02, 0x08, 0xDC, 0x11, 0x09, 0x01   /* 2台目は末尾を変える */
#define CFG_RTLTCP_PORT       1234
```

### 3-2. ビルド
```bash
cd ~/adsb-rp2350-rtltcp/firmware
./build.sh
```
`firmware/build/rtltcp_rp2350.uf2` ができます。
(`~/.pico-sdk/pico-sdk` と Homebrew の `arm-none-eabi-gcc` を使います。TinyUSB サブモジュールは自動取得)

### 3-3. 書き込み
1. ボードの **BOOT** を押しながら USB-C を Mac に接続 (Finder に `RP2350` ドライブが出る)
2. ```bash
   cd ~/adsb-rp2350-rtltcp/firmware
   ./flash.sh
   ```
3. Mac から外し、OTG 変換 + RTL-SDR を USB-C に挿し、LAN (PoE) を接続

---

## 4. 動作確認

### 4-1. 疎通と転送レート計測 (Mac でも Pi でも可)
```bash
cd ~/adsb-rp2350-rtltcp/host
python3 rtltcp_pipe.py 10.5.2.30 --rate 250000 --gain 49.6 --bench 10
```
2 秒ごとに `kB/s = kS/s (達成率 %)` が出ます。250 kS/s で達成率 100% 近くなら正常です。
`--rate 1024000` や `--rate 2400000` にすると USB1.1 の上限で頭打ちになるのが見えます。

### 4-2. SDR++ / GQRX から使う
- SDR++: Source → **RTL-TCP**、Host `10.5.2.30` Port `1234`、サンプルレート 250 kS/s
- GQRX: Device string `rtl_tcp=10.5.2.30:1234`、Input rate `250000`

### 4-3. 既設の readsb/tar1090 に流し込む (Pi 上で実行、検証用)
```bash
scp -r ~/adsb-rp2350-rtltcp/host pi@<PiのIP>:~/rtltcp-host
ssh pi@<PiのIP>
cd ~/rtltcp-host
./rtltcp_to_readsb.sh 10.5.2.30
```
別プロセスの readsb が 2.4 MS/s を要求して IQ をデコードし、結果を既設 readsb の Beast 入力
(`127.0.0.1:30004`) に流します。tar1090 はそのまま表示するだけなので設定変更は不要です。
既設側が `--net-bi-port 30004` で待ち受けている (readsb/dump1090-fa の既定) ことが前提です。
※ 帯域不足でデコード率は極めて低くなります (達成率 20% 前後の想定)。

---

## 5. 対応している rtl_tcp コマンド
0x01 周波数 / 0x02 サンプルレート / 0x03 ゲインモード / 0x04 ゲイン / 0x05 ppm 補正 /
0x07 テストモード / 0x08 AGC / 0x09 ダイレクトサンプリング / 0x0b,0x0c XTAL /
0x0d ゲイン index / 0x0e Bias-T。 (0x06 IF ゲイン, 0x0a オフセット同調は R82xx 非対応のため無視)

---

## 6. 仕組み・実装メモ

- **core0**: TinyUSB ホストで RTL2832U を列挙 → librtlsdr 移植コード (`rtl2832u.c` + `tuner_r82xx.c`)
  でベースバンド/チューナー初期化 → バルク IN (EP 0x81) から 4 KB ずつ IQ を受けてリングバッファ (192 KB) へ
- **core1**: W6300 (SPI0, GP16-21) で TCP サーバー (ポート 1234)。接続時に 12 byte ヘッダ
  `RTL0 + tuner_type + gain_count` を送り、リングバッファを送出。5 byte コマンドは core0 へキューで渡す
- **tinyusb_patch/**: 素の TinyUSB は RP2040/RP2350 ホストでバルク EP を「割り込み EP ハードウェア」
  (1 ms に 1 パケット = 64 kB/s) に割り当てるため、バルク EP を EPX エンジン (連続転送・ダブルバッファ)
  に載せ替えるパッチを当てています。EPX はコントロール転送と共用なので、レジスタアクセス時は
  ストリームを一旦止めてから行います (`usb_stream_pause/resume`)
- シリアル (GP0/GP1, 115200) に 5 秒ごとの統計 `[stat] usb xx kB/s tcp xx kB/s ring ... overrun ...` を出します
- サンプルレートを firmware 側で丸めたい場合は `config.h` の `CFG_LIMIT_SAMPLE_RATE=1`

### 既知の未検証ポイント (実機で確認が必要)
1. **VBUS**: PoE 給電時に USB-C から 5V が出るか (2 章)
2. **EP A 最大パケット長**: `config.h` の `CFG_RTL_EPA_MAXPKT`。Full-Speed で IQ が流れない /
   `usberr` が増え続ける場合は `0x4000` を試す
3. **データトグル**: 接続のたびに `rtlsdr_reset_buffer()` を送るため、ドングル側がトグルをリセットすると
   最初の 1 転送が DATA_SEQ エラーになる → HCD 側で自動再同期する実装 (1 パケット捨てる) にしてある
4. **短パケット**: ダブルバッファ受信中に 64B 未満のパケットが来ると次の 1 パケットを取りこぼす可能性
   (RTL2832U は通常フルパケットしか送らないので実害は小さい想定)
5. **シリアル出力**: 115200bps の printf 中 (~9ms) は USB の再投入が遅れ、その間サンプルが抜ける。
   気になる場合は `CFG_STATUS_PRINT_SEC` を大きくする

### ライセンス
`firmware/tuner/tuner_r82xx.c` と `src/rtl2832u.c` は osmocom librtlsdr (GPL-2.0-or-later) 由来です。
Waveshare / WIZnet ライブラリは各元ライセンス (Apache-2.0 / BSD-3-Clause)、TinyUSB は MIT。

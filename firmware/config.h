/*
 * config.h  --  RP2350-POE-ETH rtl_tcp サーバー  ユーザー設定
 *
 * ★ ここだけ書き換えて build.sh を実行し、生成された .uf2 を書き込めば動きます ★
 *
 *   1) ネットワーク (固定IP か DHCP)
 *   2) 起動時の SDR 初期値 (周波数 / サンプルレート / ゲイン)
 *   3) デバッグ用シリアルのピン
 */
#ifndef RTLTCP_CONFIG_H
#define RTLTCP_CONFIG_H

/* ---------------------------------------------------------------------------
 * 1. ネットワーク設定
 * ------------------------------------------------------------------------- */

/* 1 = 固定IP を使う (下の CFG_IP_* を使用)   0 = DHCP で取得する */
#define CFG_USE_STATIC_IP     1

/* 固定IP (CFG_USE_STATIC_IP=1 のときのみ有効)  ← 環境に合わせて変更 */
#define CFG_IP_ADDR           10, 5, 2, 20
#define CFG_IP_MASK           255, 255, 255, 0
#define CFG_IP_GATEWAY        10, 5, 2, 1
#define CFG_IP_DNS            10, 5, 2, 1

/* MAC アドレス (ローカル管理アドレス。2台使うときは末尾を変える) */
#define CFG_MAC_ADDR          0x02, 0x08, 0xDC, 0x11, 0x09, 0x01

/* rtl_tcp の待受ポート (標準 1234) */
#define CFG_RTLTCP_PORT       1234

/* ---------------------------------------------------------------------------
 * 2. SDR 初期値 (クライアントが接続すると rtl_tcp コマンドで上書きされる)
 * ------------------------------------------------------------------------- */
#define CFG_DEFAULT_FREQ_HZ        1090000000u   /* ADS-B 1090 MHz */
#define CFG_DEFAULT_SAMPLE_RATE    250000u       /* USB1.1 で現実的に流せる上限付近 (下記注意) */
#define CFG_DEFAULT_GAIN_TENTHDB   496           /* 49.6 dB (最大)   0 = AGC(自動) にしたいときは CFG_DEFAULT_AGC=1 */
#define CFG_DEFAULT_AGC            0             /* 1 = チューナー AGC 有効 */
#define CFG_DEFAULT_PPM            0             /* 周波数補正 ppm */
#define CFG_DEFAULT_BIAS_TEE       0             /* 1 = Bias-T (LNA給電) ON  ※RTL-SDR Blog V3/V4 のみ */

/*
 * 【重要】RP2350 の USB ホストは USB1.1 Full-Speed(12 Mbps) です。
 *   実測で流せる IQ データは最大 ~1 MB/s 程度 = 約 500 kS/s (8bit I/Q) が限界で、
 *   ADS-B デコードに必要な 2.0〜2.4 MS/s (4〜4.8 MB/s) は流せません。
 *   クライアントが高いレートを要求すると、その分 RTL2832U 内部 FIFO が溢れて
 *   サンプルが欠落します (壊れはしません)。
 *   CFG_LIMIT_SAMPLE_RATE=1 にすると、要求レートを CFG_MAX_SAMPLE_RATE に丸めます。
 */
#define CFG_LIMIT_SAMPLE_RATE      0
#define CFG_MAX_SAMPLE_RATE        1024000u

/* ---------------------------------------------------------------------------
 * 3. デバッグ用シリアル (UART0, 115200 8N1)
 *    RP2350-POE-ETH の GP0 = TX, GP1 = RX   (USB-C は SDR 用ホストになるので使えません)
 * ------------------------------------------------------------------------- */
#define CFG_DEBUG_UART_TX_PIN      0
#define CFG_DEBUG_UART_RX_PIN      1
#define CFG_DEBUG_UART_BAUD        115200

/* ---------------------------------------------------------------------------
 * 4. 内部パラメータ (通常変更不要)
 * ------------------------------------------------------------------------- */
#define CFG_RING_BUFFER_BYTES      (192 * 1024)  /* IQ リングバッファ (SRAM 520KB のうち) */
#define CFG_USB_XFER_BYTES         4096          /* 1回の USB バルク転送サイズ */
#define CFG_TCP_CHUNK_BYTES        8192          /* 1回の W6300 send() サイズ (ソケット0 TXバッファ 16KB) */
#define CFG_STATUS_PRINT_SEC       5             /* 統計をシリアルに出す間隔 [s] */

/* RTL2832U の USB EP A 最大パケット長レジスタ (0x2158) に書く値。
 * librtlsdr の既定は 0x0002 (=512B, High-Speed 用)。RP2350 は Full-Speed(64B) なので、
 * データが全く流れない / usberr が増え続ける場合は 0x4000 (=64B) を試してください。 */
#define CFG_RTL_EPA_MAXPKT         0x4000

#endif /* RTLTCP_CONFIG_H */

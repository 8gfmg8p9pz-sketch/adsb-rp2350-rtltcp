/*
 * scanner.h -- on-board channel scanner for the RP2350-POE-ETH (experimental unit)
 *
 *  core0 : retune / settle / FFT level measurement / AM demodulation   (scanner.c)
 *  core1 : control TCP 1237, coordination UDP 1238, audio UDP 1240+id  (scan_net.c)
 */
#ifndef SCANNER_H
#define SCANNER_H

#include <stdint.h>
#include <stdbool.h>
#include "ring.h"

#define SCAN_MAX_CH           256
#define SCAN_CTRL_PORT        1237
#define SCAN_COORD_PORT       1238
#define SCAN_AUDIO_PORT_BASE  1240          /* audio port = 1240 + board id */
#define SCAN_AUDIO_RATE       12500         /* 250 kS/s / 20, signed 16 bit mono, little endian */

/* core0 */
void scanner_init(void);                    /* load settings from flash (call before core1 starts) */
void scanner_on_sdr_ready(void);            /* dongle opened: configure and start streaming */
void scanner_on_sdr_lost(void);
void scanner_task(void);                    /* call from the core0 main loop */
void scanner_print_stats(void);
bool scanner_monitoring(void);

/* core1 */
typedef void (*scan_emit_fn)(const char *s);
void     scanner_ctl(const char *line, scan_emit_fn emit);     /* one text command */
void     scanner_set_client_ip(const uint8_t ip[4]);            /* control client (audio AUTO) */
bool     scanner_audio_dest(uint8_t ip[4], uint16_t *port);
uint32_t scanner_monitor_freq(void);                            /* 0 = not monitoring */
void     scanner_peer_mon(uint8_t id, uint32_t freq_hz);        /* peer is monitoring freq */
void     scanner_peer_end(uint8_t id, uint32_t freq_hz);
ring_t  *scanner_audio_ring(void);
void     scan_net_poll(void);                                   /* scan_net.c */

#endif

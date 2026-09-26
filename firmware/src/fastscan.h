/*
 * fastscan.h -- 改良版1: rtl_tcp server + fast on-board channel scanner
 *
 * While no rtl_tcp client (GQRX) is connected the board scans the registered
 * channels itself; a client that connects takes the tuner over and the scan
 * resumes when it leaves.
 *
 *  core0 : segment/window hopping, FFT levels, AM demodulation   (fastscan.c)
 *  core1 : control TCP 1237, coordination UDP 1238, audio UDP 1240+id  (fastscan_net.c)
 */
#ifndef FASTSCAN_H
#define FASTSCAN_H

#include <stdint.h>
#include <stdbool.h>
#include "ring.h"

#define FSCAN_MAX_CH          256
#define FSCAN_LABEL_LEN       16            /* bytes incl. NUL (UTF-8) */
#define FSCAN_CTRL_PORT       1237
#define FSCAN_COORD_PORT      1238          /* same protocol as the board 2 scanner */
#define FSCAN_AUDIO_PORT_BASE 1240          /* audio port = 1240 + board id */
#define FSCAN_AUDIO_RATE      12500         /* signed 16 bit mono, little endian */

/* core0 (main.c) */
void fastscan_init(void);                   /* load settings from flash (before core1 starts) */
void fastscan_start(void);                  /* dongle open and no rtl_tcp client: configure, stream, scan */
bool fastscan_suspend(void);                /* rtl_tcp client: stop scanning; true if the tuner was ours */
void fastscan_on_sdr_lost(void);
void fastscan_task(void);                   /* call from the core0 main loop */
void fastscan_print_stats(void);
bool fastscan_monitoring(void);

/* core1 (fastscan_net.c) */
typedef void (*fscan_emit_fn)(const char *s);
void     fastscan_ctl(const char *line, fscan_emit_fn emit);   /* one text command */
void     fastscan_set_client_ip(const uint8_t ip[4]);          /* control client (audio AUTO) */
bool     fastscan_audio_dest(uint8_t ip[4], uint16_t *port);   /* fixed / AUTO destination */
bool     fastscan_audio_auto(void);
uint32_t fastscan_monitor_freq(void);                          /* 0 = not receiving */
void     fastscan_peer_mon(uint8_t id, uint32_t freq_hz);      /* peer is receiving freq */
void     fastscan_peer_end(uint8_t id, uint32_t freq_hz);
ring_t  *fastscan_audio_ring(void);
ring_t  *fastscan_event_ring(void);                            /* text lines: HIT / END / ... */
void     fastscan_net_poll(void);                              /* fastscan_net.c */
void     fastscan_net_status(fscan_emit_fn emit);              /* fastscan_net.c: audio listener */

#endif

/*
 * fastscan.c -- 改良版1: fast on-board channel scanner beside the rtl_tcp server
 *
 * While no rtl_tcp client is connected, the registered channels are scanned on
 * the board.  Channels within ~180 kHz of each other share one "window" that is
 * measured at once by a 256 point FFT (as on the board 2 scanner, scanner.c, so
 * squelch levels in dBFS are comparable).  What makes it faster:
 *
 *  - Segments: the R820T IF filter is opened to ~2.4 MHz and windows that lie
 *    within SPAN of each other share one tuner setting.  Inside a segment the
 *    next window is reached by moving only the RTL2832U's digital down-converter
 *    (1-2 register writes: no I2C to the tuner, no PLL lock wait).  The tuner is
 *    retuned only when the scan crosses into the next segment.
 *  - The bulk transfer in flight is dropped instead of waited for, and the
 *    dongle FIFO is flushed after the hop, so every sample that arrives belongs
 *    to the new window (the few still inside the RTL2832U filters are suppressed
 *    by the FFT window) instead of discarding a guessed 3 ms.
 *  - 512 byte USB transfers (1 ms of IQ) while scanning hand the samples over sooner.
 *
 * A channel above its squelch is received: mixed down, decimated 250k -> 50k
 * (triangular) -> 12.5k (41 tap FIR) and AM demodulated into the audio ring,
 * which core1 sends by UDP.  Several units share which channel they receive
 * (UDP broadcast, same protocol as the board 2 scanner) and skip each other's.
 */
#include "config.h"
#if CFG_FASTSCAN

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"

#include "fastscan.h"
#include "rtl_tcp.h"
#include "usb_host.h"
#include "rtl2832u.h"

extern unsigned r82xx_pll_wait_us;          /* tuner_r82xx.c */
extern int      rtlsdr_keep_i2c_repeater;   /* rtl2832u.c */

/* ------------------------------------------------------------------------- */
#define FS             250000u
#define NFFT           256
#define BIN_HZ         ((float)FS / NFFT)
#define WIN_SPAN_HZ    180000u              /* max distance first..last channel in a window */
#define WIN_EDGE_HZ    95000u               /* max |channel - window centre| */
#define DC_GUARD_HZ    6000u                /* keep channels away from the DC spike */
#define IFBW_EXTRA_HZ  450000u              /* tuner IF bandwidth = SPAN + one window + margin */
#define TUNE_OFFSET_HZ 20000u               /* TUNE: channel sits 20 kHz off the DC spike */
#define SQL_TAIL_MS    300u                 /* audio stays on this long after the level drops */
#define CONFIRM_MS     40u                  /* a hit must show up in the 8 ms averages within this */
#define DATA_TIMEOUT_MS 1000u               /* no IQ after a hop: restart the stream */
#define MAX_WIN        FSCAN_MAX_CH
#define MAX_PEERS      8
#define LOG_LEN        64
#define FLASH_OFF      0x3E0000u            /* 2 sectors (board 2 scanner: 0x3F0000, OTA stage: 0x200000..0x2FFFFF) */
#define FLASH_LEN      (2 * FLASH_SECTOR_SIZE)
#define STORE_MAGIC    0x31435346u          /* 'FSC1' */
#define STORE_VERSION  1
#define SQL_DEFAULT    INT16_MIN            /* channel uses the global squelch */
#define LEVEL_NONE     INT16_MIN            /* not measured yet */
#define CH_SKIP        0x01                 /* registered, but left out of the scan */

typedef struct {
    uint32_t freq;
    int16_t  sql_x10;
    uint8_t  flags;
    uint8_t  pad;
    char     label[FSCAN_LABEL_LEN];
} store_ch_t;

typedef struct {
    uint32_t   magic;
    uint32_t   version;
    int16_t    sql_x10;        /* global squelch, dBFS x10 */
    uint16_t   hold_ms;        /* keep receiving this long after the signal drops */
    int16_t    gain_x10;       /* tuner gain dB x10, -1 = AGC */
    int16_t    ppm;
    uint16_t   span_khz;       /* windows sharing one tuner setting, 0 = retune the tuner every window */
    uint16_t   settle_us;      /* samples discarded after a DDC hop */
    uint16_t   tsettle_us;     /* samples discarded after a tuner retune */
    uint16_t   pllwait_us;     /* R820T PLL lock wait */
    uint16_t   navg;           /* FFTs averaged per window */
    uint16_t   bw_hz;          /* detection bandwidth per channel */
    uint16_t   xfer;           /* USB bulk transfer size while scanning */
    uint8_t    snr_db;         /* also require level >= noise floor + snr (0 = off) */
    uint8_t    scan_on;
    uint8_t    audio_auto;     /* 1 = send audio to the listener / last control client */
    uint8_t    audio_ip[4];
    uint8_t    pad[3];
    uint16_t   nch;
    uint16_t   pad2;
    store_ch_t ch[FSCAN_MAX_CH];   /* sorted by frequency */
} store_t;

static store_t            s_cfg;              /* shared, protected by s_cs */
static critical_section_t s_cs;
static volatile uint32_t  s_cfg_gen = 1;      /* bumped by core1 when channels / squelch change */
static volatile bool      s_gain_pending, s_bw_pending, s_force_tune, s_xfer_pending, s_ppm_pending;
static volatile uint32_t  s_tune_freq;        /* TUNE: receive this frequency instead of scanning */
static uint8_t            s_client_ip[4];
static volatile bool      s_client_valid;
static unsigned           s_pllwait_default;  /* rtl_tcp mode keeps the stock PLL wait */

/* peers (other units) */
typedef struct { uint32_t freq; uint32_t expire_ms; uint8_t id; } peer_t;
static peer_t s_peers[MAX_PEERS];

/* hit log (written by core0, read by core1, under s_cs) */
typedef struct {
    uint32_t start_ms, dur_ms, freq;
    int16_t  peak_x10;
    char     label[FSCAN_LABEL_LEN];
} logent_t;
static logent_t s_log[LOG_LEN];
static uint32_t s_log_n;

/* rings core0 -> core1 */
static uint8_t s_audio_buf[16384];
static ring_t  s_audio_ring;
static uint8_t s_ev_buf[2048];
static ring_t  s_ev_ring;
static uint32_t s_ev_dropped;

/* ------------------------------------------------------------------------- */
/* core0 working copy of the scan plan (rebuilt under s_cs)                  */
typedef struct {
    uint32_t freq;
    int16_t  sql_x10;          /* effective squelch */
    int16_t  last_x10;         /* last measured level */
    int16_t  floor_x10;        /* noise floor estimate */
    uint16_t hits;
    char     label[FSCAN_LABEL_LEN];
} wch_t;
typedef struct { uint32_t center; uint16_t first, count, seg; } win_t;
typedef struct { uint32_t tuner; } seg_t;

static wch_t    w_ch[FSCAN_MAX_CH];
static uint16_t w_nch;
static win_t    w_win[MAX_WIN];
static uint16_t w_nwin;
static seg_t    w_seg[MAX_WIN];
static uint16_t w_nseg;
static uint16_t w_cur;
static uint32_t s_plan_gen;

typedef enum { ST_OFF = 0, ST_IDLE, ST_HOP, ST_SETTLE, ST_MEASURE, ST_MONITOR } state_t;
static const char *const st_name[] = { "off", "idle", "hop", "settle", "measure", "receive" };
static volatile state_t s_state = ST_OFF;
static volatile bool    s_active;             /* the scanner owns the tuner and the IQ ring */
static volatile bool    s_suspended;          /* an rtl_tcp client has the tuner */
static uint32_t s_tuner_hz;                   /* where the tuner is, 0 = unknown */
static uint32_t s_center_hz;                  /* centre of the window being received */
static uint32_t s_discard;
static uint16_t s_nfft;
static bool     s_manual;
static uint32_t s_manual_freq;
static uint32_t s_hop_ms;
static float    s_acc[NFFT];

/* receiving */
static volatile uint32_t s_mon_freq;          /* 0 = not receiving */
static struct {
    uint32_t freq, center, gen;
    int16_t  sql_x10, floor_x10, peak_x10;
    int      wch;                             /* index into w_ch, -1 for TUNE */
    bool     manual;
    bool     confirmed;                       /* scan hit seen again by the 8 ms squelch check */
    uint32_t start_ms, active_ms;
    uint16_t blocks;
    char     label[FSCAN_LABEL_LEN];
} m;
static float s_nco_re, s_nco_im, s_rot_re, s_rot_im, s_dc;
static float s_mon_acc[NFFT];                 /* squelch: spectra of the last 8 blocks */

/* decimating FIRs (delay lines are doubled so that every dot product is contiguous) */
#define DEC1   5                              /* 250k -> 50k, triangular 9 taps (CIC order 2) */
#define DEC2   4                              /* 50k -> 12.5k */
#define H1_LEN 9
#define H2_LEN 41
typedef struct { const float *h; int len, dec, pos, cnt; float *re, *im; } decim_t;
static float  s_h1[H1_LEN], s_h2[H2_LEN];
static float  s_d1_re[2 * H1_LEN], s_d1_im[2 * H1_LEN], s_d2_re[2 * H2_LEN], s_d2_im[2 * H2_LEN];
static decim_t s_dec1 = { s_h1, H1_LEN, DEC1, 0, 0, s_d1_re, s_d1_im };
static decim_t s_dec2 = { s_h2, H2_LEN, DEC2, 0, 0, s_d2_re, s_d2_im };

/* stats (core0) and the last computed rates (read by core1) */
static uint32_t st_windows, st_checks, st_hits, st_false, st_fail;
static uint32_t st_ddc_n, st_ddc_us, st_tune_n, st_tune_us, st_hop_max;
static volatile uint32_t rt_win_s, rt_ch_s, rt_ddc_us, rt_tune_us, rt_tune_s;

/* DSP tables */
static float   s_hann[NFFT], s_tw_re[NFFT / 2], s_tw_im[NFFT / 2];
static uint8_t s_rev[NFFT];
static float   s_re[NFFT], s_im[NFFT];
static uint8_t s_iq[NFFT * 2];
static uint8_t s_junk[512];

/* ------------------------------------------------------------------------- */
static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void fmt_mhz(char *b, size_t n, uint32_t hz)
{
    snprintf(b, n, "%lu.%04lu", (unsigned long)(hz / 1000000), (unsigned long)(hz % 1000000 / 100));
}

static void fmt_db(char *b, size_t n, int32_t x10)
{
    if (x10 == LEVEL_NONE) { snprintf(b, n, "-"); return; }
    snprintf(b, n, "%s%ld.%ld", x10 < 0 ? "-" : "", (long)(labs(x10) / 10), (long)(labs(x10) % 10));
}

/* one line to the netlog and to the control client (WATCH); dropped whole if the ring is full */
static void event(const char *fmt, ...)
{
    char b[120];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(b) - 2) n = sizeof(b) - 2;
    b[n++] = '\n';
    b[n] = 0;
    printf("[fscan] %s", b);
    if (ring_free(&s_ev_ring) >= (uint32_t)n) ring_write(&s_ev_ring, (const uint8_t *)b, (uint32_t)n);
    else s_ev_dropped++;
}

/* ------------------------------------------------------------------------- */
/* settings                                                                  */
static void set_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.magic      = STORE_MAGIC;
    s_cfg.version    = STORE_VERSION;
    s_cfg.sql_x10    = -530;
    s_cfg.hold_ms    = 3000;
    s_cfg.gain_x10   = 402;
    s_cfg.span_khz   = 1800;
    s_cfg.settle_us  = 0;
    s_cfg.tsettle_us = 1000;
    s_cfg.pllwait_us = 2000;
    s_cfg.navg       = 2;
    s_cfg.bw_hz      = 6000;
    s_cfg.xfer       = 512;
    s_cfg.audio_auto = 1;
    s_cfg.scan_on    = 1;
    s_cfg.nch        = 1;
    s_cfg.ch[0].freq = 118100000u;
    s_cfg.ch[0].sql_x10 = SQL_DEFAULT;
}

static void sanitize(void)
{
    if (s_cfg.navg < 1 || s_cfg.navg > 8) s_cfg.navg = 2;
    if (s_cfg.xfer < 256 || s_cfg.xfer > CFG_USB_XFER_BYTES || s_cfg.xfer % 64) s_cfg.xfer = 512;
    if (s_cfg.span_khz > 4000) s_cfg.span_khz = 1800;
    if (s_cfg.bw_hz < 1000 || s_cfg.bw_hz > 25000) s_cfg.bw_hz = 6000;
    if (s_cfg.pllwait_us < 200) s_cfg.pllwait_us = 2000;
    for (int i = 0; i < s_cfg.nch; i++) s_cfg.ch[i].label[FSCAN_LABEL_LEN - 1] = 0;
}

static void load_settings(void)
{
    const store_t *f = (const store_t *)(XIP_BASE + FLASH_OFF);
    if (f->magic == STORE_MAGIC && f->version == STORE_VERSION && f->nch <= FSCAN_MAX_CH) {
        memcpy(&s_cfg, f, sizeof(s_cfg));
        sanitize();
        printf("[fscan] settings loaded from flash: %u channels\n", s_cfg.nch);
    } else {
        set_defaults();
        printf("[fscan] no saved settings, using defaults\n");
    }
}

/* called on core1: core0 is locked out while the flash is written */
static bool save_settings(void)
{
    static uint8_t page[FLASH_LEN] __attribute__((aligned(4)));
    _Static_assert(sizeof(store_t) <= FLASH_LEN, "store too large");
    memset(page, 0xFF, sizeof(page));
    critical_section_enter_blocking(&s_cs);
    memcpy(page, &s_cfg, sizeof(s_cfg));
    critical_section_exit(&s_cs);
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_OFF, FLASH_LEN);
    flash_range_program(FLASH_OFF, page, FLASH_LEN);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    return memcmp((const void *)(XIP_BASE + FLASH_OFF), page, sizeof(s_cfg)) == 0;
}

/* ------------------------------------------------------------------------- */
/* DSP                                                                       */
static void dsp_init(void)
{
    const float pi = 3.14159265358979f;
    for (int i = 0; i < NFFT; i++) s_hann[i] = 0.5f - 0.5f * cosf(2.0f * pi * i / NFFT);
    for (int i = 0; i < NFFT / 2; i++) {
        s_tw_re[i] = cosf(2.0f * pi * i / NFFT);
        s_tw_im[i] = -sinf(2.0f * pi * i / NFFT);
    }
    int bits = 0;
    while ((1 << bits) < NFFT) bits++;
    for (int i = 0; i < NFFT; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        s_rev[i] = (uint8_t)r;
    }

    /* stage 1: two cascaded 5-sample boxcars = triangle 1..5..1 */
    for (int i = 0; i < H1_LEN; i++) s_h1[i] = (float)(i < 5 ? i + 1 : H1_LEN - i) / 25.0f;
    /* stage 2: Hamming windowed sinc at 50 kS/s, -6 dB at 6 kHz (passband ~4 kHz, stop >= 8 kHz) */
    const float fc = 6000.0f / (FS / DEC1);
    float sum = 0;
    for (int i = 0; i < H2_LEN; i++) {
        float x = (float)(i - (H2_LEN - 1) / 2);
        float s = (x == 0.0f) ? 2.0f * fc : sinf(2.0f * pi * fc * x) / (pi * x);
        s_h2[i] = s * (0.54f - 0.46f * cosf(2.0f * pi * i / (H2_LEN - 1)));
        sum += s_h2[i];
    }
    for (int i = 0; i < H2_LEN; i++) s_h2[i] /= sum;
}

static void fft(float *re, float *im)
{
    for (int i = 0; i < NFFT; i++) {
        int j = s_rev[i];
        if (j > i) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= NFFT; len <<= 1) {
        int half = len >> 1, step = NFFT / len;
        for (int i = 0; i < NFFT; i += len) {
            for (int k = 0; k < half; k++) {
                float wr = s_tw_re[k * step], wi = s_tw_im[k * step];
                int a = i + k, b = a + half;
                float xr = re[b] * wr - im[b] * wi;
                float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr;        im[a] += xi;
            }
        }
    }
}

/* one block of 256 IQ pairs -> power spectrum added to acc[] */
static void block_to_spectrum(const uint8_t *iq, float *acc)
{
    float mr = 0, mi = 0;
    for (int i = 0; i < NFFT; i++) {
        s_re[i] = ((float)iq[2 * i] - 127.5f) * (1.0f / 127.5f);
        s_im[i] = ((float)iq[2 * i + 1] - 127.5f) * (1.0f / 127.5f);
        mr += s_re[i]; mi += s_im[i];
    }
    mr *= 1.0f / NFFT; mi *= 1.0f / NFFT;      /* remove the DC offset of the dongle */
    for (int i = 0; i < NFFT; i++) {
        s_re[i] = (s_re[i] - mr) * s_hann[i];
        s_im[i] = (s_im[i] - mi) * s_hann[i];
    }
    fft(s_re, s_im);
    for (int i = 0; i < NFFT; i++) acc[i] += s_re[i] * s_re[i] + s_im[i] * s_im[i];
}

/* level of a channel (offset from the centre) in dBFS x10 */
static int16_t channel_level(const float *acc, int navg, int32_t off_hz, uint16_t bw_hz)
{
    const float ref = (float)NFFT * NFFT * 3.0f / 8.0f;   /* full scale tone = 0 dBFS */
    int k0 = (int)lroundf((float)off_hz / BIN_HZ);
    int half = (int)lroundf((float)bw_hz * 0.5f / BIN_HZ);
    if (half < 1) half = 1;
    float sum = 0;
    for (int j = -half; j <= half; j++) sum += acc[(k0 + j + NFFT) % NFFT];
    float p = sum / ((float)navg * ref);
    if (p < 1e-12f) p = 1e-12f;
    return (int16_t)lroundf(100.0f * log10f(p));
}

/* noise floor: follows the level down quickly and creeps up slowly, so it sits at
 * the lower edge of the noise and is hardly lifted by short transmissions */
static void track_floor(wch_t *c, int16_t lv)
{
    if (c->floor_x10 == LEVEL_NONE) { c->floor_x10 = lv; return; }
    int32_t d = lv - c->floor_x10;
    c->floor_x10 += (int16_t)(d < 0 ? (d - 3) / 4 : (d + 63) / 64);
}

static bool level_open(int16_t lv, int16_t sql, int16_t floor)
{
    if (lv < sql) return false;
    if (s_cfg.snr_db && floor != LEVEL_NONE && lv < floor + 10 * (int16_t)s_cfg.snr_db) return false;
    return true;
}

static bool decim_push(decim_t *d, float xr, float xi, float *yr, float *yi)
{
    int p = d->pos;
    d->re[p] = d->re[p + d->len] = xr;
    d->im[p] = d->im[p + d->len] = xi;
    if (++p == d->len) p = 0;
    d->pos = p;
    if (++d->cnt < d->dec) return false;
    d->cnt = 0;
    const float *h = d->h, *vr = &d->re[p], *vi = &d->im[p];   /* oldest .. newest */
    float ar = 0, ai = 0;
    for (int k = 0; k < d->len; k++) { ar += h[k] * vr[k]; ai += h[k] * vi[k]; }
    *yr = ar; *yi = ai;
    return true;
}

static void demod_reset(void)
{
    s_nco_re = 1.0f; s_nco_im = 0.0f;
    memset(s_d1_re, 0, sizeof(s_d1_re)); memset(s_d1_im, 0, sizeof(s_d1_im));
    memset(s_d2_re, 0, sizeof(s_d2_re)); memset(s_d2_im, 0, sizeof(s_d2_im));
    s_dec1.pos = s_dec1.cnt = s_dec2.pos = s_dec2.cnt = 0;
    s_dc = 0;
}

/* mix the channel to 0 Hz, decimate to 12.5 kHz, AM envelope -> audio ring (if 'out') */
static void demod_block(const uint8_t *iq, bool out)
{
    int16_t pcm[NFFT / (DEC1 * DEC2) + 1];
    int n = 0;
    for (int i = 0; i < NFFT; i++) {
        float xr = ((float)iq[2 * i] - 127.5f), xi = ((float)iq[2 * i + 1] - 127.5f);
        float yr = xr * s_nco_re - xi * s_nco_im;
        float yi = xr * s_nco_im + xi * s_nco_re;
        float nr = s_nco_re * s_rot_re - s_nco_im * s_rot_im;
        s_nco_im = s_nco_re * s_rot_im + s_nco_im * s_rot_re;
        s_nco_re = nr;
        float ar, ai, br, bi;
        if (!decim_push(&s_dec1, yr, yi, &ar, &ai)) continue;
        if (!decim_push(&s_dec2, ar, ai, &br, &bi)) continue;
        float mag = sqrtf(br * br + bi * bi);
        s_dc += 0.002f * (mag - s_dc);                        /* carrier level */
        float a = (mag - s_dc) / (s_dc + 0.5f) * 12000.0f;    /* normalised modulation */
        if (a > 32767.0f) a = 32767.0f;
        if (a < -32767.0f) a = -32767.0f;
        pcm[n++] = (int16_t)a;
    }
    /* keep the NCO on the unit circle */
    float mag = sqrtf(s_nco_re * s_nco_re + s_nco_im * s_nco_im);
    s_nco_re /= mag; s_nco_im /= mag;
    if (out && n) ring_write(&s_audio_ring, (const uint8_t *)pcm, (uint32_t)n * 2);
}

/* ------------------------------------------------------------------------- */
/* scan plan: channels -> windows (one FFT) -> segments (one tuner setting)  */
static void rebuild_plan(void)
{
    static struct { uint32_t freq; int16_t last, floor; uint16_t hits; } old[FSCAN_MAX_CH];
    critical_section_enter_blocking(&s_cs);
    uint16_t nold = w_nch;
    for (int i = 0; i < nold; i++) {
        old[i].freq = w_ch[i].freq; old[i].last = w_ch[i].last_x10;
        old[i].floor = w_ch[i].floor_x10; old[i].hits = w_ch[i].hits;
    }
    uint16_t n = 0, k = 0;
    for (int i = 0; i < s_cfg.nch; i++) {
        const store_ch_t *c = &s_cfg.ch[i];
        if (c->flags & CH_SKIP) continue;
        wch_t *w = &w_ch[n++];
        w->freq = c->freq;
        w->sql_x10 = (c->sql_x10 == SQL_DEFAULT) ? s_cfg.sql_x10 : c->sql_x10;
        memcpy(w->label, c->label, FSCAN_LABEL_LEN);
        /* both lists are sorted: carry the measurements over */
        while (k < nold && old[k].freq < c->freq) k++;
        if (k < nold && old[k].freq == c->freq) {
            w->last_x10 = old[k].last; w->floor_x10 = old[k].floor; w->hits = old[k].hits;
        } else {
            w->last_x10 = w->floor_x10 = LEVEL_NONE; w->hits = 0;
        }
    }
    w_nch = n;
    s_plan_gen = s_cfg_gen;
    uint32_t span = (uint32_t)s_cfg.span_khz * 1000u;
    critical_section_exit(&s_cs);

    /* windows */
    w_nwin = 0;
    int i = 0;
    while (i < w_nch && w_nwin < MAX_WIN) {
        int j = i;
        while (j + 1 < w_nch && w_ch[j + 1].freq - w_ch[i].freq <= WIN_SPAN_HZ) j++;
        uint32_t lo = w_ch[i].freq, hi = w_ch[j].freq;
        uint32_t c = lo + (hi - lo) / 2;
        /* move the centre off any channel (DC spike), if the window still fits */
        for (int tries = 0; tries < 6; tries++) {
            bool clash = false;
            for (int q = i; q <= j; q++) {
                int32_t d = (int32_t)(w_ch[q].freq - c);
                if (d < 0) d = -d;
                if ((uint32_t)d < DC_GUARD_HZ) { clash = true; break; }
            }
            if (!clash) break;
            uint32_t cand = c + ((tries & 1) ? -(int32_t)(DC_GUARD_HZ * (tries / 2 + 1) * 2)
                                             :  (int32_t)(DC_GUARD_HZ * (tries / 2 + 1) * 2));
            /* signed: a single-channel window has cand above hi or below lo */
            if (labs((int32_t)(hi - cand)) <= (long)WIN_EDGE_HZ && labs((int32_t)(cand - lo)) <= (long)WIN_EDGE_HZ) c = cand;
        }
        w_win[w_nwin].center = c;
        w_win[w_nwin].first = (uint16_t)i;
        w_win[w_nwin].count = (uint16_t)(j - i + 1);
        w_nwin++;
        i = j + 1;
    }

    /* segments: consecutive windows whose centres lie within SPAN share the tuner */
    w_nseg = 0;
    for (int a = 0; a < w_nwin; ) {
        int b = a;
        while (span && b + 1 < w_nwin && w_win[b + 1].center - w_win[a].center <= span) b++;
        uint32_t lo = w_win[a].center, hi = w_win[b].center;
        w_seg[w_nseg].tuner = lo + (hi - lo) / 2;
        for (int q = a; q <= b; q++) w_win[q].seg = w_nseg;
        w_nseg++;
        a = b + 1;
    }
    if (w_cur >= w_nwin) w_cur = 0;
    printf("[fscan] %u channels in %u windows, %u tuner segments (span %lu kHz)\n",
           w_nch, w_nwin, w_nseg, (unsigned long)(span / 1000));
}

/* ------------------------------------------------------------------------- */
/* peers                                                                     */
static bool peer_busy(uint32_t freq, uint8_t *by)
{
    bool busy = false;
    uint32_t t = now_ms();
    critical_section_enter_blocking(&s_cs);
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].freq && (int32_t)(s_peers[i].expire_ms - t) > 0) {
            int32_t d = (int32_t)(s_peers[i].freq - freq);
            if (d > -1000 && d < 1000) { busy = true; if (by) *by = s_peers[i].id; break; }
        }
    }
    critical_section_exit(&s_cs);
    return busy;
}

void fastscan_peer_mon(uint8_t id, uint32_t freq_hz)
{
    if (id == CFG_BOARD_ID) return;
    uint32_t t = now_ms();
    critical_section_enter_blocking(&s_cs);
    int slot = -1;
    for (int i = 0; i < MAX_PEERS; i++) if (s_peers[i].id == id && s_peers[i].freq) { slot = i; break; }
    if (slot < 0) for (int i = 0; i < MAX_PEERS; i++)
        if (!s_peers[i].freq || (int32_t)(s_peers[i].expire_ms - t) <= 0) { slot = i; break; }
    if (slot >= 0) {
        s_peers[slot].id = id;
        s_peers[slot].freq = freq_hz;
        s_peers[slot].expire_ms = t + 1000;
    }
    critical_section_exit(&s_cs);
}

void fastscan_peer_end(uint8_t id, uint32_t freq_hz)
{
    (void)freq_hz;
    critical_section_enter_blocking(&s_cs);
    for (int i = 0; i < MAX_PEERS; i++)
        if (s_peers[i].id == id) s_peers[i].freq = 0;
    critical_section_exit(&s_cs);
}

/* ------------------------------------------------------------------------- */
/* tuner control                                                             */
static void apply_gain(void)
{
    int16_t g = s_cfg.gain_x10;
    s_gain_pending = false;
    if (g < 0) {
        rtlsdr_set_tuner_gain_mode(0);
        rtlsdr_set_agc_mode(1);
    } else {
        rtlsdr_set_agc_mode(0);
        rtlsdr_set_tuner_gain_mode(1);
        rtlsdr_set_tuner_gain(g);
    }
}

/* open the tuner IF filter for SPAN (retunes the tuner and resets the DDC) */
static void apply_bandwidth(void)
{
    uint32_t span = (uint32_t)s_cfg.span_khz * 1000u;
    s_bw_pending = false;
    if (rtlsdr_set_tuner_bandwidth(span ? span + IFBW_EXTRA_HZ : 0))
        printf("[fscan] tuner bandwidth for span %lu kHz failed\n", (unsigned long)(span / 1000));
    s_tuner_hz = 0;
}

/* move to a window: flush, retune (DDC only if the tuner can stay), restart the stream */
static void hop(uint32_t tuner, uint32_t center)
{
    uint32_t t0 = time_us_32();
    usb_stream_pause_abort();
    r82xx_pll_wait_us = s_cfg.pllwait_us;
    if (s_xfer_pending) { s_xfer_pending = false; usb_stream_set_xfer_len(s_cfg.xfer); }
    if (s_bw_pending) apply_bandwidth();
    if (s_ppm_pending) { s_ppm_pending = false; rtlsdr_set_freq_correction(s_cfg.ppm); s_tuner_hz = 0; }
    if (s_force_tune) { s_force_tune = false; s_tuner_hz = 0; }
    if (s_gain_pending) apply_gain();
    bool retune = (tuner != s_tuner_hz);
    int r = 0;
    if (retune) {
        r = rtlsdr_set_center_freq(tuner);
        s_tuner_hz = r ? 0 : tuner;
    }
    if (!r) r = rtlsdr_set_ddc_offset((int32_t)(center - tuner));
    if (r) st_fail++;
    s_center_hz = r ? 0 : center;
    rtlsdr_reset_buffer();
    usb_stream_reset_toggle();
    ring_reset(&g_shared.iq_ring);             /* nothing in flight: the callback cannot write now */
    usb_stream_resume();

    uint32_t dt = time_us_32() - t0;
    if (retune) { st_tune_n++; st_tune_us += dt; } else { st_ddc_n++; st_ddc_us += dt; }
    if (dt > st_hop_max) st_hop_max = dt;
    uint32_t us = retune ? s_cfg.tsettle_us : s_cfg.settle_us;
    s_discard = (us * (FS * 2 / 1000) / 1000) & ~1u;
    s_hop_ms = now_ms();
}

/* the IQ stream stalled after a hop (the main loop only restarts it on a reported fault) */
static void restart_stream(void)
{
    printf("[fscan] no IQ data for %lu ms, restarting the stream\n", (unsigned long)DATA_TIMEOUT_MS);
    st_fail++;
    usb_stream_stop();
    rtlsdr_reset_buffer();
    usb_stream_reset_toggle();
    ring_reset(&g_shared.iq_ring);
    usb_stream_start();
    s_hop_ms = now_ms();
}

/* ------------------------------------------------------------------------- */
/* receiving                                                                 */
static void monitor_start(uint32_t freq, uint32_t center, int wch, int16_t level, bool manual)
{
    const float pi = 3.14159265358979f;
    m.freq = freq;
    m.center = center;
    m.wch = wch;
    m.manual = manual;
    m.confirmed = false;
    m.gen = s_cfg_gen;
    m.peak_x10 = level;
    m.blocks = 0;
    m.start_ms = m.active_ms = now_ms();
    if (manual) m.active_ms -= s_cfg.hold_ms + SQL_TAIL_MS + 1;   /* TUNE: closed until a signal shows up */
    if (wch >= 0) {
        m.sql_x10 = w_ch[wch].sql_x10;
        m.floor_x10 = w_ch[wch].floor_x10;
        memcpy(m.label, w_ch[wch].label, FSCAN_LABEL_LEN);
    } else {
        m.sql_x10 = s_cfg.sql_x10;
        m.floor_x10 = LEVEL_NONE;
        m.label[0] = 0;
        critical_section_enter_blocking(&s_cs);
        for (int i = 0; i < s_cfg.nch; i++) {
            if (s_cfg.ch[i].freq != freq) continue;
            if (s_cfg.ch[i].sql_x10 != SQL_DEFAULT) m.sql_x10 = s_cfg.ch[i].sql_x10;
            memcpy(m.label, s_cfg.ch[i].label, FSCAN_LABEL_LEN);
        }
        critical_section_exit(&s_cs);
    }
    int32_t off = (int32_t)(freq - center);
    float w = -2.0f * pi * (float)off / (float)FS;
    s_rot_re = cosf(w); s_rot_im = sinf(w);
    demod_reset();
    memset(s_mon_acc, 0, sizeof(s_mon_acc));
    ring_reset(&s_audio_ring);
    s_state = ST_MONITOR;
    if (manual) {
        char f[16];
        fmt_mhz(f, sizeof(f), freq);
        s_mon_freq = freq;
        event("TUNE %s MHz %s", f, m.label);
    }
}

/* the 8 ms average agrees with the scan: a real signal */
static void monitor_confirm(int16_t level)
{
    char f[16], d[16];
    m.confirmed = true;
    m.start_ms = m.active_ms;
    if (m.wch >= 0) w_ch[m.wch].hits++;
    st_hits++;
    s_mon_freq = m.freq;
    fmt_mhz(f, sizeof(f), m.freq);
    fmt_db(d, sizeof(d), level);
    event("HIT %s MHz %s %s dBFS", f, m.label, d);
}

/* a noise peak that the averaged check does not confirm: back to scanning, no HIT / END */
static void monitor_drop(void)
{
    st_false++;
    w_cur = (uint16_t)((w_cur + 1) % (w_nwin ? w_nwin : 1));
    s_state = ST_HOP;
}

static void monitor_end(const char *why)
{
    if (!m.manual && !m.confirmed) {           /* never announced: nothing to end */
        s_state = ST_HOP;
        return;
    }
    /* from the hit to the last moment the squelch was open (HOLD not counted) */
    uint32_t dur = m.manual ? now_ms() - m.start_ms : m.active_ms - m.start_ms;
    char f[16], d[16];
    fmt_mhz(f, sizeof(f), m.freq);
    fmt_db(d, sizeof(d), m.peak_x10);
    event("END %s MHz %s %lu.%lu s peak %s dBFS (%s)", f, m.label, (unsigned long)(dur / 1000),
          (unsigned long)(dur % 1000 / 100), d, why);
    if (!m.manual) {
        critical_section_enter_blocking(&s_cs);
        logent_t *e = &s_log[s_log_n % LOG_LEN];
        e->start_ms = m.start_ms;
        e->dur_ms = dur;
        e->freq = m.freq;
        e->peak_x10 = m.peak_x10;
        memcpy(e->label, m.label, FSCAN_LABEL_LEN);
        s_log_n++;
        critical_section_exit(&s_cs);
        w_cur = (uint16_t)((w_cur + 1) % (w_nwin ? w_nwin : 1));
    }
    s_mon_freq = 0;
    s_state = ST_HOP;
}

/* channel list changed while receiving: follow squelch / label, stop if it was removed */
static bool monitor_revalidate(void)
{
    bool found = false;
    critical_section_enter_blocking(&s_cs);
    for (int i = 0; i < s_cfg.nch; i++) {
        const store_ch_t *c = &s_cfg.ch[i];
        if (c->freq != m.freq || (c->flags & CH_SKIP)) continue;
        m.sql_x10 = (c->sql_x10 == SQL_DEFAULT) ? s_cfg.sql_x10 : c->sql_x10;
        memcpy(m.label, c->label, FSCAN_LABEL_LEN);
        found = true;
    }
    critical_section_exit(&s_cs);
    m.gen = s_cfg_gen;
    return found;
}

static void monitor_task(void)
{
    ring_t *r = &g_shared.iq_ring;
    uint32_t t = now_ms();
    /* at most 2 blocks (2 ms of IQ) per call so that the main loop resubmits USB transfers promptly */
    for (int blocks = 0; blocks < 2; blocks++) {
        if (ring_used(r) < sizeof(s_iq)) break;
        ring_read(r, s_iq, sizeof(s_iq));
        /* audio only while the squelch is open (plus a short tail); HOLD just keeps the channel */
        demod_block(s_iq, (m.manual || m.confirmed) && t - m.active_ms <= SQL_TAIL_MS);
        /* squelch check every 8 blocks (~8 ms), averaged over them */
        block_to_spectrum(s_iq, s_mon_acc);
        if ((++m.blocks & 7) == 0) {
            int16_t lv = channel_level(s_mon_acc, 8, (int32_t)(m.freq - m.center), s_cfg.bw_hz);
            memset(s_mon_acc, 0, sizeof(s_mon_acc));
            if (m.wch >= 0) w_ch[m.wch].last_x10 = lv;
            if (level_open(lv, m.sql_x10, m.floor_x10)) {
                m.active_ms = t;
                if (lv > m.peak_x10) m.peak_x10 = lv;
                if (!m.manual && !m.confirmed) monitor_confirm(lv);
            }
        }
    }
    if (!m.manual && !m.confirmed) {
        if (t - m.start_ms > CONFIRM_MS) monitor_drop();
        return;
    }
    uint32_t tune = s_tune_freq;
    if (m.manual) {
        if (tune != m.freq) monitor_end(tune ? "retune" : "tune off");
        else if (s_cfg_gen != m.gen) monitor_revalidate();
        return;
    }
    uint8_t by = 0;
    if (tune) { monitor_end("tune"); return; }
    if (peer_busy(m.freq, &by) && by < CFG_BOARD_ID) { monitor_end("peer has priority"); return; }
    if (!s_cfg.scan_on) { monitor_end("scan off"); return; }
    if (s_cfg_gen != m.gen && !monitor_revalidate()) { monitor_end("channel removed"); return; }
    if (t - m.active_ms > s_cfg.hold_ms) monitor_end("signal gone");
}

/* ------------------------------------------------------------------------- */
void fastscan_init(void)
{
    critical_section_init(&s_cs);
    ring_init(&s_audio_ring, s_audio_buf, sizeof(s_audio_buf));
    ring_init(&s_ev_ring, s_ev_buf, sizeof(s_ev_buf));
    dsp_init();
    load_settings();
    s_pllwait_default = r82xx_pll_wait_us;
    printf("[fscan] board %d, control TCP %d, coord UDP %d, audio UDP %d\n", CFG_BOARD_ID,
           FSCAN_CTRL_PORT, FSCAN_COORD_PORT, FSCAN_AUDIO_PORT_BASE + CFG_BOARD_ID);
}

void fastscan_start(void)
{
    s_suspended = false;
    if (!g_shared.sdr_ready || usb_host_state() != USBH_DEVICE_OPEN) return;
    usb_stream_stop();
    r82xx_pll_wait_us = s_cfg.pllwait_us;
    rtlsdr_keep_i2c_repeater = 1;
    /* undo what an rtl_tcp client may have changed */
    if (rtlsdr_get_direct_sampling()) rtlsdr_set_direct_sampling(0);
    rtlsdr_set_sample_rate(FS);
    rtlsdr_set_freq_correction(s_cfg.ppm);
    apply_bandwidth();
    apply_gain();
    s_xfer_pending = s_ppm_pending = false;
    usb_stream_set_xfer_len(s_cfg.xfer);
    rtlsdr_reset_buffer();
    usb_stream_reset_toggle();
    ring_reset(&g_shared.iq_ring);
    usb_stream_start();
    s_tuner_hz = s_center_hz = 0;
    s_mon_freq = 0;
    s_plan_gen = 0;                            /* rebuild */
    s_active = true;
    s_state = ST_IDLE;
    event("SCAN start (%s)", s_cfg.scan_on ? "on" : "off");
}

bool fastscan_suspend(void)
{
    s_suspended = true;
    if (!s_active) return false;
    if (s_state == ST_MONITOR) monitor_end("rtl_tcp client");
    s_active = false;
    s_state = ST_OFF;
    /* hand the tuner back the way the rtl_tcp server knows it */
    usb_stream_stop();
    usb_stream_set_xfer_len(CFG_USB_XFER_BYTES);
    rtlsdr_keep_i2c_repeater = 0;
    r82xx_pll_wait_us = s_pllwait_default;
    rtlsdr_set_tuner_bandwidth(0);
    s_tuner_hz = s_center_hz = 0;
    event("SCAN paused: rtl_tcp client connected");
    return true;
}

void fastscan_on_sdr_lost(void)
{
    if (s_active) printf("[fscan] SDR lost\n");
    s_active = false;
    s_mon_freq = 0;
    s_state = ST_OFF;
    usb_stream_set_xfer_len(CFG_USB_XFER_BYTES);
    rtlsdr_keep_i2c_repeater = 0;
    r82xx_pll_wait_us = s_pllwait_default;
}

bool     fastscan_monitoring(void)   { return s_active && s_mon_freq != 0; }
uint32_t fastscan_monitor_freq(void) { return s_mon_freq; }
ring_t  *fastscan_audio_ring(void)   { return &s_audio_ring; }
ring_t  *fastscan_event_ring(void)   { return &s_ev_ring; }

void fastscan_task(void)
{
    if (!s_active) return;
    if (s_state == ST_MONITOR) { monitor_task(); return; }

    ring_t *r = &g_shared.iq_ring;
    if (s_plan_gen != s_cfg_gen) {
        rebuild_plan();
        if (s_state == ST_SETTLE || s_state == ST_MEASURE) s_state = ST_HOP;   /* window may have moved */
    }
    if ((s_state == ST_SETTLE || s_state == ST_MEASURE) && now_ms() - s_hop_ms > DATA_TIMEOUT_MS)
        restart_stream();
    uint32_t tune = s_tune_freq;

    switch (s_state) {
    case ST_IDLE:
        ring_reset(r);                         /* nobody reads the IQ: keep the ring empty */
        if (tune || (s_cfg.scan_on && w_nwin)) s_state = ST_HOP;
        break;

    case ST_HOP: {
        if (tune) {
            s_manual = true;
            s_manual_freq = tune;
            hop(tune - TUNE_OFFSET_HZ, tune - TUNE_OFFSET_HZ);
            s_state = ST_SETTLE;
            break;
        }
        s_manual = false;
        if (!s_cfg.scan_on || !w_nwin) { s_state = ST_IDLE; break; }
        /* skip windows whose channels are all received by other units */
        for (int n = 0; n < w_nwin; n++) {
            const win_t *w = &w_win[w_cur];
            bool any = false;
            for (int k = 0; k < w->count && !any; k++) any = !peer_busy(w_ch[w->first + k].freq, NULL);
            if (any) break;
            w_cur = (uint16_t)((w_cur + 1) % w_nwin);
        }
        const win_t *w = &w_win[w_cur];
        uint32_t tuner = w_seg[w->seg].tuner;
        bool pending = s_bw_pending || s_gain_pending || s_force_tune || s_xfer_pending || s_ppm_pending;
        if (w->center == s_center_hz && tuner == s_tuner_hz && !pending) {
            s_discard = 0;                     /* only one window: just keep measuring */
            s_hop_ms = now_ms();
        } else {
            hop(tuner, w->center);
        }
        s_state = ST_SETTLE;
        break;
    }

    case ST_SETTLE:
        while (s_discard) {
            uint32_t n = s_discard > sizeof(s_junk) ? sizeof(s_junk) : s_discard;
            uint32_t got = ring_read(r, s_junk, n);
            if (!got) return;
            s_discard -= got;
        }
        if (!s_center_hz) {                    /* the hop failed: go on with the next window */
            if (!s_manual && w_nwin) w_cur = (uint16_t)((w_cur + 1) % w_nwin);
            s_state = ST_HOP;
            break;
        }
        if (s_manual) {
            monitor_start(s_manual_freq, s_center_hz, -1, LEVEL_NONE, true);
            break;
        }
        memset(s_acc, 0, sizeof(s_acc));
        s_nfft = 0;
        s_state = ST_MEASURE;
        break;

    case ST_MEASURE: {
        if (ring_used(r) < sizeof(s_iq)) return;
        ring_read(r, s_iq, sizeof(s_iq));
        block_to_spectrum(s_iq, s_acc);
        if (++s_nfft < s_cfg.navg) return;

        const win_t *w = &w_win[w_cur];
        int best = -1;
        int16_t best_lv = INT16_MIN;
        for (int k = 0; k < w->count; k++) {
            int idx = w->first + k;
            wch_t *c = &w_ch[idx];
            int16_t lv = channel_level(s_acc, s_nfft, (int32_t)(c->freq - w->center), s_cfg.bw_hz);
            bool open = level_open(lv, c->sql_x10, c->floor_x10);
            c->last_x10 = lv;
            track_floor(c, lv);
            if (open && lv > best_lv && !peer_busy(c->freq, NULL)) { best = idx; best_lv = lv; }
        }
        st_windows++;
        st_checks += w->count;
        if (best >= 0) {
            monitor_start(w_ch[best].freq, w->center, best, best_lv, false);
        } else {
            w_cur = (uint16_t)((w_cur + 1) % w_nwin);
            s_state = ST_HOP;
        }
        break;
    }

    default:
        break;
    }
}

void fastscan_print_stats(void)
{
    static uint32_t last_ms, last_win, last_chk, last_hits, last_false;
    static uint32_t last_dn, last_dus, last_tn, last_tus;
    uint32_t t = now_ms(), dt = t - last_ms;
    if (!dt) return;
    uint32_t dn = st_ddc_n - last_dn, tn = st_tune_n - last_tn;
    rt_win_s   = (st_windows - last_win) * 1000 / dt;
    rt_ch_s    = (st_checks - last_chk) * 1000 / dt;
    rt_ddc_us  = dn ? (st_ddc_us - last_dus) / dn : 0;
    rt_tune_us = tn ? (st_tune_us - last_tus) / tn : 0;
    rt_tune_s  = tn * 1000 / dt;
    if (s_active)
        printf("[fscan] %s  win/s %lu  ch/s %lu  hop: ddc %lu us, tuner %lu us (%lu/s), max %lu us  fail %lu  hits %lu  false %lu  audio %lu B  ev drop %lu\n",
               st_name[s_state], (unsigned long)rt_win_s, (unsigned long)rt_ch_s,
               (unsigned long)rt_ddc_us, (unsigned long)rt_tune_us, (unsigned long)rt_tune_s,
               (unsigned long)st_hop_max, (unsigned long)st_fail, (unsigned long)(st_hits - last_hits),
               (unsigned long)(st_false - last_false), (unsigned long)ring_used(&s_audio_ring),
               (unsigned long)s_ev_dropped);
    last_ms = t; last_win = st_windows; last_chk = st_checks; last_hits = st_hits; last_false = st_false;
    last_dn = st_ddc_n; last_dus = st_ddc_us; last_tn = st_tune_n; last_tus = st_tune_us;
    st_hop_max = 0;
}

/* ------------------------------------------------------------------------- */
/* control commands (core1)                                                  */
static bool parse_mhz(const char *s, uint32_t *hz)
{
    uint32_t ip = 0, fp = 0, scale = 1000000;
    bool any = false;
    while (*s >= '0' && *s <= '9') { ip = ip * 10 + (uint32_t)(*s++ - '0'); any = true; }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (scale > 1) { scale /= 10; fp += (uint32_t)(*s - '0') * scale; }
            s++; any = true;
        }
    }
    if (!any || *s || ip > 1766) return false;
    uint32_t v = ip * 1000000u + fp;
    if (v < 24000000u || v > 1766000000u) return false;
    *hz = v;
    return true;
}

static bool parse_x10(const char *s, int32_t *out)
{
    int sign = 1;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    int32_t ip = 0, fp = 0;
    bool any = false;
    while (*s >= '0' && *s <= '9' && ip < 100000) { ip = ip * 10 + (*s++ - '0'); any = true; }
    if (*s == '.') { s++; if (*s >= '0' && *s <= '9') { fp = *s++ - '0'; any = true; } while (*s >= '0' && *s <= '9') s++; }
    if (!any || *s) return false;
    *out = sign * (ip * 10 + fp);
    return true;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    char *e;
    unsigned long v = strtoul(s, &e, 10);
    if (!*s || *e) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_ip(const char *s, uint8_t ip[4])
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    ip[0] = (uint8_t)a; ip[1] = (uint8_t)b; ip[2] = (uint8_t)c; ip[3] = (uint8_t)d;
    return true;
}

/* copy a label, cutting at a UTF-8 character boundary */
static void copy_label(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n > FSCAN_LABEL_LEN - 1) {
        n = FSCAN_LABEL_LEN - 1;
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
    }
    memcpy(dst, src, n);
    memset(dst + n, 0, FSCAN_LABEL_LEN - n);
}

static void upcase(char *s) { for (; *s; s++) if (*s >= 'a' && *s <= 'z') *s -= 32; }

void fastscan_set_client_ip(const uint8_t ip[4])
{
    critical_section_enter_blocking(&s_cs);
    memcpy(s_client_ip, ip, 4);
    s_client_valid = true;
    critical_section_exit(&s_cs);
}

bool fastscan_audio_auto(void) { return s_cfg.audio_auto; }

bool fastscan_audio_dest(uint8_t ip[4], uint16_t *port)
{
    bool ok = false;
    critical_section_enter_blocking(&s_cs);
    if (s_cfg.audio_auto) {
        if (s_client_valid) { memcpy(ip, s_client_ip, 4); ok = true; }
    } else if (s_cfg.audio_ip[0]) {
        memcpy(ip, s_cfg.audio_ip, 4); ok = true;
    }
    critical_section_exit(&s_cs);
    *port = FSCAN_AUDIO_PORT_BASE + CFG_BOARD_ID;
    return ok;
}

static int find_ch(uint32_t hz)
{
    for (int i = 0; i < s_cfg.nch; i++) if (s_cfg.ch[i].freq == hz) return i;
    return -1;
}

static void cfg_changed(void) { s_cfg_gen++; }

static void emit_status(fscan_emit_fn emit)
{
    char b[220], d1[16], d2[16], d3[16];
    fmt_db(d1, sizeof(d1), s_cfg.sql_x10);
    if (s_cfg.gain_x10 < 0) snprintf(d2, sizeof(d2), "AUTO"); else fmt_db(d2, sizeof(d2), s_cfg.gain_x10);
    const char *st = s_suspended ? "rtl_tcp client (scan paused)" : s_active ? st_name[s_state] : "no SDR";
    snprintf(b, sizeof(b), "board %d  scan %s  state %s  tune %s\n", CFG_BOARD_ID,
             s_cfg.scan_on ? "ON" : "OFF", st, s_tune_freq ? "ON" : "off");
    emit(b);
    snprintf(b, sizeof(b), "sql %s dBFS  snr %u dB  hold %u ms  gain %s  ppm %d  bw %u Hz\n",
             d1, s_cfg.snr_db, s_cfg.hold_ms, d2, s_cfg.ppm, s_cfg.bw_hz);
    emit(b);
    snprintf(b, sizeof(b), "span %u kHz  settle %u us  tsettle %u us  pllwait %u us  navg %u  xfer %u B\n",
             s_cfg.span_khz, s_cfg.settle_us, s_cfg.tsettle_us, s_cfg.pllwait_us, s_cfg.navg, s_cfg.xfer);
    emit(b);
    uint32_t ws = rt_win_s;
    snprintf(b, sizeof(b), "%u channels, %u windows, %u tuner segments  speed %lu win/s, %lu ch/s  one pass %lu ms\n",
             w_nch, w_nwin, w_nseg, (unsigned long)ws, (unsigned long)rt_ch_s,
             (unsigned long)(ws ? (uint32_t)w_nwin * 1000u / ws : 0));
    emit(b);
    snprintf(b, sizeof(b), "hop: ddc %lu us, tuner %lu us (%lu/s)  fail %lu  hits %lu  unconfirmed %lu\n",
             (unsigned long)rt_ddc_us, (unsigned long)rt_tune_us, (unsigned long)rt_tune_s,
             (unsigned long)st_fail, (unsigned long)st_hits, (unsigned long)st_false);
    emit(b);
    fastscan_net_status(emit);
    uint32_t mf = s_mon_freq;
    if (mf) {
        fmt_mhz(d3, sizeof(d3), mf);
        snprintf(b, sizeof(b), "receiving %s MHz %s\n", d3, m.label);
        emit(b);
    }
}

static void emit_list(fscan_emit_fn emit)
{
    char out[160], f[16], sql[16], lv[16], fl[16];
    int n = s_cfg.nch;
    for (int i = 0; i < n; i++) {
        critical_section_enter_blocking(&s_cs);
        if (i >= s_cfg.nch) { critical_section_exit(&s_cs); break; }
        store_ch_t c = s_cfg.ch[i];
        int16_t last = LEVEL_NONE, floor = LEVEL_NONE;
        uint16_t hits = 0;
        for (int k = 0; k < w_nch; k++) {
            if (w_ch[k].freq != c.freq) continue;
            last = w_ch[k].last_x10; floor = w_ch[k].floor_x10; hits = w_ch[k].hits;
            break;
        }
        critical_section_exit(&s_cs);
        fmt_mhz(f, sizeof(f), c.freq);
        if (c.sql_x10 == SQL_DEFAULT) snprintf(sql, sizeof(sql), "default"); else fmt_db(sql, sizeof(sql), c.sql_x10);
        fmt_db(lv, sizeof(lv), last);
        fmt_db(fl, sizeof(fl), floor);
        snprintf(out, sizeof(out), "%3d  %s MHz %s sql %-7s  last %-6s floor %-6s hits %-4u %s\n", i + 1, f,
                 (c.flags & CH_SKIP) ? "SKIP" : "    ", sql, lv, fl, hits, c.label);
        emit(out);
    }
    snprintf(out, sizeof(out), "%d channels\n", n);
    emit(out);
}

static void emit_log(fscan_emit_fn emit)
{
    char out[160], f[16], d[16];
    uint32_t t = now_ms();
    critical_section_enter_blocking(&s_cs);
    uint32_t total = s_log_n;
    critical_section_exit(&s_cs);
    uint32_t first = total > LOG_LEN ? total - LOG_LEN : 0;
    for (uint32_t i = first; i < total; i++) {
        critical_section_enter_blocking(&s_cs);
        logent_t e = s_log[i % LOG_LEN];
        critical_section_exit(&s_cs);
        uint32_t ago = (t - e.start_ms) / 1000;
        fmt_mhz(f, sizeof(f), e.freq);
        fmt_db(d, sizeof(d), e.peak_x10);
        snprintf(out, sizeof(out), "%2lu:%02lu:%02lu ago  %s MHz  %3lu.%lu s  peak %-6s %s\n",
                 (unsigned long)(ago / 3600), (unsigned long)(ago / 60 % 60), (unsigned long)(ago % 60), f,
                 (unsigned long)(e.dur_ms / 1000), (unsigned long)(e.dur_ms % 1000 / 100), d, e.label);
        emit(out);
    }
    snprintf(out, sizeof(out), "%lu hits since boot\n", (unsigned long)total);
    emit(out);
}

/* SKIP / UNSKIP one channel */
static void set_skip(uint32_t hz, bool skip, fscan_emit_fn emit)
{
    critical_section_enter_blocking(&s_cs);
    int i = find_ch(hz);
    if (i >= 0) {
        if (skip) s_cfg.ch[i].flags |= CH_SKIP; else s_cfg.ch[i].flags &= (uint8_t)~CH_SKIP;
        cfg_changed();
    }
    critical_section_exit(&s_cs);
    emit(i >= 0 ? "OK\n" : "ERR not found\n");
}

void fastscan_ctl(const char *line, fscan_emit_fn emit)
{
    char buf[160], *save = NULL;
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *cmd = strtok_r(buf, " \t\r\n", &save);
    if (!cmd || cmd[0] == '#') return;
    upcase(cmd);
    char *a1 = strtok_r(NULL, " \t\r\n", &save);
    char *a2 = strtok_r(NULL, " \t\r\n", &save);
    char *a3 = strtok_r(NULL, " \t\r\n", &save);
    char out[160], f[16];
    uint32_t hz, u;
    int32_t x;

    if (!strcmp(cmd, "HELP")) {
        emit("ADD <MHz> [label] [-sql]  DEL <MHz>  SKIP <MHz>  UNSKIP <MHz>  CLEAR  LIST  STAT  LOG\n"
             "SQL <dBFS>  SNR <dB>|0  HOLD <ms>  GAIN <dB>|AUTO  PPM <n>  BW <Hz>\n"
             "SPAN <kHz>|0  SETTLE <us>  TSETTLE <us>  PLLWAIT <us>  NAVG <1-8>  XFER <bytes>\n"
             "AUDIO <ip>|AUTO|OFF  SCAN ON|OFF  TUNE <MHz>|OFF  WATCH ON|OFF  SAVE  QUIT\n");
    } else if (!strcmp(cmd, "ADD") && a1 && parse_mhz(a1, &hz)) {
        /* optional arguments in any order: a squelch starts with '-' or '+', anything else is the label */
        const char *label = NULL;
        bool has_sql = false;
        int16_t sql = SQL_DEFAULT;
        char *args[2] = { a2, a3 };
        for (int k = 0; k < 2; k++) {
            if (!args[k]) continue;
            if (args[k][0] == '-' || args[k][0] == '+') {
                if (!parse_x10(args[k], &x) || x < -1500 || x > 0) { emit("ERR sql\n"); return; }
                sql = (int16_t)x; has_sql = true;
            } else {
                label = args[k];
            }
        }
        critical_section_enter_blocking(&s_cs);
        int i = find_ch(hz);
        bool ok = true;
        if (i < 0) {
            if (s_cfg.nch >= FSCAN_MAX_CH) ok = false;
            else {
                int pos = s_cfg.nch;
                while (pos > 0 && s_cfg.ch[pos - 1].freq > hz) { s_cfg.ch[pos] = s_cfg.ch[pos - 1]; pos--; }
                memset(&s_cfg.ch[pos], 0, sizeof(store_ch_t));
                s_cfg.ch[pos].freq = hz;
                s_cfg.ch[pos].sql_x10 = SQL_DEFAULT;
                s_cfg.nch++;
                i = pos;
            }
        }
        if (ok) {
            if (has_sql) s_cfg.ch[i].sql_x10 = sql;
            if (label) copy_label(s_cfg.ch[i].label, label);
            cfg_changed();
        }
        critical_section_exit(&s_cs);
        fmt_mhz(f, sizeof(f), hz);
        snprintf(out, sizeof(out), ok ? "OK %s\n" : "ERR full (%s)\n", f);
        emit(out);
    } else if (!strcmp(cmd, "DEL") && a1 && parse_mhz(a1, &hz)) {
        critical_section_enter_blocking(&s_cs);
        int i = find_ch(hz);
        if (i >= 0) {
            for (int k = i; k + 1 < s_cfg.nch; k++) s_cfg.ch[k] = s_cfg.ch[k + 1];
            s_cfg.nch--;
            cfg_changed();
        }
        critical_section_exit(&s_cs);
        emit(i >= 0 ? "OK\n" : "ERR not found\n");
    } else if (!strcmp(cmd, "SKIP") && a1 && parse_mhz(a1, &hz)) {
        set_skip(hz, true, emit);
    } else if (!strcmp(cmd, "UNSKIP") && a1 && parse_mhz(a1, &hz)) {
        set_skip(hz, false, emit);
    } else if (!strcmp(cmd, "CLEAR")) {
        critical_section_enter_blocking(&s_cs);
        s_cfg.nch = 0;
        cfg_changed();
        critical_section_exit(&s_cs);
        emit("OK\n");
    } else if (!strcmp(cmd, "LIST")) {
        emit_status(emit);
        emit_list(emit);
    } else if (!strcmp(cmd, "STAT")) {
        emit_status(emit);
    } else if (!strcmp(cmd, "LOG")) {
        emit_log(emit);
    } else if (!strcmp(cmd, "SQL") && a1 && parse_x10(a1, &x) && x >= -1500 && x <= 0) {
        s_cfg.sql_x10 = (int16_t)x; cfg_changed(); emit("OK\n");
    } else if (!strcmp(cmd, "SNR") && a1 && parse_u32(a1, &u) && u <= 40) {
        s_cfg.snr_db = (uint8_t)u; emit("OK\n");
    } else if (!strcmp(cmd, "HOLD") && a1 && parse_u32(a1, &u) && u <= 60000) {
        s_cfg.hold_ms = (uint16_t)u; emit("OK\n");
    } else if (!strcmp(cmd, "GAIN") && a1) {
        upcase(a1);
        if (!strcmp(a1, "AUTO")) s_cfg.gain_x10 = -1;
        else if (parse_x10(a1, &x) && x >= 0 && x <= 500) s_cfg.gain_x10 = (int16_t)x;
        else { emit("ERR gain\n"); return; }
        s_gain_pending = true; emit("OK\n");
    } else if (!strcmp(cmd, "PPM") && a1 && parse_x10(a1, &x) && x % 10 == 0 && x >= -2000 && x <= 2000) {
        s_cfg.ppm = (int16_t)(x / 10); s_ppm_pending = true; emit("OK\n");
    } else if (!strcmp(cmd, "SPAN") && a1 && parse_u32(a1, &u) && (u == 0 || (u >= 200 && u <= 4000))) {
        critical_section_enter_blocking(&s_cs);
        s_cfg.span_khz = (uint16_t)u;
        cfg_changed();
        critical_section_exit(&s_cs);
        s_bw_pending = true; emit("OK\n");
    } else if (!strcmp(cmd, "SETTLE") && a1 && parse_u32(a1, &u) && u <= 20000) {
        s_cfg.settle_us = (uint16_t)u; emit("OK\n");
    } else if (!strcmp(cmd, "TSETTLE") && a1 && parse_u32(a1, &u) && u <= 20000) {
        s_cfg.tsettle_us = (uint16_t)u; emit("OK\n");
    } else if (!strcmp(cmd, "NAVG") && a1 && parse_u32(a1, &u) && u >= 1 && u <= 8) {
        s_cfg.navg = (uint16_t)u; emit("OK\n");
    } else if (!strcmp(cmd, "BW") && a1 && parse_u32(a1, &u) && u >= 1000 && u <= 25000) {
        s_cfg.bw_hz = (uint16_t)u; emit("OK\n");
    } else if (!strcmp(cmd, "PLLWAIT") && a1 && parse_u32(a1, &u) && u >= 200 && u <= 20000) {
        s_cfg.pllwait_us = (uint16_t)u; s_force_tune = true; emit("OK\n");
    } else if (!strcmp(cmd, "XFER") && a1 && parse_u32(a1, &u) && u >= 256 && u <= CFG_USB_XFER_BYTES && u % 64 == 0) {
        s_cfg.xfer = (uint16_t)u; s_xfer_pending = true; emit("OK\n");
    } else if (!strcmp(cmd, "AUDIO") && a1) {
        uint8_t ip[4];
        upcase(a1);
        critical_section_enter_blocking(&s_cs);
        bool ok = true;
        if (!strcmp(a1, "AUTO")) s_cfg.audio_auto = 1;
        else if (!strcmp(a1, "OFF")) { s_cfg.audio_auto = 0; memset(s_cfg.audio_ip, 0, 4); }
        else if (parse_ip(a1, ip)) { s_cfg.audio_auto = 0; memcpy(s_cfg.audio_ip, ip, 4); }
        else ok = false;
        critical_section_exit(&s_cs);
        emit(ok ? "OK\n" : "ERR ip\n");
    } else if (!strcmp(cmd, "SCAN") && a1) {
        upcase(a1);
        if (!strcmp(a1, "ON")) { s_cfg.scan_on = 1; s_tune_freq = 0; }
        else if (!strcmp(a1, "OFF")) s_cfg.scan_on = 0;
        else { emit("ERR\n"); return; }
        emit("OK\n");
    } else if (!strcmp(cmd, "TUNE") && a1) {
        upcase(a1);
        if (!strcmp(a1, "OFF")) s_tune_freq = 0;
        else if (parse_mhz(a1, &hz)) s_tune_freq = hz;
        else { emit("ERR freq\n"); return; }
        emit("OK\n");
    } else if (!strcmp(cmd, "SAVE")) {
        emit(save_settings() ? "OK saved\n" : "ERR flash verify\n");
    } else {
        snprintf(out, sizeof(out), "ERR unknown or bad argument: %s (HELP for commands)\n", cmd);
        emit(out);
    }
}

#endif /* CFG_FASTSCAN */

/*
 * scanner.c -- on-board multi-channel scanner (experimental unit)
 *
 * Channels that lie within ~180 kHz of each other are grouped into one
 * "window": the tuner is retuned once per window and all channels in it are
 * measured at the same time from a 256 point FFT (250 kS/s -> 977 Hz bins).
 * A channel above its squelch level is monitored: the IQ stream is mixed down,
 * decimated by 20 (12.5 kHz) and AM demodulated into the audio ring, which
 * core1 sends by UDP.  Several units share which channel they are monitoring
 * (UDP broadcast) so that the others skip it.
 */
#include "config.h"
#if CFG_SCANNER

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"

#include "scanner.h"
#include "rtl_tcp.h"
#include "usb_host.h"
#include "rtl2832u.h"

extern unsigned r82xx_pll_wait_us;          /* tuner_r82xx.c */
extern int      rtlsdr_keep_i2c_repeater;   /* rtl2832u.c */

/* ------------------------------------------------------------------------- */
#define FS            250000u
#define NFFT          256
#define BIN_HZ        ((float)FS / NFFT)
#define WIN_SPAN_HZ   180000u              /* max distance first..last channel in a window */
#define WIN_EDGE_HZ   95000u               /* max |channel - centre| */
#define DC_GUARD_HZ   6000u                /* keep channels away from the DC spike */
#define DECIM         20                   /* 250 kS/s -> 12.5 kS/s audio */
#define MAX_WIN       SCAN_MAX_CH
#define MAX_PEERS     8
#define FLASH_OFF     0x3F0000u            /* settings sector (OTA stage is 0x200000..0x2FFFFF) */
#define STORE_MAGIC   0x4E435352u          /* 'RSCN' */
#define STORE_VERSION 1
#define SQL_DEFAULT   INT16_MIN            /* channel uses the global squelch */

typedef struct {
    uint32_t freq;
    int16_t  sql_x10;
    int16_t  pad;
} store_ch_t;

typedef struct {
    uint32_t   magic;
    uint32_t   version;
    int16_t    sql_x10;        /* global squelch, dBFS x10 */
    uint16_t   hold_ms;        /* keep monitoring this long after the signal drops */
    int16_t    gain_x10;       /* tuner gain dB x10, -1 = AGC */
    uint16_t   settle_us;      /* data discarded after a retune */
    uint16_t   navg;           /* FFTs averaged per window */
    uint16_t   bw_hz;          /* detection bandwidth per channel */
    uint16_t   pllwait_us;     /* R820T PLL lock wait */
    uint8_t    audio_ip[4];
    uint8_t    audio_auto;     /* 1 = send audio to the last control client */
    uint8_t    scan_on;
    uint8_t    keep_rep;       /* keep the I2C repeater on (fewer USB transfers per retune) */
    uint8_t    pad;
    uint16_t   nch;
    uint16_t   pad2;
    store_ch_t ch[SCAN_MAX_CH];
} store_t;

static store_t            s_cfg;            /* shared, protected by s_cs */
static int16_t            s_last_x10[SCAN_MAX_CH];
static critical_section_t s_cs;
static volatile bool      s_dirty = true;   /* channel list changed -> rebuild windows */
static volatile bool      s_gain_pending = false;
static uint8_t            s_client_ip[4];
static volatile bool      s_client_valid = false;

/* peers (other units) */
typedef struct { uint32_t freq; uint32_t expire_ms; uint8_t id; } peer_t;
static peer_t s_peers[MAX_PEERS];

/* audio ring (core0 -> core1) */
static uint8_t s_audio_buf[16384];
static ring_t  s_audio_ring;

/* ------------------------------------------------------------------------- */
/* core0 working copy                                                        */
typedef struct { uint32_t freq; int16_t sql_x10; } wch_t;
typedef struct { uint32_t center; uint16_t first; uint16_t count; } win_t;

static wch_t    w_ch[SCAN_MAX_CH];
static uint16_t w_nch;
static win_t    w_win[MAX_WIN];
static uint16_t w_nwin;
static uint16_t w_cur;

typedef enum { ST_OFF = 0, ST_IDLE, ST_RETUNE, ST_SETTLE, ST_MEASURE, ST_MONITOR } scan_state_t;
static const char *const st_name[] = { "off", "idle", "retune", "settle", "measure", "monitor" };
static scan_state_t s_state = ST_OFF;
static bool     s_sdr = false;
static uint32_t s_discard;
static uint16_t s_nfft;
static float    s_acc[NFFT];

/* monitor */
static volatile uint32_t s_mon_freq = 0;
static int      s_mon_ch = -1;
static uint32_t s_mon_center;
static uint32_t s_mon_start_ms, s_mon_active_ms;
static float    s_nco_re, s_nco_im, s_rot_re, s_rot_im;
static float    s_dec_re, s_dec_im, s_dc;
static int      s_dec_n;
static uint16_t s_mon_blocks;
static float    s_mon_acc[NFFT];
static uint8_t  s_junk[512];

/* stats */
static uint32_t st_windows, st_checks, st_hits, st_retune_us, st_retune_max, st_retune_fail;

/* DSP tables */
static float s_hann[NFFT], s_tw_re[NFFT / 2], s_tw_im[NFFT / 2];
static uint8_t s_rev[NFFT];
static float s_re[NFFT], s_im[NFFT];
static uint8_t s_iq[NFFT * 2];

/* ------------------------------------------------------------------------- */
static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

static void set_defaults(void)
{
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_cfg.magic      = STORE_MAGIC;
    s_cfg.version    = STORE_VERSION;
    s_cfg.sql_x10    = -530;
    s_cfg.hold_ms    = 3000;
    s_cfg.gain_x10   = 402;
    s_cfg.settle_us  = 3000;
    s_cfg.navg       = 2;
    s_cfg.bw_hz      = 6000;
    s_cfg.pllwait_us = 2000;
    s_cfg.audio_auto = 1;
    s_cfg.scan_on    = 1;
    s_cfg.keep_rep   = 1;
    s_cfg.nch        = 1;
    s_cfg.ch[0].freq = 118100000u;
    s_cfg.ch[0].sql_x10 = SQL_DEFAULT;
}

static void load_settings(void)
{
    const store_t *f = (const store_t *)(XIP_BASE + FLASH_OFF);
    if (f->magic == STORE_MAGIC && f->version == STORE_VERSION && f->nch <= SCAN_MAX_CH) {
        memcpy(&s_cfg, f, sizeof(s_cfg));
        printf("[scan] settings loaded from flash: %u channels\n", s_cfg.nch);
    } else {
        set_defaults();
        printf("[scan] no saved settings, using defaults\n");
    }
    for (int i = 0; i < SCAN_MAX_CH; i++) s_last_x10[i] = -1500;
}

/* called on core1: core0 is locked out while the flash is written */
static bool save_settings(void)
{
    static uint8_t page[FLASH_SECTOR_SIZE] __attribute__((aligned(4)));
    _Static_assert(sizeof(store_t) <= FLASH_SECTOR_SIZE, "store too large");
    memset(page, 0xFF, sizeof(page));
    critical_section_enter_blocking(&s_cs);
    memcpy(page, &s_cfg, sizeof(s_cfg));
    critical_section_exit(&s_cs);
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_OFF, FLASH_SECTOR_SIZE);
    flash_range_program(FLASH_OFF, page, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
    return memcmp((const void *)(XIP_BASE + FLASH_OFF), page, sizeof(s_cfg)) == 0;
}

/* ------------------------------------------------------------------------- */
/* FFT                                                                       */
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

/* ------------------------------------------------------------------------- */
/* windows                                                                   */
static void rebuild_windows(void)
{
    critical_section_enter_blocking(&s_cs);
    w_nch = s_cfg.nch;
    for (int i = 0; i < w_nch; i++) {
        w_ch[i].freq = s_cfg.ch[i].freq;
        w_ch[i].sql_x10 = (s_cfg.ch[i].sql_x10 == SQL_DEFAULT) ? s_cfg.sql_x10 : s_cfg.ch[i].sql_x10;
    }
    s_dirty = false;
    critical_section_exit(&s_cs);

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
            for (int k = i; k <= j; k++) {
                int32_t d = (int32_t)(w_ch[k].freq - c);
                if (d < 0) d = -d;
                if ((uint32_t)d < DC_GUARD_HZ) { clash = true; break; }
            }
            if (!clash) break;
            uint32_t cand = c + ((tries & 1) ? -(int32_t)(DC_GUARD_HZ * (tries / 2 + 1) * 2)
                                             :  (int32_t)(DC_GUARD_HZ * (tries / 2 + 1) * 2));
            if (hi - cand <= WIN_EDGE_HZ && cand - lo <= WIN_EDGE_HZ) c = cand;
        }
        w_win[w_nwin].center = c;
        w_win[w_nwin].first = (uint16_t)i;
        w_win[w_nwin].count = (uint16_t)(j - i + 1);
        w_nwin++;
        i = j + 1;
    }
    if (w_cur >= w_nwin) w_cur = 0;
    printf("[scan] %u channels in %u windows\n", w_nch, w_nwin);
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

void scanner_peer_mon(uint8_t id, uint32_t freq_hz)
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

void scanner_peer_end(uint8_t id, uint32_t freq_hz)
{
    critical_section_enter_blocking(&s_cs);
    for (int i = 0; i < MAX_PEERS; i++)
        if (s_peers[i].id == id) s_peers[i].freq = 0;
    critical_section_exit(&s_cs);
}

/* ------------------------------------------------------------------------- */
/* tuner control                                                             */
static void apply_gain(void)
{
    int16_t g;
    critical_section_enter_blocking(&s_cs);
    g = s_cfg.gain_x10;
    critical_section_exit(&s_cs);
    if (g < 0) {
        rtlsdr_set_tuner_gain_mode(0);
        rtlsdr_set_agc_mode(1);
    } else {
        rtlsdr_set_agc_mode(0);
        rtlsdr_set_tuner_gain_mode(1);
        rtlsdr_set_tuner_gain(g);
    }
    s_gain_pending = false;
}

static bool retune(uint32_t center)
{
    uint32_t t0 = time_us_32();
    usb_stream_pause();
    r82xx_pll_wait_us = s_cfg.pllwait_us;
    rtlsdr_keep_i2c_repeater = s_cfg.keep_rep;
    if (s_gain_pending) apply_gain();
    int r = rtlsdr_set_center_freq(center);
    ring_reset(&g_shared.iq_ring);             /* no transfer in flight while paused */
    usb_stream_resume();
    uint32_t dt = time_us_32() - t0;
    st_retune_us += dt;
    if (dt > st_retune_max) st_retune_max = dt;
    if (r != 0) st_retune_fail++;
    s_discard = (uint32_t)s_cfg.settle_us * (FS * 2 / 1000) / 1000;
    s_discard &= ~1u;
    return r == 0;
}

/* ------------------------------------------------------------------------- */
/* monitor (AM demodulation)                                                 */
static void monitor_start(int ch, uint32_t center, int16_t level)
{
    const float pi = 3.14159265358979f;
    s_mon_ch = ch;
    s_mon_center = center;
    s_mon_freq = w_ch[ch].freq;
    int32_t off = (int32_t)(w_ch[ch].freq - center);
    float w = -2.0f * pi * (float)off / (float)FS;
    s_rot_re = cosf(w); s_rot_im = sinf(w);
    s_nco_re = 1.0f; s_nco_im = 0.0f;
    s_dec_re = s_dec_im = 0; s_dec_n = 0;
    s_dc = 0; s_mon_blocks = 0;
    s_mon_start_ms = s_mon_active_ms = now_ms();
    ring_reset(&s_audio_ring);
    st_hits++;
    printf("[scan] HIT %lu.%04lu MHz  %d.%d dBFS\n", (unsigned long)(s_mon_freq / 1000000),
           (unsigned long)(s_mon_freq % 1000000 / 100), level / 10, abs(level % 10));
    s_state = ST_MONITOR;
}

static void monitor_end(const char *why)
{
    uint32_t dur = now_ms() - s_mon_start_ms;
    printf("[scan] END %lu.%04lu MHz  %lu.%lu s (%s)\n", (unsigned long)(s_mon_freq / 1000000),
           (unsigned long)(s_mon_freq % 1000000 / 100), (unsigned long)(dur / 1000),
           (unsigned long)(dur % 1000 / 100), why);
    s_mon_freq = 0;
    s_mon_ch = -1;
    w_cur = (uint16_t)((w_cur + 1) % (w_nwin ? w_nwin : 1));
    s_state = ST_RETUNE;
}

static void demod_block(const uint8_t *iq)
{
    int16_t out[NFFT / DECIM + 1];
    int nout = 0;
    for (int i = 0; i < NFFT; i++) {
        float xr = ((float)iq[2 * i] - 127.5f), xi = ((float)iq[2 * i + 1] - 127.5f);
        float yr = xr * s_nco_re - xi * s_nco_im;
        float yi = xr * s_nco_im + xi * s_nco_re;
        float nr = s_nco_re * s_rot_re - s_nco_im * s_rot_im;
        s_nco_im = s_nco_re * s_rot_im + s_nco_im * s_rot_re;
        s_nco_re = nr;
        s_dec_re += yr; s_dec_im += yi;
        if (++s_dec_n == DECIM) {
            float m = sqrtf(s_dec_re * s_dec_re + s_dec_im * s_dec_im) * (1.0f / DECIM);
            s_dec_re = s_dec_im = 0; s_dec_n = 0;
            s_dc += 0.002f * (m - s_dc);                         /* carrier level */
            float a = (m - s_dc) / (s_dc + 0.5f) * 12000.0f;     /* normalised modulation */
            if (a > 32767.0f) a = 32767.0f;
            if (a < -32767.0f) a = -32767.0f;
            out[nout++] = (int16_t)a;
        }
    }
    /* keep the NCO on the unit circle */
    float mag = sqrtf(s_nco_re * s_nco_re + s_nco_im * s_nco_im);
    s_nco_re /= mag; s_nco_im /= mag;
    ring_write(&s_audio_ring, (const uint8_t *)out, (uint32_t)nout * 2);
}

/* ------------------------------------------------------------------------- */
void scanner_init(void)
{
    critical_section_init(&s_cs);
    ring_init(&s_audio_ring, s_audio_buf, sizeof(s_audio_buf));
    dsp_init();
    load_settings();
    printf("[scan] board %d, control TCP %d, coord UDP %d, audio UDP %d\n", CFG_BOARD_ID,
           SCAN_CTRL_PORT, SCAN_COORD_PORT, SCAN_AUDIO_PORT_BASE + CFG_BOARD_ID);
}

void scanner_on_sdr_ready(void)
{
    rtlsdr_set_sample_rate(FS);
    r82xx_pll_wait_us = s_cfg.pllwait_us;
    rtlsdr_keep_i2c_repeater = s_cfg.keep_rep;
    apply_gain();
    rtlsdr_reset_buffer();
    usb_stream_reset_toggle();
    ring_reset(&g_shared.iq_ring);
    usb_stream_start();
    s_sdr = true;
    s_dirty = true;
    s_state = ST_IDLE;
    printf("[scan] SDR ready, scanning starts\n");
}

void scanner_on_sdr_lost(void)
{
    if (s_sdr) printf("[scan] SDR lost\n");
    s_sdr = false;
    s_mon_freq = 0;
    s_state = ST_OFF;
}

bool scanner_monitoring(void) { return s_state == ST_MONITOR; }
uint32_t scanner_monitor_freq(void) { return s_mon_freq; }
ring_t *scanner_audio_ring(void) { return &s_audio_ring; }

void scanner_task(void)
{
    if (!s_sdr || s_state == ST_OFF) return;
    ring_t *r = &g_shared.iq_ring;

    if (s_dirty && s_state != ST_MONITOR) { rebuild_windows(); s_state = ST_RETUNE; }

    switch (s_state) {
    case ST_IDLE:
        ring_reset(r);
        if (s_cfg.scan_on && w_nwin) s_state = ST_RETUNE;
        break;

    case ST_RETUNE: {
        if (!s_cfg.scan_on || !w_nwin) { s_state = ST_IDLE; break; }
        /* skip windows whose channels are all monitored by other units */
        for (int n = 0; n < w_nwin; n++) {
            const win_t *w = &w_win[w_cur];
            bool any = false;
            for (int k = 0; k < w->count && !any; k++) any = !peer_busy(w_ch[w->first + k].freq, NULL);
            if (any) break;
            w_cur = (uint16_t)((w_cur + 1) % w_nwin);
        }
        retune(w_win[w_cur].center);
        s_state = ST_SETTLE;
        break;
    }

    case ST_SETTLE: {
        while (s_discard) {
            uint32_t n = s_discard > sizeof(s_junk) ? sizeof(s_junk) : s_discard;
            uint32_t got = ring_read(r, s_junk, n);
            if (!got) return;
            s_discard -= got;
        }
        memset(s_acc, 0, sizeof(s_acc));
        s_nfft = 0;
        s_state = ST_MEASURE;
        break;
    }

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
            int16_t lv = channel_level(s_acc, s_nfft, (int32_t)(w_ch[idx].freq - w->center), s_cfg.bw_hz);
            s_last_x10[idx] = lv;
            if (lv >= w_ch[idx].sql_x10 && lv > best_lv && !peer_busy(w_ch[idx].freq, NULL)) {
                best = idx; best_lv = lv;
            }
        }
        st_windows++;
        st_checks += w->count;
        if (best >= 0) {
            monitor_start(best, w->center, best_lv);
        } else {
            w_cur = (uint16_t)((w_cur + 1) % w_nwin);
            s_state = ST_RETUNE;
        }
        break;
    }

    case ST_MONITOR: {
        for (int blocks = 0; blocks < 4; blocks++) {
            if (ring_used(r) < sizeof(s_iq)) break;
            ring_read(r, s_iq, sizeof(s_iq));
            demod_block(s_iq);
            /* squelch check every 8 blocks (~8 ms) */
            if ((++s_mon_blocks & 7) == 0) {
                memset(s_mon_acc, 0, sizeof(s_mon_acc));
                block_to_spectrum(s_iq, s_mon_acc);
                int16_t lv = channel_level(s_mon_acc, 1, (int32_t)(s_mon_freq - s_mon_center), s_cfg.bw_hz);
                s_last_x10[s_mon_ch] = lv;
                if (lv >= w_ch[s_mon_ch].sql_x10) s_mon_active_ms = now_ms();
            }
        }
        uint8_t by = 0;
        if (peer_busy(s_mon_freq, &by) && by < CFG_BOARD_ID) { monitor_end("peer has priority"); break; }
        if (!s_cfg.scan_on) { monitor_end("scan off"); break; }
        if (s_dirty) { monitor_end("channel list changed"); break; }
        if (now_ms() - s_mon_active_ms > s_cfg.hold_ms) monitor_end("signal gone");
        break;
    }

    default:
        break;
    }
}

void scanner_print_stats(void)
{
    static uint32_t last_ms = 0, last_win = 0, last_chk = 0, last_retune = 0, last_hits = 0;
    uint32_t t = now_ms();
    uint32_t dt = t - last_ms;
    if (!dt) return;
    uint32_t win = st_windows - last_win, chk = st_checks - last_chk;
    uint32_t avg_rt = win ? (st_retune_us - last_retune) / win : 0;
    printf("[scan] %s  win/s %lu  ch/s %lu  retune avg %lu us max %lu us  fail %lu  hits %lu  audio %lu B\n",
           st_name[s_state], (unsigned long)(win * 1000 / dt), (unsigned long)(chk * 1000 / dt),
           (unsigned long)avg_rt, (unsigned long)st_retune_max, (unsigned long)st_retune_fail,
           (unsigned long)(st_hits - last_hits), (unsigned long)ring_used(&s_audio_ring));
    last_ms = t; last_win = st_windows; last_chk = st_checks;
    last_retune = st_retune_us; last_hits = st_hits;
    st_retune_max = 0;
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
    if (!any || *s) return false;
    uint64_t v = (uint64_t)ip * 1000000u + fp;
    if (v < 24000000u || v > 1766000000u) return false;
    *hz = (uint32_t)v;
    return true;
}

static bool parse_x10(const char *s, int32_t *out)
{
    int sign = 1;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') s++;
    int32_t ip = 0, fp = 0;
    bool any = false;
    while (*s >= '0' && *s <= '9') { ip = ip * 10 + (*s++ - '0'); any = true; }
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

static void fmt_mhz(char *b, size_t n, uint32_t hz)
{
    snprintf(b, n, "%lu.%04lu", (unsigned long)(hz / 1000000), (unsigned long)(hz % 1000000 / 100));
}

static void fmt_db(char *b, size_t n, int32_t x10)
{
    snprintf(b, n, "%s%ld.%ld", x10 < 0 ? "-" : "", (long)(labs(x10) / 10), (long)(labs(x10) % 10));
}

void scanner_set_client_ip(const uint8_t ip[4])
{
    critical_section_enter_blocking(&s_cs);
    memcpy(s_client_ip, ip, 4);
    s_client_valid = true;
    critical_section_exit(&s_cs);
}

bool scanner_audio_dest(uint8_t ip[4], uint16_t *port)
{
    bool ok = false;
    critical_section_enter_blocking(&s_cs);
    if (s_cfg.audio_auto) {
        if (s_client_valid) { memcpy(ip, s_client_ip, 4); ok = true; }
    } else if (s_cfg.audio_ip[0]) {
        memcpy(ip, s_cfg.audio_ip, 4); ok = true;
    }
    critical_section_exit(&s_cs);
    *port = SCAN_AUDIO_PORT_BASE + CFG_BOARD_ID;
    return ok;
}

static void emit_status(scan_emit_fn emit)
{
    char b[200], d1[16], d2[16];
    fmt_db(d1, sizeof(d1), s_cfg.sql_x10);
    if (s_cfg.gain_x10 < 0) snprintf(d2, sizeof(d2), "AUTO"); else fmt_db(d2, sizeof(d2), s_cfg.gain_x10);
    snprintf(b, sizeof(b),
             "board %d  scan %s  state %s  sql %s dBFS  hold %u ms  gain %s  settle %u us  navg %u  bw %u Hz  pllwait %u us  repeater %s\n",
             CFG_BOARD_ID, s_cfg.scan_on ? "ON" : "OFF", st_name[s_state], d1, s_cfg.hold_ms, d2,
             s_cfg.settle_us, s_cfg.navg, s_cfg.bw_hz, s_cfg.pllwait_us, s_cfg.keep_rep ? "KEEP" : "TOGGLE");
    emit(b);
    uint8_t ip[4]; uint16_t port;
    if (scanner_audio_dest(ip, &port))
        snprintf(b, sizeof(b), "audio -> %u.%u.%u.%u:%u (%s)  12500 Hz s16le mono\n", ip[0], ip[1], ip[2], ip[3],
                 port, s_cfg.audio_auto ? "AUTO" : "fixed");
    else
        snprintf(b, sizeof(b), "audio -> none (%s)\n", s_cfg.audio_auto ? "AUTO, no client yet" : "OFF");
    emit(b);
    uint32_t mf = s_mon_freq;
    if (mf) { fmt_mhz(d1, sizeof(d1), mf); snprintf(b, sizeof(b), "monitoring %s MHz\n", d1); emit(b); }
}

static int find_ch(uint32_t hz)
{
    for (int i = 0; i < s_cfg.nch; i++) if (s_cfg.ch[i].freq == hz) return i;
    return -1;
}

void scanner_ctl(const char *line, scan_emit_fn emit)
{
    char buf[128], *save = NULL;
    strncpy(buf, line, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *cmd = strtok_r(buf, " \t\r\n", &save);
    if (!cmd || cmd[0] == '#') return;
    for (char *p = cmd; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    char *a1 = strtok_r(NULL, " \t\r\n", &save);
    char *a2 = strtok_r(NULL, " \t\r\n", &save);
    char out[160], f[16], d[16];
    uint32_t hz, u;
    int32_t x;

    if (!strcmp(cmd, "HELP")) {
        emit("ADD <MHz> [sql]  DEL <MHz>  CLEAR  LIST  STAT\n"
             "SQL <dBFS>  HOLD <ms>  GAIN <dB>|AUTO  SETTLE <us>  NAVG <1-8>  BW <Hz>  PLLWAIT <us>\n"
             "REPEATER KEEP|TOGGLE  AUDIO <ip>|AUTO|OFF  SCAN ON|OFF  SAVE\n");
    } else if (!strcmp(cmd, "ADD") && a1 && parse_mhz(a1, &hz)) {
        int16_t sql = SQL_DEFAULT;
        if (a2) { if (!parse_x10(a2, &x)) { emit("ERR sql\n"); return; } sql = (int16_t)x; }
        critical_section_enter_blocking(&s_cs);
        int i = find_ch(hz);
        bool ok = true;
        if (i >= 0) s_cfg.ch[i].sql_x10 = sql;
        else if (s_cfg.nch >= SCAN_MAX_CH) ok = false;
        else {
            int pos = s_cfg.nch;
            while (pos > 0 && s_cfg.ch[pos - 1].freq > hz) { s_cfg.ch[pos] = s_cfg.ch[pos - 1]; s_last_x10[pos] = s_last_x10[pos - 1]; pos--; }
            s_cfg.ch[pos].freq = hz; s_cfg.ch[pos].sql_x10 = sql; s_cfg.ch[pos].pad = 0;
            s_last_x10[pos] = -1500;
            s_cfg.nch++;
        }
        s_dirty = true;
        critical_section_exit(&s_cs);
        fmt_mhz(f, sizeof(f), hz);
        snprintf(out, sizeof(out), ok ? "OK %s\n" : "ERR full (%s)\n", f);
        emit(out);
    } else if (!strcmp(cmd, "DEL") && a1 && parse_mhz(a1, &hz)) {
        critical_section_enter_blocking(&s_cs);
        int i = find_ch(hz);
        if (i >= 0) {
            for (int k = i; k + 1 < s_cfg.nch; k++) { s_cfg.ch[k] = s_cfg.ch[k + 1]; s_last_x10[k] = s_last_x10[k + 1]; }
            s_cfg.nch--;
            s_dirty = true;
        }
        critical_section_exit(&s_cs);
        emit(i >= 0 ? "OK\n" : "ERR not found\n");
    } else if (!strcmp(cmd, "CLEAR")) {
        critical_section_enter_blocking(&s_cs);
        s_cfg.nch = 0;
        s_dirty = true;
        critical_section_exit(&s_cs);
        emit("OK\n");
    } else if (!strcmp(cmd, "LIST")) {
        emit_status(emit);
        int n = s_cfg.nch;
        for (int i = 0; i < n; i++) {
            store_ch_t c = s_cfg.ch[i];
            fmt_mhz(f, sizeof(f), c.freq);
            if (c.sql_x10 == SQL_DEFAULT) snprintf(d, sizeof(d), "default"); else fmt_db(d, sizeof(d), c.sql_x10);
            char lv[16];
            fmt_db(lv, sizeof(lv), s_last_x10[i]);
            snprintf(out, sizeof(out), "%3d  %s MHz  sql %-7s  last %s dBFS\n", i + 1, f, d, lv);
            emit(out);
        }
        snprintf(out, sizeof(out), "%d channels\n", n);
        emit(out);
    } else if (!strcmp(cmd, "STAT")) {
        emit_status(emit);
    } else if (!strcmp(cmd, "SQL") && a1 && parse_x10(a1, &x)) {
        s_cfg.sql_x10 = (int16_t)x; s_dirty = true; emit("OK\n");
    } else if (!strcmp(cmd, "HOLD") && a1 && parse_u32(a1, &u) && u <= 60000) {
        s_cfg.hold_ms = (uint16_t)u; emit("OK\n");
    } else if (!strcmp(cmd, "GAIN") && a1) {
        for (char *p = a1; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        if (!strcmp(a1, "AUTO")) s_cfg.gain_x10 = -1;
        else if (parse_x10(a1, &x) && x >= 0 && x <= 500) s_cfg.gain_x10 = (int16_t)x;
        else { emit("ERR gain\n"); return; }
        s_gain_pending = true; emit("OK\n");
    } else if (!strcmp(cmd, "SETTLE") && a1 && parse_u32(a1, &u) && u <= 20000) {
        s_cfg.settle_us = (uint16_t)u; emit("OK\n");
    } else if (!strcmp(cmd, "NAVG") && a1 && parse_u32(a1, &u) && u >= 1 && u <= 8) {
        s_cfg.navg = (uint16_t)u; emit("OK\n");
    } else if (!strcmp(cmd, "BW") && a1 && parse_u32(a1, &u) && u >= 1000 && u <= 25000) {
        s_cfg.bw_hz = (uint16_t)u; emit("OK\n");
    } else if (!strcmp(cmd, "PLLWAIT") && a1 && parse_u32(a1, &u) && u >= 200 && u <= 20000) {
        s_cfg.pllwait_us = (uint16_t)u; emit("OK\n");
    } else if (!strcmp(cmd, "REPEATER") && a1) {
        for (char *p = a1; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        if (!strcmp(a1, "KEEP")) s_cfg.keep_rep = 1;
        else if (!strcmp(a1, "TOGGLE")) s_cfg.keep_rep = 0;
        else { emit("ERR\n"); return; }
        emit("OK\n");
    } else if (!strcmp(cmd, "AUDIO") && a1) {
        uint8_t ip[4];
        for (char *p = a1; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        critical_section_enter_blocking(&s_cs);
        bool ok = true;
        if (!strcmp(a1, "AUTO")) s_cfg.audio_auto = 1;
        else if (!strcmp(a1, "OFF")) { s_cfg.audio_auto = 0; memset(s_cfg.audio_ip, 0, 4); }
        else if (parse_ip(a1, ip)) { s_cfg.audio_auto = 0; memcpy(s_cfg.audio_ip, ip, 4); }
        else ok = false;
        critical_section_exit(&s_cs);
        emit(ok ? "OK\n" : "ERR ip\n");
    } else if (!strcmp(cmd, "SCAN") && a1) {
        for (char *p = a1; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        if (!strcmp(a1, "ON")) s_cfg.scan_on = 1;
        else if (!strcmp(a1, "OFF")) s_cfg.scan_on = 0;
        else { emit("ERR\n"); return; }
        emit("OK\n");
    } else if (!strcmp(cmd, "SAVE")) {
        emit(save_settings() ? "OK saved\n" : "ERR flash verify\n");
    } else {
        snprintf(out, sizeof(out), "ERR unknown or bad argument: %s (HELP for commands)\n", cmd);
        emit(out);
    }
}

#endif /* CFG_SCANNER */

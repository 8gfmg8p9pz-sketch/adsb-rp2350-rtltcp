/*
 * rtl2832u.h -- minimal librtlsdr port for the RP2350 USB host
 * (register access, baseband init, tuner probing, R820T/R828D via tuner_r82xx.c)
 *
 * Derived from librtlsdr (osmocom) -- GPL-2.0-or-later
 */
#ifndef RTL2832U_H
#define RTL2832U_H

#include <stdint.h>
#include <stdbool.h>

enum rtlsdr_tuner {
    RTLSDR_TUNER_UNKNOWN = 0,
    RTLSDR_TUNER_E4000,
    RTLSDR_TUNER_FC0012,
    RTLSDR_TUNER_FC0013,
    RTLSDR_TUNER_FC2580,
    RTLSDR_TUNER_R820T,
    RTLSDR_TUNER_R828D
};

int  rtlsdr_open(void);                       /* after USB enumeration; probes tuner */
void rtlsdr_close(void);

int  rtlsdr_set_center_freq(uint32_t freq);
uint32_t rtlsdr_get_center_freq(void);
int  rtlsdr_set_sample_rate(uint32_t rate);
uint32_t rtlsdr_get_sample_rate(void);
int  rtlsdr_set_freq_correction(int ppm);
int  rtlsdr_set_tuner_gain_mode(int manual);
int  rtlsdr_set_tuner_gain(int gain_tenth_db);
int  rtlsdr_set_tuner_gain_by_index(unsigned index);
int  rtlsdr_get_tuner_gains(int *gains);      /* NULL -> count only */
int  rtlsdr_set_tuner_if_gain(int stage, int gain);
int  rtlsdr_set_tuner_bandwidth(uint32_t bw);
int  rtlsdr_set_testmode(int on);
int  rtlsdr_set_agc_mode(int on);
int  rtlsdr_set_direct_sampling(int on);
int  rtlsdr_set_offset_tuning(int on);
int  rtlsdr_set_xtal_freq(uint32_t rtl_freq, uint32_t tuner_freq);
int  rtlsdr_set_bias_tee(int on);
int  rtlsdr_set_ddc_offset(int32_t offset_hz);  /* fast scanner: retune inside the tuner IF filter */
int  rtlsdr_get_direct_sampling(void);
int  rtlsdr_reset_buffer(void);
enum rtlsdr_tuner rtlsdr_get_tuner_type(void);
const char *rtlsdr_tuner_name(void);

#endif

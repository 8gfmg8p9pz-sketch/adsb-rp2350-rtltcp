/*
 * rtl2832u.c -- librtlsdr core ported to the RP2350 TinyUSB host
 *
 * This is a reduced port of librtlsdr.c (osmocom rtl-sdr, GPL-2.0-or-later):
 * register access over vendor control transfers, baseband initialisation,
 * tuner probing and the R820T/R820T2/R860/R828D tuner (tuner_r82xx.c verbatim).
 * Other tuners (E4000, FC0012/13, FC2580) are detected but not supported.
 */
#include <stdio.h>
#include <string.h>
#include "rtl2832u.h"
#include "usb_host.h"
#include "rtlsdr_i2c.h"
#include "tuner_r82xx.h"
#include "config.h"

#define TWO_POW(n)          ((double)(1ULL << (n)))
#define DEF_RTL_XTAL_FREQ   28800000
#define MIN_RTL_XTAL_FREQ   (DEF_RTL_XTAL_FREQ - 1000)
#define MAX_RTL_XTAL_FREQ   (DEF_RTL_XTAL_FREQ + 1000)

enum usb_reg {
    USB_SYSCTL       = 0x2000,
    USB_CTRL         = 0x2010,
    USB_STAT         = 0x2014,
    USB_EPA_CFG      = 0x2144,
    USB_EPA_CTL      = 0x2148,
    USB_EPA_MAXPKT   = 0x2158,
    USB_EPA_MAXPKT_2 = 0x215a,
    USB_EPA_FIFO_CFG = 0x2160,
};
enum sys_reg {
    DEMOD_CTL   = 0x3000,
    GPO         = 0x3001,
    GPI         = 0x3002,
    GPOE        = 0x3003,
    GPD         = 0x3004,
    SYSINTE     = 0x3005,
    SYSINTS     = 0x3006,
    GP_CFG0     = 0x3007,
    GP_CFG1     = 0x3008,
    SYSINTE_1   = 0x3009,
    SYSINTS_1   = 0x300a,
    DEMOD_CTL_1 = 0x300b,
    IR_SUSPEND  = 0x300c,
};
enum blocks { DEMODB = 0, USBB = 1, SYSB = 2, TUNB = 3, ROMB = 4, IRB = 5, IICB = 6 };

/* other tuner probe constants (from the respective tuner headers) */
#define E4K_I2C_ADDR     0xc8
#define E4K_CHECK_ADDR   0x02
#define E4K_CHECK_VAL    0x40
#define FC0012_I2C_ADDR  0xc6
#define FC0012_CHECK_ADDR 0x00
#define FC0012_CHECK_VAL 0xa1
#define FC0013_I2C_ADDR  0xc6
#define FC0013_CHECK_ADDR 0x00
#define FC0013_CHECK_VAL 0xa3
#define FC2580_I2C_ADDR  0xac
#define FC2580_CHECK_ADDR 0x01
#define FC2580_CHECK_VAL 0x56

#define FIR_LEN 16
static const int fir_default[FIR_LEN] = {
    -54, -36, -41, -40, -32, -14, 14, 53,   /* 8 bit signed */
    101, 156, 215, 273, 327, 372, 404, 421  /* 12 bit signed */
};

/* ---------------------------------------------------------------------------
 * device state (single device)
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t rate;       /* Hz */
    uint32_t rtl_xtal;   /* Hz */
    int      fir[FIR_LEN];
    int      direct_sampling;
    enum rtlsdr_tuner tuner_type;
    uint32_t tun_xtal;   /* Hz */
    uint32_t freq;       /* Hz */
    uint32_t bw;
    uint32_t offs_freq;  /* Hz */
    int      corr;       /* ppm */
    int      gain;       /* tenth dB */
    struct r82xx_config r82xx_c;
    struct r82xx_priv   r82xx_p;
    int      is_open;
} rtlsdr_dev_t;

static rtlsdr_dev_t g_dev;
static rtlsdr_dev_t *dev = &g_dev;

/* ---------------------------------------------------------------------------
 * low level register access (vendor request 0)
 * ------------------------------------------------------------------------- */
static int rtlsdr_read_array(uint8_t block, uint16_t addr, uint8_t *array, uint8_t len)
{
    uint16_t index = (block << 8);
    return usb_host_control(true, 0, addr, index, array, len);
}

static int rtlsdr_write_array(uint8_t block, uint16_t addr, uint8_t *array, uint8_t len)
{
    uint16_t index = (block << 8) | 0x10;
    return usb_host_control(false, 0, addr, index, array, len);
}

static int rtlsdr_i2c_write(uint8_t i2c_addr, uint8_t *buffer, int len)
{
    return rtlsdr_write_array(IICB, i2c_addr, buffer, (uint8_t)len);
}

static int rtlsdr_i2c_read(uint8_t i2c_addr, uint8_t *buffer, int len)
{
    return rtlsdr_read_array(IICB, i2c_addr, buffer, (uint8_t)len);
}

static uint8_t rtlsdr_i2c_read_reg(uint8_t i2c_addr, uint8_t reg)
{
    uint8_t data = 0;
    rtlsdr_write_array(IICB, i2c_addr, &reg, 1);
    rtlsdr_read_array(IICB, i2c_addr, &data, 1);
    return data;
}

static uint16_t rtlsdr_read_reg(uint8_t block, uint16_t addr, uint8_t len)
{
    uint8_t data[2] = {0, 0};
    uint16_t index = (block << 8);
    int r = usb_host_control(true, 0, addr, index, data, len);
    if (r < 0) printf("%s failed with %d\n", __FUNCTION__, r);
    return (uint16_t)((data[1] << 8) | data[0]);
}

static int rtlsdr_write_reg(uint8_t block, uint16_t addr, uint16_t val, uint8_t len)
{
    uint8_t data[2];
    uint16_t index = (block << 8) | 0x10;
    if (len == 1) data[0] = val & 0xff; else data[0] = val >> 8;
    data[1] = val & 0xff;
    int r = usb_host_control(false, 0, addr, index, data, len);
    if (r < 0) printf("%s failed with %d\n", __FUNCTION__, r);
    return r;
}

static uint16_t rtlsdr_demod_read_reg(uint8_t page, uint16_t addr, uint8_t len)
{
    uint8_t data[2] = {0, 0};
    uint16_t index = page;
    addr = (addr << 8) | 0x20;
    int r = usb_host_control(true, 0, addr, index, data, len);
    if (r < 0) printf("%s failed with %d\n", __FUNCTION__, r);
    return (uint16_t)((data[1] << 8) | data[0]);
}

static int rtlsdr_demod_write_reg(uint8_t page, uint16_t addr, uint16_t val, uint8_t len)
{
    uint8_t data[2];
    uint16_t index = 0x10 | page;
    addr = (addr << 8) | 0x20;
    if (len == 1) data[0] = val & 0xff; else data[0] = val >> 8;
    data[1] = val & 0xff;
    int r = usb_host_control(false, 0, addr, index, data, len);
    if (r < 0) printf("%s failed with %d\n", __FUNCTION__, r);
    rtlsdr_demod_read_reg(0x0a, 0x01, 1);
    return (r == len) ? 0 : -1;
}

static void rtlsdr_set_gpio_bit(uint8_t gpio, int val)
{
    uint16_t r;
    gpio = 1 << gpio;
    r = rtlsdr_read_reg(SYSB, GPO, 1);
    r = val ? (r | gpio) : (r & ~gpio);
    rtlsdr_write_reg(SYSB, GPO, r, 1);
}

static void rtlsdr_set_gpio_output(uint8_t gpio)
{
    int r;
    gpio = 1 << gpio;
    r = rtlsdr_read_reg(SYSB, GPD, 1);
    rtlsdr_write_reg(SYSB, GPD, r & ~gpio, 1);
    r = rtlsdr_read_reg(SYSB, GPOE, 1);
    rtlsdr_write_reg(SYSB, GPOE, r | gpio, 1);
}

static void rtlsdr_set_i2c_repeater(int on)
{
    rtlsdr_demod_write_reg(1, 0x01, on ? 0x18 : 0x10, 1);
}

/* ---------------------------------------------------------------------------
 * callbacks required by tuner_r82xx.c (rtlsdr_i2c.h)
 * ------------------------------------------------------------------------- */
int rtlsdr_check_dongle_model(void *d, char *manufact_check, char *product_check)
{
    (void)d;
    return (strcmp(usb_host_manufacturer(), manufact_check) == 0 &&
            strcmp(usb_host_product(), product_check) == 0) ? 1 : 0;
}

static int rtlsdr_get_xtal_freq(uint32_t *rtl_freq, uint32_t *tuner_freq)
{
#define APPLY_PPM_CORR(val, ppm) (((val) * (1.0 + (ppm) / 1e6)))
    if (rtl_freq)   *rtl_freq   = (uint32_t)APPLY_PPM_CORR(dev->rtl_xtal, dev->corr);
    if (tuner_freq) *tuner_freq = (uint32_t)APPLY_PPM_CORR(dev->tun_xtal, dev->corr);
    return 0;
}

uint32_t rtlsdr_get_tuner_clock(void *d)
{
    (void)d;
    uint32_t tuner_freq;
    if (rtlsdr_get_xtal_freq(NULL, &tuner_freq)) return 0;
    return tuner_freq;
}

int rtlsdr_i2c_write_fn(void *d, uint8_t addr, uint8_t *buf, int len)
{
    (void)d;
    return rtlsdr_i2c_write(addr, buf, len);
}

int rtlsdr_i2c_read_fn(void *d, uint8_t addr, uint8_t *buf, int len)
{
    (void)d;
    return rtlsdr_i2c_read(addr, buf, len);
}

int rtlsdr_set_bias_tee_gpio(void *d, int gpio, int on)
{
    (void)d;
    rtlsdr_set_gpio_output(gpio);
    rtlsdr_set_gpio_bit(gpio, on);
    return 0;
}

/* ---------------------------------------------------------------------------
 * baseband
 * ------------------------------------------------------------------------- */
static int rtlsdr_set_fir(void)
{
    uint8_t fir[20];
    int i;
    for (i = 0; i < 8; ++i) {
        const int val = dev->fir[i];
        if (val < -128 || val > 127) return -1;
        fir[i] = val;
    }
    for (i = 0; i < 8; i += 2) {
        const int val0 = dev->fir[8 + i];
        const int val1 = dev->fir[8 + i + 1];
        if (val0 < -2048 || val0 > 2047 || val1 < -2048 || val1 > 2047) return -1;
        fir[8 + i * 3 / 2]     = val0 >> 4;
        fir[8 + i * 3 / 2 + 1] = (val0 << 4) | ((val1 >> 8) & 0x0f);
        fir[8 + i * 3 / 2 + 2] = val1;
    }
    for (i = 0; i < (int)sizeof(fir); i++) {
        if (rtlsdr_demod_write_reg(1, 0x1c + i, fir[i], 1)) return -1;
    }
    return 0;
}

static void rtlsdr_init_baseband(void)
{
    unsigned int i;

    /* initialize USB */
    rtlsdr_write_reg(USBB, USB_SYSCTL, 0x09, 1);
    rtlsdr_write_reg(USBB, USB_EPA_MAXPKT, CFG_RTL_EPA_MAXPKT, 2);
    rtlsdr_write_reg(USBB, USB_EPA_CTL, 0x1002, 2);

    /* poweron demod */
    rtlsdr_write_reg(SYSB, DEMOD_CTL_1, 0x22, 1);
    rtlsdr_write_reg(SYSB, DEMOD_CTL, 0xe8, 1);

    /* reset demod (bit 3, soft_rst) */
    rtlsdr_demod_write_reg(1, 0x01, 0x14, 1);
    rtlsdr_demod_write_reg(1, 0x01, 0x10, 1);

    /* disable spectrum inversion and adjacent channel rejection */
    rtlsdr_demod_write_reg(1, 0x15, 0x00, 1);
    rtlsdr_demod_write_reg(1, 0x16, 0x0000, 2);

    /* clear both DDC shift and IF frequency registers  */
    for (i = 0; i < 6; i++)
        rtlsdr_demod_write_reg(1, 0x16 + i, 0x00, 1);

    rtlsdr_set_fir();

    /* enable SDR mode, disable DAGC (bit 5) */
    rtlsdr_demod_write_reg(0, 0x19, 0x05, 1);

    /* init FSM state-holding register */
    rtlsdr_demod_write_reg(1, 0x93, 0xf0, 1);
    rtlsdr_demod_write_reg(1, 0x94, 0x0f, 1);

    /* disable AGC (en_dagc, bit 0) (this seems to have no effect) */
    rtlsdr_demod_write_reg(1, 0x11, 0x00, 1);

    /* disable RF and IF AGC loop */
    rtlsdr_demod_write_reg(1, 0x04, 0x00, 1);

    /* disable PID filter (enable_PID = 0) */
    rtlsdr_demod_write_reg(0, 0x61, 0x60, 1);

    /* opt_adc_iq = 0, default ADC_I/ADC_Q datapath */
    rtlsdr_demod_write_reg(0, 0x06, 0x80, 1);

    /* Enable Zero-IF mode (en_bbin bit), DC cancellation (en_dc_est),
     * IQ estimation/compensation (en_iq_comp, en_iq_est) */
    rtlsdr_demod_write_reg(1, 0xb1, 0x1b, 1);

    /* disable 4.096 MHz clock output on pin TP_CK0 */
    rtlsdr_demod_write_reg(0, 0x0d, 0x83, 1);
}

static int rtlsdr_set_if_freq(uint32_t freq)
{
    uint32_t rtl_xtal;
    int32_t if_freq;
    uint8_t tmp;
    int r;

    if (rtlsdr_get_xtal_freq(&rtl_xtal, NULL)) return -2;

    if_freq = ((freq * TWO_POW(22)) / rtl_xtal) * (-1);

    tmp = (if_freq >> 16) & 0x3f;
    r = rtlsdr_demod_write_reg(1, 0x19, tmp, 1);
    tmp = (if_freq >> 8) & 0xff;
    r |= rtlsdr_demod_write_reg(1, 0x1a, tmp, 1);
    tmp = if_freq & 0xff;
    r |= rtlsdr_demod_write_reg(1, 0x1b, tmp, 1);
    return r;
}

static int rtlsdr_set_sample_freq_correction(int ppm)
{
    int r = 0;
    uint8_t tmp;
    int16_t offs = ppm * (-1) * TWO_POW(24) / 1000000;

    tmp = offs & 0xff;
    r |= rtlsdr_demod_write_reg(1, 0x3f, tmp, 1);
    tmp = (offs >> 8) & 0x3f;
    r |= rtlsdr_demod_write_reg(1, 0x3e, tmp, 1);
    return r;
}

/* ---------------------------------------------------------------------------
 * R82xx tuner wrapper (from librtlsdr.c)
 * ------------------------------------------------------------------------- */
static int r820t_init(void)
{
    dev->r82xx_p.rtl_dev = dev;
    if (dev->tuner_type == RTLSDR_TUNER_R828D) {
        dev->r82xx_c.i2c_addr = R828D_I2C_ADDR;
        dev->r82xx_c.rafael_chip = CHIP_R828D;
    } else {
        dev->r82xx_c.i2c_addr = R820T_I2C_ADDR;
        dev->r82xx_c.rafael_chip = CHIP_R820T;
    }
    rtlsdr_get_xtal_freq(NULL, &dev->r82xx_c.xtal);
    dev->r82xx_c.max_i2c_msg_len = 8;
    dev->r82xx_c.use_predetect = 0;
    dev->r82xx_p.cfg = &dev->r82xx_c;
    return r82xx_init(&dev->r82xx_p);
}

static bool tuner_is_r82xx(void)
{
    return dev->tuner_type == RTLSDR_TUNER_R820T || dev->tuner_type == RTLSDR_TUNER_R828D;
}

static int tuner_set_freq(uint32_t freq)
{
    if (!tuner_is_r82xx()) return -1;
    return r82xx_set_freq(&dev->r82xx_p, freq);
}

static int tuner_set_bw(int bw)
{
    if (!tuner_is_r82xx()) return -1;
    int r = r82xx_set_bandwidth(&dev->r82xx_p, bw, dev->rate);
    if (r < 0) return r;
    r = rtlsdr_set_if_freq(r);
    if (r) return r;
    return rtlsdr_set_center_freq(dev->freq);
}

static int tuner_set_gain(int gain)
{
    if (!tuner_is_r82xx()) return -1;
    return r82xx_set_gain(&dev->r82xx_p, 1, gain);
}

static int tuner_set_gain_mode(int manual)
{
    if (!tuner_is_r82xx()) return -1;
    return r82xx_set_gain(&dev->r82xx_p, manual, 0);
}

/* ---------------------------------------------------------------------------
 * public API
 * ------------------------------------------------------------------------- */
int rtlsdr_open(void)
{
    uint8_t reg;
    int r = 0;

    memset(dev, 0, sizeof(*dev));
    memcpy(dev->fir, fir_default, sizeof(fir_default));
    dev->rtl_xtal = DEF_RTL_XTAL_FREQ;

    /* perform a dummy write, if it fails the device is not alive */
    if (rtlsdr_write_reg(USBB, USB_SYSCTL, 0x09, 1) < 0) {
        printf("[rtl] device does not answer to vendor requests\n");
        return -1;
    }

    rtlsdr_init_baseband();

    /* Probe tuners */
    rtlsdr_set_i2c_repeater(1);

    reg = rtlsdr_i2c_read_reg(E4K_I2C_ADDR, E4K_CHECK_ADDR);
    if (reg == E4K_CHECK_VAL) {
        printf("[rtl] Found Elonics E4000 tuner (NOT supported by this firmware)\n");
        dev->tuner_type = RTLSDR_TUNER_E4000;
        goto found;
    }
    reg = rtlsdr_i2c_read_reg(FC0013_I2C_ADDR, FC0013_CHECK_ADDR);
    if (reg == FC0013_CHECK_VAL) {
        printf("[rtl] Found Fitipower FC0013 tuner (NOT supported by this firmware)\n");
        dev->tuner_type = RTLSDR_TUNER_FC0013;
        goto found;
    }
    reg = rtlsdr_i2c_read_reg(R820T_I2C_ADDR, R82XX_CHECK_ADDR);
    if (reg == R82XX_CHECK_VAL) {
        printf("[rtl] Found Rafael Micro R820T tuner\n");
        if (rtlsdr_check_dongle_model(dev, "RTLSDRBlog", "Blog V4L"))
            printf("[rtl] RTL-SDR Blog V4 Lite Detected\n");
        dev->tuner_type = RTLSDR_TUNER_R820T;
        goto found;
    }
    reg = rtlsdr_i2c_read_reg(R828D_I2C_ADDR, R82XX_CHECK_ADDR);
    if (reg == R82XX_CHECK_VAL) {
        printf("[rtl] Found Rafael Micro R828D tuner\n");
        if (rtlsdr_check_dongle_model(dev, "RTLSDRBlog", "Blog V4"))
            printf("[rtl] RTL-SDR Blog V4 Detected\n");
        dev->tuner_type = RTLSDR_TUNER_R828D;
        goto found;
    }

    /* initialise GPIOs */
    rtlsdr_set_gpio_output(4);
    /* reset tuner before probing */
    rtlsdr_set_gpio_bit(4, 1);
    rtlsdr_set_gpio_bit(4, 0);

    reg = rtlsdr_i2c_read_reg(FC2580_I2C_ADDR, FC2580_CHECK_ADDR);
    if ((reg & 0x7f) == FC2580_CHECK_VAL) {
        printf("[rtl] Found FCI 2580 tuner (NOT supported by this firmware)\n");
        dev->tuner_type = RTLSDR_TUNER_FC2580;
        goto found;
    }
    reg = rtlsdr_i2c_read_reg(FC0012_I2C_ADDR, FC0012_CHECK_ADDR);
    if (reg == FC0012_CHECK_VAL) {
        printf("[rtl] Found Fitipower FC0012 tuner (NOT supported by this firmware)\n");
        rtlsdr_set_gpio_output(6);
        dev->tuner_type = RTLSDR_TUNER_FC0012;
        goto found;
    }

found:
    /* use the rtl clock value by default */
    dev->tun_xtal = dev->rtl_xtal;

    switch (dev->tuner_type) {
    case RTLSDR_TUNER_R828D:
        /* If NOT an RTL-SDR Blog V4, set typical R828D 16 MHz freq. Otherwise, keep at 28.8 MHz. */
        if (!(rtlsdr_check_dongle_model(dev, "RTLSDRBlog", "Blog V4")))
            dev->tun_xtal = R828D_XTAL_FREQ;
        /* fall-through */
    case RTLSDR_TUNER_R820T:
        /* disable Zero-IF mode */
        rtlsdr_demod_write_reg(1, 0xb1, 0x1a, 1);
        /* only enable In-phase ADC input */
        rtlsdr_demod_write_reg(0, 0x08, 0x4d, 1);
        /* the R82XX use 3.57 MHz IF for the DVB-T 6 MHz mode, and
         * 4.57 MHz for the 8 MHz mode */
        rtlsdr_set_if_freq(R82XX_IF_FREQ);
        /* enable spectrum inversion */
        rtlsdr_demod_write_reg(1, 0x15, 0x01, 1);
        break;
    case RTLSDR_TUNER_UNKNOWN:
        printf("[rtl] No supported tuner found\n");
        rtlsdr_set_i2c_repeater(0);
        dev->is_open = 1;
        rtlsdr_set_direct_sampling(1);
        return 0;
    default:
        break;
    }

    if (tuner_is_r82xx())
        r = r820t_init();

    rtlsdr_set_i2c_repeater(0);
    dev->is_open = 1;
    return r;
}

void rtlsdr_close(void)
{
    if (!dev->is_open) return;
    if (tuner_is_r82xx()) {
        rtlsdr_set_i2c_repeater(1);
        r82xx_standby(&dev->r82xx_p);
        rtlsdr_set_i2c_repeater(0);
    }
    /* poweroff demodulator and ADCs */
    rtlsdr_write_reg(SYSB, DEMOD_CTL, 0x20, 1);
    dev->is_open = 0;
}

int rtlsdr_set_center_freq(uint32_t freq)
{
    int r = -1;
    if (!dev->is_open) return -1;

    if (dev->direct_sampling) {
        r = rtlsdr_set_if_freq(freq);
    } else if (tuner_is_r82xx()) {
        rtlsdr_set_i2c_repeater(1);
        r = tuner_set_freq(freq - dev->offs_freq);
        rtlsdr_set_i2c_repeater(0);
    }
    dev->freq = r ? 0 : freq;
    return r;
}

uint32_t rtlsdr_get_center_freq(void) { return dev->freq; }

int rtlsdr_set_freq_correction(int ppm)
{
    int r = 0;
    if (dev->corr == ppm) return -2;
    dev->corr = ppm;
    r |= rtlsdr_set_sample_freq_correction(ppm);
    /* read corrected clock value into r82xx structure */
    if (rtlsdr_get_xtal_freq(NULL, &dev->r82xx_c.xtal)) return -3;
    if (dev->freq) /* retune to apply new correction value */
        r |= rtlsdr_set_center_freq(dev->freq);
    return r;
}

enum rtlsdr_tuner rtlsdr_get_tuner_type(void) { return dev->tuner_type; }

const char *rtlsdr_tuner_name(void)
{
    switch (dev->tuner_type) {
    case RTLSDR_TUNER_E4000:  return "E4000";
    case RTLSDR_TUNER_FC0012: return "FC0012";
    case RTLSDR_TUNER_FC0013: return "FC0013";
    case RTLSDR_TUNER_FC2580: return "FC2580";
    case RTLSDR_TUNER_R820T:  return "R820T";
    case RTLSDR_TUNER_R828D:  return "R828D";
    default: return "unknown";
    }
}

static const int r82xx_gains[] = { 0, 9, 14, 27, 37, 77, 87, 125, 144, 157,
                                   166, 197, 207, 229, 254, 280, 297, 328,
                                   338, 364, 372, 386, 402, 421, 434, 439,
                                   445, 480, 496 };

int rtlsdr_get_tuner_gains(int *gains)
{
    if (!tuner_is_r82xx()) return 0;
    int n = sizeof(r82xx_gains) / sizeof(int);
    if (gains) memcpy(gains, r82xx_gains, sizeof(r82xx_gains));
    return n;
}

int rtlsdr_set_tuner_bandwidth(uint32_t bw)
{
    int r = 0;
    if (!tuner_is_r82xx()) return -1;
    rtlsdr_set_i2c_repeater(1);
    r = tuner_set_bw(bw > 0 ? bw : dev->rate);
    rtlsdr_set_i2c_repeater(0);
    if (r) return r;
    dev->bw = bw;
    return r;
}

int rtlsdr_set_tuner_gain(int gain)
{
    int r = 0;
    if (!tuner_is_r82xx()) return -1;
    rtlsdr_set_i2c_repeater(1);
    r = tuner_set_gain(gain);
    rtlsdr_set_i2c_repeater(0);
    dev->gain = r ? 0 : gain;
    return r;
}

int rtlsdr_set_tuner_gain_by_index(unsigned index)
{
    int n = sizeof(r82xx_gains) / sizeof(int);
    if (!tuner_is_r82xx()) return -1;
    if (index >= (unsigned)n) index = n - 1;
    return rtlsdr_set_tuner_gain(r82xx_gains[index]);
}

int rtlsdr_set_tuner_if_gain(int stage, int gain)
{
    (void)stage; (void)gain;
    return 0; /* R82xx has no IF gain stages */
}

int rtlsdr_set_tuner_gain_mode(int mode)
{
    int r = 0;
    if (!tuner_is_r82xx()) return -1;
    rtlsdr_set_i2c_repeater(1);
    r = tuner_set_gain_mode(mode);
    rtlsdr_set_i2c_repeater(0);
    return r;
}

int rtlsdr_set_sample_rate(uint32_t samp_rate)
{
    int r = 0;
    uint16_t tmp;
    uint32_t rsamp_ratio, real_rsamp_ratio;
    double real_rate;

    /* check if the rate is supported by the resampler */
    if ((samp_rate <= 225000) || (samp_rate > 3200000) ||
        ((samp_rate > 300000) && (samp_rate <= 900000))) {
        printf("[rtl] Invalid sample rate: %lu Hz\n", (unsigned long)samp_rate);
        return -22;
    }

    rsamp_ratio = (dev->rtl_xtal * TWO_POW(22)) / samp_rate;
    rsamp_ratio &= 0x0ffffffc;

    real_rsamp_ratio = rsamp_ratio | ((rsamp_ratio & 0x08000000) << 1);
    real_rate = (dev->rtl_xtal * TWO_POW(22)) / real_rsamp_ratio;

    if (((double)samp_rate) != real_rate)
        printf("[rtl] Exact sample rate is: %lu Hz\n", (unsigned long)real_rate);

    dev->rate = (uint32_t)real_rate;

    if (tuner_is_r82xx()) {
        rtlsdr_set_i2c_repeater(1);
        tuner_set_bw(dev->bw > 0 ? dev->bw : dev->rate);
        rtlsdr_set_i2c_repeater(0);
    }

    tmp = (rsamp_ratio >> 16);
    r |= rtlsdr_demod_write_reg(1, 0x9f, tmp, 2);
    tmp = rsamp_ratio & 0xffff;
    r |= rtlsdr_demod_write_reg(1, 0xa1, tmp, 2);

    r |= rtlsdr_set_sample_freq_correction(dev->corr);

    /* reset demod (bit 3, soft_rst) */
    r |= rtlsdr_demod_write_reg(1, 0x01, 0x14, 1);
    r |= rtlsdr_demod_write_reg(1, 0x01, 0x10, 1);

    /* recalculate offset frequency if offset tuning is enabled */
    if (dev->offs_freq)
        rtlsdr_set_offset_tuning(1);

    return r;
}

uint32_t rtlsdr_get_sample_rate(void) { return dev->rate; }

int rtlsdr_set_testmode(int on)
{
    return rtlsdr_demod_write_reg(0, 0x19, on ? 0x03 : 0x05, 1);
}

int rtlsdr_set_agc_mode(int on)
{
    return rtlsdr_demod_write_reg(0, 0x19, on ? 0x25 : 0x05, 1);
}

int rtlsdr_set_direct_sampling(int on)
{
    int r = 0;
    if (on) {
        if (tuner_is_r82xx()) {
            rtlsdr_set_i2c_repeater(1);
            r = r82xx_standby(&dev->r82xx_p);
            rtlsdr_set_i2c_repeater(0);
        }
        /* disable Zero-IF mode */
        r |= rtlsdr_demod_write_reg(1, 0xb1, 0x1a, 1);
        /* disable spectrum inversion */
        r |= rtlsdr_demod_write_reg(1, 0x15, 0x00, 1);
        /* only enable In-phase ADC input */
        r |= rtlsdr_demod_write_reg(0, 0x08, 0x4d, 1);
        /* swap I and Q ADC, this allows to select between two inputs */
        r |= rtlsdr_demod_write_reg(0, 0x06, (on > 1) ? 0x90 : 0x80, 1);
        printf("[rtl] Enabled direct sampling mode, input %i\n", on);
        dev->direct_sampling = on;
    } else {
        if (tuner_is_r82xx()) {
            rtlsdr_set_i2c_repeater(1);
            r |= r820t_init();
            rtlsdr_set_i2c_repeater(0);
        }
        if (tuner_is_r82xx()) {
            r |= rtlsdr_set_if_freq(R82XX_IF_FREQ);
            /* enable spectrum inversion */
            r |= rtlsdr_demod_write_reg(1, 0x15, 0x01, 1);
        } else {
            r |= rtlsdr_set_if_freq(0);
            /* enable In-phase + Quadrature ADC input */
            r |= rtlsdr_demod_write_reg(0, 0x08, 0xcd, 1);
            /* Enable Zero-IF mode */
            r |= rtlsdr_demod_write_reg(1, 0xb1, 0x1b, 1);
        }
        /* opt_adc_iq = 0, default ADC_I/ADC_Q datapath */
        r |= rtlsdr_demod_write_reg(0, 0x06, 0x80, 1);
        printf("[rtl] Disabled direct sampling mode\n");
        dev->direct_sampling = 0;
    }
    r |= rtlsdr_set_center_freq(dev->freq);
    return r;
}

int rtlsdr_set_offset_tuning(int on)
{
    (void)on;
    if (tuner_is_r82xx()) return -2;   /* not applicable to R82xx */
    return -3;
}

int rtlsdr_set_xtal_freq(uint32_t rtl_freq, uint32_t tuner_freq)
{
    int r = 0;
    if (rtl_freq > 0 && (rtl_freq < MIN_RTL_XTAL_FREQ || rtl_freq > MAX_RTL_XTAL_FREQ))
        return -2;
    if (rtl_freq > 0 && dev->rtl_xtal != rtl_freq) {
        dev->rtl_xtal = rtl_freq;
        if (dev->rate) r = rtlsdr_set_sample_rate(dev->rate);
    }
    if (dev->tun_xtal != tuner_freq) {
        dev->tun_xtal = (tuner_freq == 0) ? dev->rtl_xtal : tuner_freq;
        if (rtlsdr_get_xtal_freq(NULL, &dev->r82xx_c.xtal)) return -3;
        if (dev->freq) r = rtlsdr_set_center_freq(dev->freq);
    }
    return r;
}

int rtlsdr_set_bias_tee(int on)
{
    return rtlsdr_set_bias_tee_gpio(dev, 0, on);
}

int rtlsdr_reset_buffer(void)
{
    rtlsdr_write_reg(USBB, USB_EPA_CTL, 0x1002, 2);
    rtlsdr_write_reg(USBB, USB_EPA_CTL, 0x0000, 2);
    return 0;
}

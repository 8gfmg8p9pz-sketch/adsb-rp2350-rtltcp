/*
 * usb_host.c -- TinyUSB host glue for the RTL2832U dongle on the RP2350 native USB port
 *
 *  - enumerates the dongle (vendor class, no class driver needed)
 *  - opens the bulk IN endpoint (0x81) on the patched EPX engine
 *  - blocking vendor control transfers with timeout (used by rtl2832u.c)
 *  - continuous bulk IN streaming into the shared ring buffer
 *
 * EPX hardware is shared between control transfers and the bulk pipe (see
 * tinyusb_patch/hcd_rp2040.c) so every control transfer first idles the stream.
 */
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "tusb.h"
#include "usb_host.h"
#include "config.h"

/* ------------------------------------------------------------------------- */
static volatile usbh_state_t s_state = USBH_NO_DEVICE;
static uint8_t  s_daddr = 0;
static uint8_t  s_ep_in = 0;
static uint16_t s_ep_mps = 64;
static uint16_t s_vid, s_pid;
static char     s_manufact[64];
static char     s_product[64];

/* streaming */
static ring_t  *s_ring = NULL;
static volatile bool s_stream_enabled = false;   /* application wants data */
static volatile bool s_stream_paused  = false;   /* temporarily idle for control xfers */
static volatile bool s_xfer_active    = false;   /* a bulk transfer is in flight */
static volatile uint32_t s_bytes_total = 0;
static volatile uint32_t s_errors = 0;
static volatile uint32_t s_consec_errors = 0;
static volatile bool     s_fault = false;          /* stream stopped itself after repeated errors */
static absolute_time_t   s_resubmit_at;            /* delayed re-submit after a failed transfer */
static volatile bool     s_resubmit_pending = false;
#define STREAM_MAX_CONSEC_ERRORS  20

/* provided by tinyusb_patch/hcd_rp2040.c */
extern void hcd_rp2040_bulk_reset_toggle(void);
static uint8_t CFG_TUSB_MEM_ALIGN s_xfer_buf[CFG_USB_XFER_BYTES];
static uint16_t s_xfer_len = CFG_USB_XFER_BYTES;   /* bytes per bulk transfer (<= buffer) */

/* control */
static volatile bool          s_ctrl_done;
static volatile xfer_result_t s_ctrl_result;
static volatile uint32_t      s_ctrl_len;

/* ------------------------------------------------------------------------- */
#ifndef BOARD_TUH_RHPORT
#define BOARD_TUH_RHPORT 0
#endif

void usb_host_init(void)
{
    tuh_init(BOARD_TUH_RHPORT);
}

static bool submit_bulk(void);
static void usb_mount_work(uint8_t daddr);
static volatile bool    s_mount_pending = false;
static volatile uint8_t s_mount_addr = 0;

void usb_host_task(void)
{
    tuh_task();
    if (s_mount_pending) { s_mount_pending = false; usb_mount_work(s_mount_addr); }
    if (s_resubmit_pending && time_reached(s_resubmit_at)) {
        s_resubmit_pending = false;
        if (s_stream_enabled && !s_stream_paused && !s_xfer_active && s_state != USBH_NO_DEVICE) {
            submit_bulk();
        }
    }
}

usbh_state_t usb_host_state(void)     { return s_state; }
uint8_t      usb_host_daddr(void)     { return s_daddr; }
uint16_t     usb_host_vid(void)       { return s_vid; }
uint16_t     usb_host_pid(void)       { return s_pid; }
const char  *usb_host_manufacturer(void) { return s_manufact; }
const char  *usb_host_product(void)   { return s_product; }

void usb_host_set_open(bool open)
{
    if (s_state == USBH_NO_DEVICE) return;
    s_state = open ? USBH_DEVICE_OPEN : USBH_DEVICE_READY;
}

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */
static void utf16_to_ascii(const uint16_t *utf16, size_t utf16_len, char *out, size_t out_len)
{
    size_t n = 0;
    for (size_t i = 0; i < utf16_len && n + 1 < out_len; i++) {
        uint16_t c = utf16[i];
        out[n++] = (c < 0x80) ? (char)c : '?';
    }
    out[n] = 0;
}

static void get_string(uint8_t daddr, bool manufacturer, char *out, size_t out_len)
{
    static uint16_t CFG_TUSB_MEM_ALIGN buf[64];
    memset(buf, 0, sizeof(buf));
    uint8_t r = manufacturer ? tuh_descriptor_get_manufacturer_string_sync(daddr, 0x0409, buf, sizeof(buf))
                             : tuh_descriptor_get_product_string_sync(daddr, 0x0409, buf, sizeof(buf));
    out[0] = 0;
    if (r != XFER_RESULT_SUCCESS) return;
    uint8_t blen = tu_u16_low(buf[0]);            /* bLength */
    if (blen < 2) return;
    utf16_to_ascii(buf + 1, (blen - 2) / 2, out, out_len);
}

/* ------------------------------------------------------------------------- */
/* mount / unmount                                                           */
static void usb_mount_work(uint8_t daddr)
{
    tuh_vid_pid_get(daddr, &s_vid, &s_pid);
    printf("[usb] device mounted addr=%u VID=%04x PID=%04x speed=%s\n", daddr, s_vid, s_pid,
           tuh_speed_get(daddr) == TUSB_SPEED_FULL ? "full" : "low");

    /* find the first bulk IN endpoint in configuration 1 */
    static uint8_t CFG_TUSB_MEM_ALIGN cfg[CFG_TUH_ENUMERATION_BUFSIZE];
    uint16_t want_len = 9;
    if (tuh_descriptor_get_configuration_sync(daddr, 0, cfg, 9) == XFER_RESULT_SUCCESS) {
        want_len = tu_le16toh(((tusb_desc_configuration_t const *)cfg)->wTotalLength);
        if (want_len > sizeof(cfg)) want_len = sizeof(cfg);
    }
    printf("[usb] config descriptor length %u\n", want_len);
    if (tuh_descriptor_get_configuration_sync(daddr, 0, cfg, want_len) != XFER_RESULT_SUCCESS) {
        printf("[usb] failed to read configuration descriptor\n");
        return;
    }
    tusb_desc_configuration_t const *desc_cfg = (tusb_desc_configuration_t const *)cfg;
    uint16_t total = tu_le16toh(desc_cfg->wTotalLength);
    if (total > sizeof(cfg)) total = sizeof(cfg);
    uint8_t const *p = cfg + sizeof(tusb_desc_configuration_t);
    uint8_t const *end = cfg + total;
    tusb_desc_endpoint_t ep_desc;
    bool found = false;
    while (p + 1 < end && p[0] >= 2) {
        if (p[1] == TUSB_DESC_ENDPOINT) {
            tusb_desc_endpoint_t const *e = (tusb_desc_endpoint_t const *)p;
            if (e->bmAttributes.xfer == TUSB_XFER_BULK && tu_edpt_dir(e->bEndpointAddress) == TUSB_DIR_IN) {
                memcpy(&ep_desc, e, sizeof(ep_desc));
                found = true;
                break;
            }
        }
        p += p[0];
    }
    if (!found) {
        printf("[usb] no bulk IN endpoint: not an RTL2832U?\n");
        return;
    }
    /* full-speed: max packet size is 64 even if the (high-speed) descriptor says 512 */
    uint16_t mps = tu_edpt_packet_size(&ep_desc);
    if (mps > 64 || mps == 0) {
        printf("[usb] clamping wMaxPacketSize %u -> 64\n", mps);
        ep_desc.wMaxPacketSize = tu_htole16(64);
    }
    s_ep_mps = 64;
    if (!tuh_edpt_open(daddr, &ep_desc)) {
        printf("[usb] tuh_edpt_open(0x%02x) failed\n", ep_desc.bEndpointAddress);
        return;
    }
    s_ep_in = ep_desc.bEndpointAddress;
    s_daddr = daddr;

    s_manufact[0] = 0;
    s_product[0] = 0;
    printf("[usb] bulk IN ep=0x%02x  \"%s\" / \"%s\"\n", s_ep_in, s_manufact, s_product);

    s_bytes_total = 0;
    s_errors = 0;
    s_state = USBH_DEVICE_READY;
}

void tuh_mount_cb(uint8_t daddr)
{
    s_mount_addr = daddr;
    s_mount_pending = true;
}

void tuh_umount_cb(uint8_t daddr)
{
    printf("[usb] device removed addr=%u\n", daddr);
    s_mount_pending = false;
    s_stream_enabled = false;
    s_xfer_active = false;
    s_state = USBH_NO_DEVICE;
    s_daddr = 0;
    s_ep_in = 0;
}

/* ------------------------------------------------------------------------- */
/* control transfers                                                         */
static void ctrl_complete_cb(tuh_xfer_t *xfer)
{
    s_ctrl_result = xfer->result;
    s_ctrl_len    = xfer->actual_len;
    s_ctrl_done   = true;
}

int usb_host_control(bool dir_in, uint8_t request, uint16_t value, uint16_t index,
                     uint8_t *data, uint16_t len)
{
    if (s_state == USBH_NO_DEVICE || s_daddr == 0) return -1;

    /* EPX is shared with the bulk pipe: make sure it is idle */
    bool was_streaming = s_stream_enabled && !s_stream_paused;
    if (was_streaming) usb_stream_pause();

    static uint8_t CFG_TUSB_MEM_ALIGN xbuf[64];
    if (len > sizeof(xbuf)) return -1;
    if (!dir_in && len) memcpy(xbuf, data, len);

    tusb_control_request_t const req = {
        .bmRequestType_bit = {
            .recipient = TUSB_REQ_RCPT_DEVICE,
            .type      = TUSB_REQ_TYPE_VENDOR,
            .direction = dir_in ? TUSB_DIR_IN : TUSB_DIR_OUT,
        },
        .bRequest = request,
        .wValue   = tu_htole16(value),
        .wIndex   = tu_htole16(index),
        .wLength  = tu_htole16(len),
    };
    tuh_xfer_t xfer = {
        .daddr       = s_daddr,
        .ep_addr     = 0,
        .setup       = &req,
        .buffer      = len ? xbuf : NULL,
        .complete_cb = ctrl_complete_cb,
        .user_data   = 0,
    };

    s_ctrl_done = false;
    s_ctrl_result = XFER_RESULT_INVALID;
    int ret = -1;
    if (tuh_control_xfer(&xfer)) {
        absolute_time_t deadline = make_timeout_time_ms(300);
        while (!s_ctrl_done) {
            tuh_task_ext(0, false);
            if (time_reached(deadline)) {
                printf("[usb] control xfer timeout (req=%u val=%04x idx=%04x)\n", request, value, index);
                tuh_edpt_abort_xfer(s_daddr, 0);
                break;
            }
            if (s_state == USBH_NO_DEVICE) break;
        }
        if (s_ctrl_done && s_ctrl_result == XFER_RESULT_SUCCESS) {
            if (dir_in && len) memcpy(data, xbuf, len);
            ret = (int)s_ctrl_len;
        }
    } else {
        /* a refused SETUP leaves usbh's control stage armed: reset it */
        tuh_edpt_abort_xfer(s_daddr, 0);
        printf("[usb] tuh_control_xfer refused\n");
    }

    if (was_streaming) usb_stream_resume();
    return ret;
}

/* ------------------------------------------------------------------------- */
/* bulk streaming                                                            */
static bool submit_bulk(void);

static void bulk_complete_cb(tuh_xfer_t *xfer)
{
    s_xfer_active = false;
    if (xfer->result == XFER_RESULT_SUCCESS) {
        s_consec_errors = 0;
        if (xfer->actual_len && s_ring) {
            ring_write(s_ring, s_xfer_buf, xfer->actual_len);
            s_bytes_total += xfer->actual_len;
        }
    } else {
        s_errors++;
        s_consec_errors++;
        if (xfer->result == XFER_RESULT_STALLED || s_consec_errors >= STREAM_MAX_CONSEC_ERRORS) {
            /* give up: main loop restarts the stream after a device-side reset */
            printf("[usb] bulk pipe fault (%s, %lu consecutive errors) -> stream stopped\n",
                   xfer->result == XFER_RESULT_STALLED ? "STALL" : "errors", (unsigned long)s_consec_errors);
            s_stream_enabled = false;
            s_fault = true;
            return;
        }
        /* back off a little before retrying (toggle resync, device FIFO refill) */
        s_resubmit_at = make_timeout_time_ms(2);
        s_resubmit_pending = true;
        return;
    }
    if (s_stream_enabled && !s_stream_paused && s_state != USBH_NO_DEVICE) {
        submit_bulk();
    }
}

static bool submit_bulk(void)
{
    tuh_xfer_t xfer = {
        .daddr       = s_daddr,
        .ep_addr     = s_ep_in,
        .buflen      = s_xfer_len,
        .buffer      = s_xfer_buf,
        .complete_cb = bulk_complete_cb,
        .user_data   = 0,
    };
    if (!tuh_edpt_xfer(&xfer)) {
        s_errors++;
        return false;
    }
    s_xfer_active = true;
    return true;
}

void usb_stream_set_ring(ring_t *ring) { s_ring = ring; }

void usb_stream_start(void)
{
    if (s_state != USBH_DEVICE_OPEN || s_ep_in == 0) return;
    s_stream_paused = false;
    s_fault = false;
    s_consec_errors = 0;
    s_resubmit_pending = false;
    s_stream_enabled = true;
    if (!s_xfer_active) submit_bulk();
}

void usb_stream_reset_toggle(void)
{
    hcd_rp2040_bulk_reset_toggle();
}

bool usb_stream_fault(void) { return s_fault; }

static void wait_idle(void)
{
    absolute_time_t deadline = make_timeout_time_ms(200);
    while (s_xfer_active && !time_reached(deadline) && s_state != USBH_NO_DEVICE) {
        tuh_task_ext(0, false);
    }
    if (s_xfer_active) {
        /* transfer did not finish (device stalled?) -- abort it in the HCD so the
         * endpoint can be claimed again and the EPX engine is idle */
        printf("[usb] bulk transfer did not complete, aborting\n");
        tuh_edpt_abort_xfer(s_daddr, s_ep_in);
        s_xfer_active = false;
        s_errors++;
    }
}

void usb_stream_stop(void)
{
    s_stream_enabled = false;
    wait_idle();
}

void usb_stream_pause(void)
{
    if (!s_stream_enabled) return;
    s_stream_paused = true;
    wait_idle();
}

/* like usb_stream_pause(), but drops the transfer in flight instead of waiting for it
 * (its data is stale after a retune anyway).  Follow with rtlsdr_reset_buffer() and
 * usb_stream_reset_toggle() so that both ends restart the pipe at DATA0. */
void usb_stream_pause_abort(void)
{
    if (!s_stream_enabled) return;
    s_stream_paused = true;
    s_resubmit_pending = false;
    if (s_xfer_active) {
        tuh_edpt_abort_xfer(s_daddr, s_ep_in);
        s_xfer_active = false;
    }
}

/* smaller transfers hand data over sooner after a retune (scanner), larger ones cost
 * fewer interrupts (rtl_tcp).  Multiple of 64, takes effect with the next transfer. */
void usb_stream_set_xfer_len(uint16_t len)
{
    if (len < 64) len = 64;
    if (len > sizeof(s_xfer_buf)) len = sizeof(s_xfer_buf);
    s_xfer_len = len & ~63u;
}

void usb_stream_resume(void)
{
    if (!s_stream_enabled) return;
    s_stream_paused = false;
    if (!s_xfer_active && s_state != USBH_NO_DEVICE) submit_bulk();
}

bool     usb_stream_running(void)     { return s_stream_enabled && !s_stream_paused; }
uint32_t usb_stream_bytes_total(void) { return s_bytes_total; }
uint32_t usb_stream_errors(void)      { return s_errors; }

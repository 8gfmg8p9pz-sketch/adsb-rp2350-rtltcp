/* TinyUSB host glue: RTL2832U enumeration, control transfers, bulk IQ streaming */
#ifndef USB_HOST_H
#define USB_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include "ring.h"

typedef enum {
    USBH_NO_DEVICE = 0,   /* nothing plugged / not enumerated */
    USBH_DEVICE_READY,    /* enumerated, endpoint opened, waiting for rtl2832u_open() */
    USBH_DEVICE_OPEN,     /* rtl2832u initialised, can stream */
} usbh_state_t;

void         usb_host_init(void);
void         usb_host_task(void);             /* call as often as possible from core0 */
usbh_state_t usb_host_state(void);
void         usb_host_set_open(bool open);    /* main.c marks device initialised */
uint8_t      usb_host_daddr(void);
uint16_t     usb_host_vid(void);
uint16_t     usb_host_pid(void);
const char  *usb_host_manufacturer(void);
const char  *usb_host_product(void);

/* vendor control transfer, returns bytes transferred or -1 */
int usb_host_control(bool dir_in, uint8_t request, uint16_t value, uint16_t index,
                     uint8_t *data, uint16_t len);

/* bulk IQ streaming into the ring buffer */
void     usb_stream_set_ring(ring_t *ring);
void     usb_stream_start(void);
void     usb_stream_stop(void);          /* stop and wait for the pipe to go idle */
void     usb_stream_pause(void);         /* temporarily idle the EPX engine (before control xfers) */
void     usb_stream_resume(void);
bool     usb_stream_running(void);
void     usb_stream_reset_toggle(void);  /* host-side data toggle -> DATA0 (after rtlsdr_reset_buffer) */
bool     usb_stream_fault(void);         /* stream stopped itself after repeated errors/STALL */
uint32_t usb_stream_bytes_total(void);
uint32_t usb_stream_errors(void);

#endif

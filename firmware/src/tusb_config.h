/* TinyUSB configuration: native USB port in HOST mode (RTL2832U dongle) */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined (pico-sdk sets it)
#endif

/* TUSB_DEBUG_NETLOG */
#undef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG 1
#define CFG_TUSB_RHPORT0_MODE     (OPT_MODE_HOST | OPT_MODE_FULL_SPEED)
#define CFG_TUH_ENABLED           1
#define CFG_TUD_ENABLED           0

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS               OPT_OS_PICO
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG            0
#endif

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif
#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

/* host stack */
#define CFG_TUH_ENUMERATION_BUFSIZE 256
#define CFG_TUH_HUB               0
#define CFG_TUH_DEVICE_MAX        1
#define CFG_TUH_CDC               0
#define CFG_TUH_HID               0
#define CFG_TUH_MSC               0
#define CFG_TUH_VENDOR            0
#define CFG_TUH_ENDPOINT_MAX      4

/* RTL2832U is a vendor-class device: we use the application driver hook */
#define CFG_TUH_API_EDPT_XFER     1

#ifdef __cplusplus
}
#endif

#endif

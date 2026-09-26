/* ota.c -- firmware update over TCP 1235 (stage at 2MB, CRC32 check, copy to 0, reboot) */
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/structs/watchdog.h"
#include "hardware/structs/psm.h"
#include "hardware/regs/addressmap.h"
#include "socket.h"
#include "ota.h"

#define SOCK_OTA      2
#define OTA_PORT      1235
#define OTA_STAGE_OFF 0x200000u
#define OTA_MAX_LEN   0x100000u
#define SECT          FLASH_SECTOR_SIZE

static uint8_t  s_buf[SECT] __attribute__((aligned(4)));
static uint8_t  s_hdr[12];
static uint32_t s_hdrn, s_len, s_crc, s_got, s_fill, s_sect;
static bool     s_est;

static uint32_t crc32_calc(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

static void stage_write_sector(uint32_t idx)
{
    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(OTA_STAGE_OFF + idx * SECT, SECT);
    flash_range_program(OTA_STAGE_OFF + idx * SECT, s_buf, SECT);
    restore_interrupts(ints);
    multicore_lockout_end_blocking();
}

static void __no_inline_not_in_flash_func(ota_copy_and_reboot)(uint32_t nsect)
{
    for (uint32_t i = 0; i < nsect; i++) {
        const volatile uint8_t *src = (const volatile uint8_t *)(XIP_BASE + OTA_STAGE_OFF + i * SECT);
        for (uint32_t j = 0; j < SECT; j++) s_buf[j] = src[j];
        flash_range_erase(i * SECT, SECT);
        flash_range_program(i * SECT, s_buf, SECT);
    }
    psm_hw->wdsel = PSM_WDSEL_BITS & ~(PSM_WDSEL_ROSC_BITS | PSM_WDSEL_XOSC_BITS);
    watchdog_hw->scratch[4] = 0;
    watchdog_hw->ctrl = WATCHDOG_CTRL_TRIGGER_BITS;
    while (1) { __asm volatile ("nop"); }
}

static void ota_reset(void) { s_hdrn = s_len = s_crc = s_got = s_fill = s_sect = 0; }
static void ota_reply(const char *m) { send(SOCK_OTA, (uint8_t *)m, (uint16_t)strlen(m)); }

static void ota_apply(uint32_t len)
{
    uint32_t nsect = (len + SECT - 1) / SECT;
    printf("[ota] applying %lu bytes (%lu sectors)\n", (unsigned long)len, (unsigned long)nsect);
    sleep_ms(50);
    multicore_lockout_start_blocking();
    (void)save_and_disable_interrupts();
    ota_copy_and_reboot(nsect);
}

void ota_poll(void)
{
    uint8_t sr = getSn_SR(SOCK_OTA);
    switch (sr) {
    case SOCK_CLOSED:
        s_est = false;
        ota_reset();
        socket(SOCK_OTA, Sn_MR_TCP4, OTA_PORT, 0);
        break;
    case SOCK_INIT:
        listen(SOCK_OTA);
        break;
    case SOCK_CLOSE_WAIT:
        disconnect(SOCK_OTA);
        break;
    case SOCK_ESTABLISHED: {
        if (!s_est) {
            s_est = true;
            ota_reset();
            ota_reply("RP2350-OTA " __DATE__ " " __TIME__ "\n");
        }
        uint16_t rsr = getSn_RX_RSR(SOCK_OTA);
        while (rsr > 0) {
            if (s_hdrn < 12) {
                uint16_t n = (uint16_t)(12 - s_hdrn);
                if (n > rsr) n = rsr;
                int32_t r = recv(SOCK_OTA, s_hdr + s_hdrn, n);
                if (r <= 0) return;
                s_hdrn += (uint32_t)r; rsr -= (uint16_t)r;
                if (s_hdrn == 12) {
                    if (memcmp(s_hdr, "OTA1", 4) != 0) { ota_reply("NG magic\n"); disconnect(SOCK_OTA); return; }
                    memcpy(&s_len, s_hdr + 4, 4);
                    memcpy(&s_crc, s_hdr + 8, 4);
                    if (s_len == 0 || s_len > OTA_MAX_LEN) { ota_reply("NG size\n"); disconnect(SOCK_OTA); return; }
                    printf("[ota] receiving %lu bytes\n", (unsigned long)s_len);
                }
                continue;
            }
            uint32_t want = SECT - s_fill;
            uint32_t left = s_len - s_got;
            if (want > left) want = left;
            if (want > rsr) want = rsr;
            int32_t r = recv(SOCK_OTA, s_buf + s_fill, (uint16_t)want);
            if (r <= 0) return;
            s_fill += (uint32_t)r; s_got += (uint32_t)r; rsr -= (uint16_t)r;
            if (s_fill == SECT || s_got == s_len) {
                if (s_fill < SECT) memset(s_buf + s_fill, 0xFF, SECT - s_fill);
                stage_write_sector(s_sect++);
                s_fill = 0;
            }
            if (s_got == s_len) {
                uint32_t c = crc32_calc((const uint8_t *)(XIP_BASE + OTA_STAGE_OFF), s_len);
                if (c != s_crc) { ota_reply("NG crc\n"); disconnect(SOCK_OTA); return; }
                ota_reply("OK applying\n");
                sleep_ms(300);
                disconnect(SOCK_OTA);
                sleep_ms(200);
                ota_apply(s_len);
            }
        }
        break;
    }
    default:
        break;
    }
}

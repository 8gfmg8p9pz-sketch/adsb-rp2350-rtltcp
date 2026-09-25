/**
    Copyright (c) 2022 WIZnet Co.,Ltd

    SPDX-License-Identifier: BSD-3-Clause
*/

/**
    ----------------------------------------------------------------------------------------------------
    Includes
    ----------------------------------------------------------------------------------------------------
*/
#include <stdio.h>
#include "DEV_Config.h"

#include "port_common.h"

#include "ethchip_conf.h"
#include "ethchip_spi.h"

#include "pico/stdlib.h"
#include "pico/binary_info.h"
#include "pico/critical_section.h"
#include "hardware/dma.h"

/**
    ----------------------------------------------------------------------------------------------------
    Macros
    ----------------------------------------------------------------------------------------------------
*/

/**
    ----------------------------------------------------------------------------------------------------
    Variables
    ----------------------------------------------------------------------------------------------------
*/
static critical_section_t g_ethchip_cri_sec;

#ifdef USE_SPI_DMA
static uint dma_tx;
static uint dma_rx;
static dma_channel_config dma_channel_config_tx;
static dma_channel_config dma_channel_config_rx;
#endif

/**
    ----------------------------------------------------------------------------------------------------
    Functions
    ----------------------------------------------------------------------------------------------------
*/
static inline void ethchip_select(void) {
    gpio_put(PIN_CS, 0);
}

static inline void ethchip_deselect(void) {
    gpio_put(PIN_CS, 1);
}

void ethchip_reset() {
    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);

    gpio_put(PIN_RST, 1);
    sleep_ms(100);

    gpio_put(PIN_RST, 0);
    sleep_ms(100);

    gpio_put(PIN_RST, 1);
    sleep_ms(100);

    bi_decl(bi_1pin_with_name(PIN_RST, "ETHCHIP RESET"));
}

static uint8_t ethchip_read(void) {
    uint8_t rx_data = 0;
    uint8_t tx_data = 0xFF;

    spi_read_blocking(SPI_PORT, tx_data, &rx_data, 1);

    return rx_data;
}

static void ethchip_write(uint8_t tx_data) {
    spi_write_blocking(SPI_PORT, &tx_data, 1);
}

#ifdef USE_SPI_DMA
static void ethchip_read_burst(uint8_t *pBuf, uint16_t len) {
    uint8_t dummy_data = 0xFF;

    channel_config_set_read_increment(&dma_channel_config_tx, false);
    channel_config_set_write_increment(&dma_channel_config_tx, false);
    dma_channel_configure(dma_tx, &dma_channel_config_tx,
                          &spi_get_hw(SPI_PORT)->dr, // write address
                          &dummy_data,               // read address
                          len,                       // element count (each element is of size transfer_data_size)
                          false);                    // don't start yet

    channel_config_set_read_increment(&dma_channel_config_rx, false);
    channel_config_set_write_increment(&dma_channel_config_rx, true);
    dma_channel_configure(dma_rx, &dma_channel_config_rx,
                          pBuf,                      // write address
                          &spi_get_hw(SPI_PORT)->dr, // read address
                          len,                       // element count (each element is of size transfer_data_size)
                          false);                    // don't start yet

    dma_start_channel_mask((1u << dma_tx) | (1u << dma_rx));
    dma_channel_wait_for_finish_blocking(dma_rx);
}

static void ethchip_write_burst(uint8_t *pBuf, uint16_t len) {
    uint8_t dummy_data;

    channel_config_set_read_increment(&dma_channel_config_tx, true);
    channel_config_set_write_increment(&dma_channel_config_tx, false);
    dma_channel_configure(dma_tx, &dma_channel_config_tx,
                          &spi_get_hw(SPI_PORT)->dr, // write address
                          pBuf,                      // read address
                          len,                       // element count (each element is of size transfer_data_size)
                          false);                    // don't start yet

    channel_config_set_read_increment(&dma_channel_config_rx, false);
    channel_config_set_write_increment(&dma_channel_config_rx, false);
    dma_channel_configure(dma_rx, &dma_channel_config_rx,
                          &dummy_data,               // write address
                          &spi_get_hw(SPI_PORT)->dr, // read address
                          len,                       // element count (each element is of size transfer_data_size)
                          false);                    // don't start yet

    dma_start_channel_mask((1u << dma_tx) | (1u << dma_rx));
    dma_channel_wait_for_finish_blocking(dma_rx);
}
#endif

static inline uint8_t sspi_transfer(uint8_t data) 
{
    uint8_t received;
    spi_write_read_blocking(SPI_PORT, &data, &received, 1);
    return received;
}

static inline void sspi_read_bytes(uint8_t* data, size_t len) 
{
    uint8_t dummy = 0xff;
    for (size_t i = 0; i < len; i++) {
        data[i] = sspi_transfer(dummy);
    }
}

static inline void sspi_write_bytes(const uint8_t* data, size_t len) 
{
    spi_write_blocking(SPI_PORT, data, len);
}

void ethchip_sspi_read(uint8_t opcode, uint16_t addr, uint8_t* pBuf, uint16_t len) 
{
    uint8_t cmd[4];
    cmd[0] = opcode;
    cmd[1] = (addr >> 8) & 0xFF;
    cmd[2] = addr & 0xFF;       
    cmd[3] = 0x00;
    
#ifdef USE_SPI_DMA
    ethchip_write_burst(cmd, sizeof(cmd));
    ethchip_read_burst(pBuf, len);
#else
    sspi_write_bytes(cmd, sizeof(cmd));
    sspi_read_bytes(pBuf, len);
#endif

}

void ethchip_sspi_write(uint8_t opcode, uint16_t addr, uint8_t* pBuf, uint16_t len) 
{
    uint8_t cmd[4];
    cmd[0] = opcode;
    cmd[1] = (addr >> 8) & 0xFF;
    cmd[2] = addr & 0xFF;
    cmd[3] = 0x00;
    
#ifdef USE_SPI_DMA
    ethchip_write_burst(cmd, sizeof(cmd));
    ethchip_write_burst(pBuf, len);
#else
    sspi_write_bytes(cmd, sizeof(cmd));
    sspi_write_bytes(pBuf, len);
#endif

}

static void ethchip_critical_section_lock(void) {
    critical_section_enter_blocking(&g_ethchip_cri_sec);
}

static void ethchip_critical_section_unlock(void) {
    critical_section_exit(&g_ethchip_cri_sec);
}

void ethchip_spi_initialize(void) {
    // this example will use SPI0 at 5MHz
    spi_init(SPI_PORT, SPI_CLK * 1000 * 1000);

    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MISO, GPIO_FUNC_SPI);

    // make the SPI pins available to picotool
    bi_decl(bi_3pins_with_func(PIN_MISO, PIN_MOSI, PIN_SCK, GPIO_FUNC_SPI));

    // chip select is active-low, so we'll initialise it to a driven-high state
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    // make the SPI pins available to picotool
    bi_decl(bi_1pin_with_name(PIN_CS, "W6x00 CHIP SELECT"));

#ifdef USE_SPI_DMA
    dma_tx = dma_claim_unused_channel(true);
    dma_rx = dma_claim_unused_channel(true);

    dma_channel_config_tx = dma_channel_get_default_config(dma_tx);
    channel_config_set_transfer_data_size(&dma_channel_config_tx, DMA_SIZE_8);
    channel_config_set_dreq(&dma_channel_config_tx, DREQ_SPI0_TX);

    // We set the inbound DMA to transfer from the SPI receive FIFO to a memory buffer paced by the SPI RX FIFO DREQ
    // We coinfigure the read address to remain unchanged for each element, but the write
    // address to increment (so data is written throughout the buffer)
    dma_channel_config_rx = dma_channel_get_default_config(dma_rx);
    channel_config_set_transfer_data_size(&dma_channel_config_rx, DMA_SIZE_8);
    channel_config_set_dreq(&dma_channel_config_rx, DREQ_SPI0_RX);
    channel_config_set_read_increment(&dma_channel_config_rx, false);
    channel_config_set_write_increment(&dma_channel_config_rx, true);
#endif
}

void ethchip_cris_initialize(void) {
    critical_section_init(&g_ethchip_cri_sec);
    reg_ethchip_cris_cbfunc(ethchip_critical_section_lock, ethchip_critical_section_unlock);
}

void ethchip_initialize(void) {
    /* Deselect the FLASH : chip select high */
    ethchip_deselect();
    /* CS function register */
    reg_ethchip_cs_cbfunc(ethchip_select, ethchip_deselect);
    /* SPI function register */

#if   (_NETCHIP_ == W6300)
    reg_ethchip_qspi_cbfunc(ethchip_sspi_read, ethchip_sspi_write);
#else

    reg_ethchip_spi_cbfunc(ethchip_read, ethchip_write);

#ifdef USE_SPI_DMA
    reg_ethchip_spiburst_cbfunc(ethchip_read_burst, ethchip_write_burst);
#endif

#endif

    /* W6x00 initialize */
    uint8_t temp;
    uint8_t memsize[2][8] = {{4, 4, 4, 4, 4, 4, 4, 4}, {4, 4, 4, 4, 4, 4, 4, 4}};

    if (ctlethchip(CW_INIT_NETCHIP, (void *)memsize) == -1) {
        printf(" W6300 initialized fail\n");
        return;
    }
    /* Check PHY link status */
    do {
        if (ctlethchip(CW_GET_PHYLINK, (void *)&temp) == -1) {
            printf(" Unknown PHY link status\n");

            return;
        }
    } while (temp == PHY_LINK_OFF);
}


/* rtl_tcp: same as ethchip_initialize() but returns immediately (link is polled by the app)
 * and gives socket 0 (the IQ stream) a 16 KB TX/RX buffer. */
void ethchip_initialize_nowait(void) {
    ethchip_deselect();
    reg_ethchip_cs_cbfunc(ethchip_select, ethchip_deselect);
#if   (_NETCHIP_ == W6300)
    reg_ethchip_qspi_cbfunc(ethchip_sspi_read, ethchip_sspi_write);
#else
    reg_ethchip_spi_cbfunc(ethchip_read, ethchip_write);
#endif
    uint8_t memsize[2][8] = {{16, 2, 2, 2, 2, 2, 2, 4}, {16, 2, 2, 2, 2, 2, 2, 4}};
    if (ctlethchip(CW_INIT_NETCHIP, (void *)memsize) == -1) {
        printf(" W6300 initialized fail\n");
    }
}

void ethchip_check(void) {
    /* Read version register */
    if (getCIDR() != 0x6300) {
        printf(" ACCESS ERR : VERSION != 0x6100, read value = 0x%02x\n", getCIDR());

        while (1)
            ;
    }
}

/* Network */
void network_initialize(eth_NetInfo net_info) {
    uint8_t syslock = SYS_NET_LOCK;
    ctlethchip(CW_SYS_UNLOCK, &syslock);
    ctlnetwork(CN_SET_NETINFO, (void *)&net_info);
}

void print_network_information(eth_NetInfo net_info) {
    uint8_t tmp_str[8] = {
        0,
    };

    ctlnetwork(CN_GET_NETINFO, (void *)&net_info);
    ctlethchip(CW_GET_ID, (void *)tmp_str);
    printf("==========================================================\n");
    printf(" %s network configuration\n\n", (char *)tmp_str);

    printf(" MAC         : %02X:%02X:%02X:%02X:%02X:%02X\n", net_info.mac[0], net_info.mac[1], net_info.mac[2], net_info.mac[3], net_info.mac[4], net_info.mac[5]);
    printf(" IP          : %d.%d.%d.%d\n", net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3]);
    printf(" Subnet Mask : %d.%d.%d.%d\n", net_info.sn[0], net_info.sn[1], net_info.sn[2], net_info.sn[3]);
    printf(" Gateway     : %d.%d.%d.%d\n", net_info.gw[0], net_info.gw[1], net_info.gw[2], net_info.gw[3]);
    printf(" DNS         : %d.%d.%d.%d\n", net_info.dns[0], net_info.dns[1], net_info.dns[2], net_info.dns[3]);
    print_ipv6_addr(" GW6 ", net_info.gw6);
    print_ipv6_addr(" LLA ", net_info.lla);
    print_ipv6_addr(" GUA ", net_info.gua);
    print_ipv6_addr(" SUB6", net_info.sn6);
    print_ipv6_addr(" DNS6", net_info.dns6);
    printf("==========================================================\n\n");
}

void print_ipv6_addr(uint8_t* name, uint8_t* ip6addr) {
    printf("%s        : ", name);
    printf("%04X:%04X", ((uint16_t)ip6addr[0] << 8) | ((uint16_t)ip6addr[1]), ((uint16_t)ip6addr[2] << 8) | ((uint16_t)ip6addr[3]));
    printf(":%04X:%04X", ((uint16_t)ip6addr[4] << 8) | ((uint16_t)ip6addr[5]), ((uint16_t)ip6addr[6] << 8) | ((uint16_t)ip6addr[7]));
    printf(":%04X:%04X", ((uint16_t)ip6addr[8] << 8) | ((uint16_t)ip6addr[9]), ((uint16_t)ip6addr[10] << 8) | ((uint16_t)ip6addr[11]));
    printf(":%04X:%04X\r\n", ((uint16_t)ip6addr[12] << 8) | ((uint16_t)ip6addr[13]), ((uint16_t)ip6addr[14] << 8) | ((uint16_t)ip6addr[15]));
}



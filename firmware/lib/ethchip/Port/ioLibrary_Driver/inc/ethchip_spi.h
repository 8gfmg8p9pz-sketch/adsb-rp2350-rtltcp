/**
    Copyright (c) 2022 WIZnet Co.,Ltd

    SPDX-License-Identifier: BSD-3-Clause
*/

#ifndef _ETHCHIP_SPI_H_
#define _ETHCHIP_SPI_H_

#include "DEV_Config.h"

/**
    ----------------------------------------------------------------------------------------------------
    Macros
    ----------------------------------------------------------------------------------------------------
*/
/* SPI */
#define SPI_PORT    ETHCHIP_SPI_PORT
#define SPI_CLK     ETHCHIP_SPI_CLK

#define PIN_SCK     ETHCHIP_PIN_SCK
#define PIN_MOSI    ETHCHIP_PIN_MOSI
#define PIN_MISO    ETHCHIP_PIN_MISO
#define PIN_CS      ETHCHIP_PIN_CS
#define PIN_RST     ETHCHIP_PIN_RST
#define PIN_INT     ETHCHIP_PIN_INT

/* Use SPI DMA */
#undef USE_SPI_DMA // if you want to use SPI DMA, uncomment.

/**
    ----------------------------------------------------------------------------------------------------
    Functions
    ----------------------------------------------------------------------------------------------------
*/
/* ethchip */
/*! \brief Set CS pin
    \ingroup ethchip_spi

    Set chip select pin of spi0 to low(Active low).

    \param none
*/
static inline void ethchip_select(void);

/*! \brief Set CS pin
    \ingroup ethchip_spi

    Set chip select pin of spi0 to high(Inactive high).

    \param none
*/
static inline void ethchip_deselect(void);

/*! \brief Read from an SPI device, blocking
    \ingroup ethchip_spi

    Set spi_read_blocking function.
    Read byte from SPI to rx_data buffer.
    Blocks until all data is transferred. No timeout, as SPI hardware always transfers at a known data rate.

    \param none
*/
static uint8_t ethchip_read(void);

/*! \brief Write to an SPI device, blocking
    \ingroup ethchip_spi

    Set spi_write_blocking function.
    Write byte from tx_data buffer to SPI device.
    Blocks until all data is transferred. No timeout, as SPI hardware always transfers at a known data rate.

    \param tx_data Buffer of data to write
*/
static void ethchip_write(uint8_t tx_data);

#ifdef USE_SPI_DMA
/*! \brief Configure all DMA parameters and optionally start transfer
    \ingroup ethchip_spi

    Configure all DMA parameters and read from DMA

    \param pBuf Buffer of data to read
    \param len element count (each element is of size transfer_data_size)
*/
static void ethchip_read_burst(uint8_t *pBuf, uint16_t len);

/*! \brief Configure all DMA parameters and optionally start transfer
    \ingroup ethchip_spi

    Configure all DMA parameters and write to DMA

    \param pBuf Buffer of data to write
    \param len element count (each element is of size transfer_data_size)
*/
static void ethchip_write_burst(uint8_t *pBuf, uint16_t len);
#endif

/*! \brief Enter a critical section
    \ingroup ethchip_spi

    Set ciritical section enter blocking function.
    If the spin lock associated with this critical section is in use, then this
    method will block until it is released.

    \param none
*/
static void ethchip_critical_section_lock(void);

/*! \brief Release a critical section
    \ingroup ethchip_spi

    Set ciritical section exit function.
    Release a critical section.

    \param none
*/
static void ethchip_critical_section_unlock(void);

/*! \brief Initialize SPI instances and Set DMA channel
    \ingroup ethchip_spi

    Set GPIO to spi0.
    Puts the SPI into a known state, and enable it.
    Set DMA channel completion channel.

    \param none
*/
void ethchip_spi_initialize(void);

/*! \brief Initialize a critical section structure
    \ingroup ethchip_spi

    The critical section is initialized ready for use.
    Registers callback function for critical section for WIZchip.

    \param none
*/
void ethchip_cris_initialize(void);

/*! \brief wizchip reset
    \ingroup ethchip_spi

    Set a reset pin and reset.

    \param none
*/
void ethchip_reset(void);

/*! \brief Initialize WIZchip
    \ingroup ethchip_spi

    Set callback function to read/write byte using SPI & QSPI.
    Set callback function for WIZchip select/deselect.
    Set memory size of wizchip and monitor PHY link status.

    \param none
*/
void ethchip_initialize(void);
void ethchip_initialize_nowait(void);

/*! \brief Check chip version
    \ingroup ethchip_spi

    Get version information.

    \param none
*/
void ethchip_check(void);

/* Network */
/*! \brief Initialize network
    \ingroup ethchip_spi

    Set network information.

    \param net_info network information.
*/
void network_initialize(eth_NetInfo net_info);

/*! \brief Print network information
    \ingroup ethchip_spi

    Print network information about MAC address, IP address, Subnet mask, Gateway, DHCP and DNS address.

    \param net_info network information.
*/
void print_network_information(eth_NetInfo net_info);

/*! \brief Print IPv6 Address
    \ingroup ethchip_spi

    Print IPv6 Address.

    \param net_info network information.
*/
void print_ipv6_addr(uint8_t* name, uint8_t* ip6addr);

#endif /* _ETHCHIP_SPI_H_ */

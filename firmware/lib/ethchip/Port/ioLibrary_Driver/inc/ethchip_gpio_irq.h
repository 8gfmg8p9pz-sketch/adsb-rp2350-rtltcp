/**
    Copyright (c) 2022 WIZnet Co.,Ltd

    SPDX-License-Identifier: BSD-3-Clause
*/

#include "ethchip_spi.h"

#ifndef _ETHCHIP_GPIO_IRQ_H_
#define _ETHCHIP_GPIO_IRQ_H_

/**
    ----------------------------------------------------------------------------------------------------
    Macros
    ----------------------------------------------------------------------------------------------------
*/

/**
    ----------------------------------------------------------------------------------------------------
    Functions
    ----------------------------------------------------------------------------------------------------
*/
/* GPIO */
/*! \brief Initialize ethchip gpio interrupt callback function
    \ingroup ethchip_gpio_irq

    Add a ethchip interrupt callback.

    \param socket socket number
    \param callback the gpio interrupt callback function
*/
void ethchip_gpio_interrupt_initialize(uint8_t socket, void (*callback)(void));

/*! \brief Assign gpio interrupt callback function
    \ingroup ethchip_gpio_irq

    GPIO interrupt callback function.

    \param gpio Which GPIO caused this interrupt
    \param events Which events caused this interrupt. See \ref gpio_set_irq_enabled for details.
*/
static void ethchip_gpio_interrupt_callback(uint gpio, uint32_t events);

#endif /* _ETHCHIP_GPIO_IRQ_H_ */

/*****************************************************************************
* | File      	:   DEV_Config.h
* | Author      :   
* | Function    :   Hardware underlying interface
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2026-03-23
* | Info        :   
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of theex Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
******************************************************************************/
#ifndef _DEV_CONFIG_H_
#define _DEV_CONFIG_H_

#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/i2c.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"

/* system clock config  */
#define PLL_SYS_KHZ         150 * 1000

/* spi config */
#define HW_SPI_PORT         spi1

/* ethchip config */
#define ETHCHIP_SPI_PORT    spi0
#define ETHCHIP_SPI_CLK     70  /* Mhz */

#define ETHCHIP_PIN_SCK     18
#define ETHCHIP_PIN_MOSI    19
#define ETHCHIP_PIN_MISO    16
#define ETHCHIP_PIN_CS      17
#define ETHCHIP_PIN_RST     20
#define ETHCHIP_PIN_INT     21

/* i2c config */
#define I2C_PORT            i2c0

/*------------------------------------------------------------------------------------------------------*/
void DEV_Digital_Write(uint16_t Pin, uint8_t Value);
uint8_t DEV_Digital_Read(uint16_t Pin);

void DEV_GPIO_Mode(uint16_t Pin, uint16_t Mode);
void DEV_KEY_Config(uint16_t Pin);
void DEV_Digital_Write(uint16_t Pin, uint8_t Value);
uint8_t DEV_Digital_Read(uint16_t Pin);

void DEV_SPI_WriteByte(uint8_t Value);
void DEV_SPI_Write_nByte(uint8_t *pData, uint32_t Len);

void DEV_Delay_ms(uint32_t xms);
void DEV_Delay_us(uint32_t xus);

void DEV_I2C_Write_Byte(uint8_t addr, uint8_t reg, uint8_t Value);
void DEV_I2C_Write_nByte(uint8_t addr, uint8_t *pData, uint32_t Len);
uint8_t DEV_I2C_Read_Byte(uint8_t addr, uint8_t reg);
void DEV_I2C_Read_nByte(uint8_t addr,uint8_t reg, uint8_t *pData, uint32_t Len);

void DEV_IRQ_SET(uint gpio, uint32_t events, gpio_irq_callback_t callback);

void DEV_SET_PWM(uint8_t Value);

uint8_t DEV_Module_Init(void);
void DEV_Module_Exit(void);

#endif

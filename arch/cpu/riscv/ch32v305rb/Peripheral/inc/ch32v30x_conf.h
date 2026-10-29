/********************************** (C) COPYRIGHT *******************************
* File Name          : ch32v30x_conf.h
* Author             : WCH (Quilla: it.h include removed)
* Version            : V1.0.0
* Date               : 2021/06/06
* Description        : Library configuration file.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#ifndef __CH32V30x_CONF_H
#define __CH32V30x_CONF_H

/* Derived from the WCH EVT ch32v30x_conf.h. The upstream file also includes
 * "ch32v30x_it.h", which is a per-project interrupt-prototype header that in
 * turn pulls in debug.h and re-declares every IRQn handler. Quilla defines its
 * own handlers (USART2_IRQHandler, SysTick_Handler, TIM1_UP_IRQHandler,
 * DMA1_Channel6_IRQHandler) with the WCH fast-interrupt attribute, so that
 * include is dropped here to avoid a prototype clash and a debug.h dependency
 * the boot images do not want. Add it back only if a project needs the plain
 * prototypes. */
#include "ch32v30x_adc.h"
#include "ch32v30x_bkp.h"
#include "ch32v30x_can.h"
#include "ch32v30x_crc.h"
#include "ch32v30x_dac.h"
#include "ch32v30x_dbgmcu.h"
#include "ch32v30x_dma.h"
#include "ch32v30x_exti.h"
#include "ch32v30x_flash.h"
#include "ch32v30x_fsmc.h"
#include "ch32v30x_gpio.h"
#include "ch32v30x_i2c.h"
#include "ch32v30x_iwdg.h"
#include "ch32v30x_pwr.h"
#include "ch32v30x_rcc.h"
#include "ch32v30x_rtc.h"
#include "ch32v30x_sdio.h"
#include "ch32v30x_spi.h"
#include "ch32v30x_tim.h"
#include "ch32v30x_usart.h"
#include "ch32v30x_wwdg.h"
#include "ch32v30x_misc.h"

#endif /* __CH32V30x_CONF_H */

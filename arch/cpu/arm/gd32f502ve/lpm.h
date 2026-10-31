/*
 * Copyright (c) 2026, JiChiRen Info. Tech.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * This file is part of the Quilla operating system.
 */

/**
 * \file
 * Low-power modes for the GD32F502VE port.
 *
 * Only PM0 is implemented:
 *
 *   - PM0 - Sleep: `__WFI()` gates the core clock only. Every peripheral
 *     clock keeps running, so USART0, its DMA RX channel and the IDLE
 *     interrupt stay live and any NVIC interrupt wakes the core. No clock is stopped, so RTIMER_NOW(),
 *     clock_time() and SysTick all stay coherent across the sleep and nothing
 *     has to be restored on wake-up. Safe to enter unconditionally.
 *
 * Deep-sleep / standby are deliberately NOT implemented. A deep-sleep entry
 * would stop the APB clocks (and with them USART0 RX DMA, the console and the
 * rtimer's TIMER0), and this port has no verified wake-up source for it yet.
 * `LPM_CONF_MAX_PM` above 0 is rejected at compile time rather than silently
 * degrading to Sleep, so nobody can believe they are saving power they are
 * not.
 */

#ifndef LPM_H_
#define LPM_H_

#include "contiki.h"
#include "rtimer.h"

#include <stdbool.h>
#include <stdint.h>

/* LPM_SHELL_ENABLED is retained for compatibility; the "lpm" shell command
 * was removed, so this guard no longer gates any shell registration. */
#ifndef LPM_CONF_SHELL
#define LPM_CONF_SHELL              1
#endif
#ifndef LPM_SHELL_ENABLED
#define LPM_SHELL_ENABLED           LPM_CONF_SHELL
#endif

/*---------------------------------------------------------------------------*/
/**
 * \name LPM stats
 * @{
 */
/** \brief Number of times the core entered PM0 */
extern uint32_t lpm_stats_sleeps;
/** @} */

/*---------------------------------------------------------------------------*/
/**
 * \name LPM control
 * @{
 */

/** \brief Initialise the LPM module and its statistics */
void lpm_init(void);

/** \brief Enter the lowest power mode that is safe right now */
void lpm_enter(void);

/** \brief Exit a low-power mode (no-op for PM0: nothing was stopped) */
void lpm_exit(void);


/** @} */

#endif /* LPM_H_ */

/**
 * @}
 * @}
 */

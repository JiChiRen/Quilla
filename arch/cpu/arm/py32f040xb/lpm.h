/*
 * Copyright (c) 2025, JiChiRen Info. Tech.
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
 * Low-power modes for Quilla Arm M0 (PY32F040xB).
 *
 * The PY32F040xB offers, in rising order of saving:
 *
 *   - PM0 - Sleep: the core clock is gated, every peripheral clock keeps
 *     running and any interrupt in the NVIC wakes the core. The system
 *     clock does not stop, so RTIMER_NOW(), clock_time() and the SysTick
 *     all stay coherent across the sleep. Nothing has to be restored on
 *     wake-up. This mode is always safe to enter.
 *
 *   - PM1 - Stop 0: every clock in the VCORE domain stops except LSI and
 *     LSE; the PLL, HSI and HSE are switched off; SRAM and registers are
 *     retained. Wake-up restores HSI as the system clock. Because the
 *     SysTick stops, the software clock has to be re-synchronised against
 *     the rtimer on exit, and the sleep has to fit inside the watchdog
 *     window (the IWDG keeps counting while the core is stopped).
 *
 * There is deliberately no PM2. Standby drops the VCORE domain, which on
 * this part means losing SRAM and resetting on wake-up; a unit that comes
 * back through reset has no way to tell that from a fault, and this
 * project's safe state is entered from software, not from a reset handler.
 * LPM_CONF_MAX_PM above 1 is rejected at compile time.
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
 *
 * Time spent in each power mode, in rtimer ticks. Kept for bring-up: the
 * numbers are the only way to tell "the unit is sleeping" from "the unit
 * thinks it is sleeping".
 * @{
 */
#if LPM_CONF_STATS
extern rtimer_clock_t lpm_stats[3];

/**
 * \brief Read the time spent in a PM in rtimer ticks
 * \param pm The pm as a value in [0,2]
 */
#define LPM_STATS_GET(pm)        lpm_stats[pm]
#else
#define LPM_STATS_GET(pm)
#endif
/** @} */
/*---------------------------------------------------------------------------*/
/**
 * \name Constants to be used as arguments to lpm_set_max_pm()
 * @{
 */
#define LPM_PM0           0   /**< Sleep.                                   */
#define LPM_PM1           1   /**< Stop 0 (SRAM retained, HSI off).         */
/** @} */
/*---------------------------------------------------------------------------*/
#if LPM_CONF_MAX_PM > LPM_PM1
#error "LPM_CONF_MAX_PM above 1 is not supported: PY32F040xB Standby loses SRAM"
#endif
/*---------------------------------------------------------------------------*/
typedef bool (*lpm_periph_permit_pm1_func_t)(void);

#if LPM_CONF_ENABLE
/**
 * \brief Initialise the LPM module
 *
 * Selects the Stop-mode wake-up timings and starts in PM0. Does not change
 * which PM may be used: that is LPM_CONF_MAX_PM plus lpm_set_max_pm().
 */
void lpm_init(void);

/**
 * \brief Drop to the lowest power mode the current state allows
 *
 * Called from the idle loop once every event has been serviced. The
 * decision is made in this order, and the first failing test wins:
 *
 *   - max_pm == 0, or a registered peripheral refuses PM1, or there are
 *     pending process events, or no rtimer is scheduled -> PM0 (Sleep);
 *   - the earliest wake-up the hardware can actually deliver is later
 *     than the watchdog allows -> PM0;
 *   - otherwise PM1 (Stop 0).
 *
 * The watchdog test deserves a note because it is the one that decides
 * whether this unit resets or not. The IWDG is clocked from the LSI and
 * keeps running in Stop, so the longest admissible sleep is bounded by
 * qwdt_timeout_min_ms(), not by the rtimer target: the rtimer target says
 * when the software would like to wake up, which is not the same as when
 * the hardware can wake it up. lpm_enter() uses the latter.
 */
void lpm_enter(void);

/**
 * \brief Perform the wake-up sequence for PM1
 *
 * Called from the ISR that brought the unit out of Stop, before control
 * returns to the instruction after the WFI. Restores the system clock to
 * HSI, re-synchronises the software clock (the SysTick stopped) and closes
 * the energest and statistics windows.
 *
 * Returns immediately when the unit was in PM0: Sleep stops no clock, so
 * there is nothing to restore.
 *
 * \note This function must be called at the beginning of every ISR that
 * can wake the core from Stop.
 */
void lpm_exit(void);

/**
 * \brief Prevent the SoC from dropping to a PM higher than \e max_pm
 * \param pm The highest PM we are allowed to enter, specified as
 *        LPM_PM0 or LPM_PM1
 *
 * PM0 cannot be disabled at run time. Use LPM_CONF_ENABLE to remove LPM
 * support from an image altogether.
 *
 * \note A value above LPM_CONF_MAX_PM is clamped to LPM_CONF_MAX_PM.
 */
void lpm_set_max_pm(uint8_t pm);

/**
 * \brief Register a peripheral function which will get called by the LPM
 * module to get 'permission' to drop to PM1
 * \param permit_pm1_func Pointer to the function
 *
 * A peripheral clocked from something that stops in Stop mode - the USART
 * and its DMA in this port - loses data if the core sleeps under it. Its
 * function returns true to permit PM1 and false to force PM0. One refusal
 * is enough. Registering the same function twice counts once.
 */
void lpm_register_peripheral(lpm_periph_permit_pm1_func_t permit_pm1_func);

/** \brief Highest PM currently allowed. */
unsigned int lpm_max_pm(void);

/** \brief Number of times PM1 was considered and refused. */
unsigned long lpm_stop_refusals(void);

/**
 * \brief Reason the last PM1 decision was refused
 * \return A short static string.
 */
const char *lpm_stop_refusal_reason(void);

/*---------------------------------------------------------------------------*/
/* Disable the entire module if required */
#else
#define lpm_init()
#define lpm_enter()
#define lpm_exit()
static inline void
lpm_set_max_pm(uint8_t pm)
{
  (void)pm;
}
static inline void
lpm_register_peripheral(lpm_periph_permit_pm1_func_t permit_pm1_func)
{
  (void)permit_pm1_func;
}
static inline unsigned int
lpm_max_pm(void)
{
  return 0U;
}
static inline unsigned long
lpm_stop_refusals(void)
{
  return 0UL;
}
static inline const char *
lpm_stop_refusal_reason(void)
{
  return "disabled";
}
#endif /* LPM_CONF_ENABLE */

#endif /* LPM_H_ */

/**
 * @}
 * @}
 */

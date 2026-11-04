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
 * 3. Neither the name of the copyright holder nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
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
 * Low-power mode driver for Quilla Arm M0 (PY32F040xB).
 *
 * WHAT THIS PORT ACTUALLY USES
 * ----------------------------
 *   PM0 - Sleep:  HAL_PWR_EnterSLEEPMode(). The core clock is gated and
 *                 everything else keeps running, so no state has to be
 *                 restored and the watchdog is unaffected.
 *
 *   PM1 - Stop 0: HAL_PWR_EnterSTOPMode(PWR_MAINREGULATOR_ON, ...). Every
 *                 clock in the VCORE domain stops except LSI/LSE, which is
 *                 what keeps the rtimer (LPTIM1 on LSI) alive across the
 *                 sleep. Wake-up restores HSI as the system clock.
 *
 * WHY PM1 IS REFUSED BY DEFAULT
 * -----------------------------
 * Two facts decide it, and neither is a matter of taste:
 *
 *   1. The rtimer drives LPTIM1 in continue mode with ARR = 0xFFFF and
 *      compares the target in software, inside the autoreload-match
 *      callback. The hardware therefore produces an interrupt once per
 *      65536 LSI ticks - about two seconds - no matter how near the
 *      scheduled target is. Stop cannot be left any earlier than that.
 *
 *   2. The IWDG is clocked from the LSI and keeps counting in Stop. With
 *      the measured LSI the shortest timeout is a few hundred
 *      milliseconds.
 *
 * Two seconds is longer than a few hundred milliseconds, so a Stop entered
 * on this rtimer ends in a watchdog reset. lpm_enter() therefore compares
 * the moment the hardware can wake the core - not the moment the software
 * would like to be woken - against the watchdog window, and takes PM0
 * whenever the comparison fails. PM1 becomes available the moment the
 * rtimer can raise an interrupt at the target (an LPTIM compare-match
 * wake-up), with no change to this decision logic.
 *
 * On top of that, LPM_CONF_STOP_WAKEUP_VERIFIED gates the whole PM1 path
 * at compile time: it stays 0 until this board has been shown to come out
 * of Stop on the LPTIM interrupt. Until then the PM1 code is not even
 * built, which also keeps it out of the ROM budget.
 */

#include "contiki.h"
#include "sys/energest.h"
#include "sys/process.h"
#include "sys/kern-status.h"
#include "sys/kern-flow.h"
#include "rtimer-arch.h"
#include "arm-def.h"
#include "dev/qwdt.h"
#include "lpm.h"

#include "py32f0xx_hal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if LPM_SHELL_ENABLED
#include "../../../../os/services/shell/quilla-shell.h"
#endif

/* Provided by user/example/Src/system_py32f040.c, which ships no header. */
extern void SystemCoreClockUpdate(void);

#if LPM_CONF_ENABLE != 0
/*---------------------------------------------------------------------------*/
/*
 * Stop admission thresholds, in rtimer ticks (LSI ticks).
 *
 * A sleep shorter than the minimum is not worth the cost of stopping and
 * restarting the clock tree; one longer than the maximum is refused rather
 * than reasoned about, which also keeps the milliseconds conversion below
 * inside 32 bits.
 */
#ifndef LPM_CONF_STOP_MIN_SLEEP_TICKS
#define LPM_CONF_STOP_MIN_SLEEP_TICKS   (RTIMER_SECOND / 50UL)  /* 20 ms   */
#endif
#ifndef LPM_CONF_STOP_MAX_SLEEP_TICKS
#define LPM_CONF_STOP_MAX_SLEEP_TICKS   (RTIMER_SECOND * 2UL)
#endif

/*
 * Time the wake-up sequence needs before the software is running again:
 * regulator and HSI start-up, the Flash wake-up delay and interrupt
 * latency. Subtracted from the watchdog window, so a larger value is the
 * conservative direction.
 */
#ifndef LPM_CONF_STOP_WAKEUP_MARGIN_MS
#define LPM_CONF_STOP_WAKEUP_MARGIN_MS  20U
#endif

/*
 * Distance in ticks to the earliest wake-up Stop can be guaranteed to
 * deliver. 0 means "derive it from the rtimer hardware", which today is
 * the LPTIM rollover; an image that gains a compare-match wake-up can
 * state the bound directly instead.
 */
#ifndef LPM_CONF_STOP_WAKEUP_TICKS
#define LPM_CONF_STOP_WAKEUP_TICKS      0UL
#endif

/* LPTIM1 runs with ARR = 0xFFFF, so one full scale is 65536 counts. */
#define LPM_LPTIM_FULL_SCALE            0x10000UL
/*---------------------------------------------------------------------------*/
#if LPM_CONF_STATS
rtimer_clock_t lpm_stats[3];

#define LPM_STATS_INIT() \
  do { memset(lpm_stats, 0, sizeof(lpm_stats)); } while(0)
#define LPM_STATS_ADD(pm, val)   do { lpm_stats[pm] += val; } while(0)
#else
#define LPM_STATS_INIT() do {} while (0)
#define LPM_STATS_ADD(stat, val) do {} while (0)
#endif
/*---------------------------------------------------------------------------*/
#if LPM_CONF_STATS
/* Remembers what time it was when we went to sleep, for the statistics. */
static rtimer_clock_t sleep_enter_time;
#endif

void clock_adjust(void);
/*---------------------------------------------------------------------------*/
/* Stores the currently specified MAX allowed PM */
static uint8_t max_pm;

/* PM the core is in, or was in when the interrupt that woke it arrived.
 * Only PM1 needs anything done on exit, so this is the whole state. */
static uint8_t current_pm;

static unsigned long sleep_entries;
static unsigned long stop_entries;
static unsigned long stop_refusals;
static const char *stop_reason;
/*---------------------------------------------------------------------------*/
/* Buffer to store peripheral PM1 permission FPs */
#ifdef LPM_CONF_PERIPH_PERMIT_PM1_FUNCS_MAX
#define LPM_PERIPH_PERMIT_PM1_FUNCS_MAX LPM_CONF_PERIPH_PERMIT_PM1_FUNCS_MAX
#else
#define LPM_PERIPH_PERMIT_PM1_FUNCS_MAX 5
#endif

static lpm_periph_permit_pm1_func_t
periph_permit_pm1_funcs[LPM_PERIPH_PERMIT_PM1_FUNCS_MAX];
/*---------------------------------------------------------------------------*/
static bool
periph_permit_pm1(void)
{
  int i;

  for(i = 0; i < LPM_PERIPH_PERMIT_PM1_FUNCS_MAX &&
      periph_permit_pm1_funcs[i] != NULL; i++) {
    if(!periph_permit_pm1_funcs[i]()) {
      return false;
    }
  }
  return true;
}
/*---------------------------------------------------------------------------*/
static void
refuse_stop(const char *reason)
{
  stop_refusals++;
  stop_reason = reason;
}
/*---------------------------------------------------------------------------*/
/*
 * Routine to put us in PM0. Nothing is stopped except the core clock, so
 * there is no state to save and no clock to restore: any interrupt in the
 * NVIC ends the sleep.
 */
static void
enter_pm0(void)
{
  ENERGEST_SWITCH(ENERGEST_TYPE_CPU, ENERGEST_TYPE_LPM);

#if LPM_CONF_STATS
  sleep_enter_time = RTIMER_NOW();
#endif

  HAL_PWR_EnterSLEEPMode(PWR_SLEEPENTRY_WFI);

#if LPM_CONF_STATS
  LPM_STATS_ADD(LPM_PM0, RTIMER_NOW() - sleep_enter_time);
#endif

  ENERGEST_SWITCH(ENERGEST_TYPE_LPM, ENERGEST_TYPE_CPU);

  sleep_entries++;
}
/*---------------------------------------------------------------------------*/
#if LPM_CONF_STOP_WAKEUP_VERIFIED
/*
 * Distance, in rtimer ticks, to the earliest moment the hardware can be
 * relied upon to leave Stop.
 *
 * The rtimer asks LPTIM1 for an autoreload-match interrupt every full
 * scale and does the comparison against the scheduled target in software,
 * so the interrupt arrives at the rollover and not at the target. That
 * rollover is the only wake-up source this port has, so it is also the
 * bound on how long a Stop can last.
 */
static rtimer_clock_t
stop_wakeup_distance(void)
{
#if LPM_CONF_STOP_WAKEUP_TICKS > 0UL
  return (rtimer_clock_t)LPM_CONF_STOP_WAKEUP_TICKS;
#else
  return (rtimer_clock_t)(LPM_LPTIM_FULL_SCALE -
                          (unsigned long)(rtimer_arch_now() &
                                          (LPM_LPTIM_FULL_SCALE - 1UL)));
#endif
}
/*---------------------------------------------------------------------------*/
static unsigned long
ticks_to_ms(rtimer_clock_t ticks)
{
  return (unsigned long)(((unsigned long)ticks * 1000UL) /
                         (unsigned long)RTIMER_SECOND);
}
/*---------------------------------------------------------------------------*/
/*
 * Decide whether the watchdog admits a Stop of this length.
 *
 * The IWDG runs off the LSI and is not stopped by Stop, so a sleep longer
 * than its timeout is a reset, not a saving. The counter is refreshed
 * first: without that the remaining window is unknown, and qwdt_timeout_min_ms()
 * only bounds a window that starts now. Refreshing does not hide a fault -
 * a Stop that is never left still ends in a reset.
 */
static bool
watchdog_admits(unsigned long sleep_ms)
{
  unsigned long window_ms;

  if(qwdt_is_started() == 0U) {
    /* No watchdog armed: nothing bounds the sleep but the thresholds. */
    return true;
  }

  (void)qwdt_refresh();

  window_ms = (unsigned long)qwdt_timeout_min_ms();
  if(window_ms <= (unsigned long)LPM_CONF_STOP_WAKEUP_MARGIN_MS) {
    return false;
  }
  window_ms -= (unsigned long)LPM_CONF_STOP_WAKEUP_MARGIN_MS;

  return sleep_ms < window_ms;
}
/*---------------------------------------------------------------------------*/
/*
 * Restore what Stop took away.
 *
 * Stop switches the HSI off and hands the system clock back to it on
 * wake-up, without the divider this image may have been running. Changing
 * the clock under a running UART would corrupt the link, so the port
 * simply refuses PM1 while anything clocked from the core is busy
 * (lpm_register_peripheral()); what is left to do here is to make the
 * software's idea of the clock frequency match reality.
 */
static void
restore_clock(void)
{
  if((RCC->CFGR & RCC_CFGR_SWS_Msk) != 0U) {
    /* Not the HSI. Something re-selected a source across the sleep, which
     * this port never does: report it rather than silently running at an
     * unexpected rate. */
    kern_raise(KERN_ERR_CLOCK, KERN_MOD_CLOCK, (unsigned int)__LINE__);
  }

  SystemCoreClockUpdate();
}
/*---------------------------------------------------------------------------*/
static void
enter_pm1(void)
{
  ENERGEST_SWITCH(ENERGEST_TYPE_CPU, ENERGEST_TYPE_DEEP_LPM);

#if LPM_CONF_STATS
  sleep_enter_time = RTIMER_NOW();
#endif

  /*
   * The monitored sequences cannot run while the core is stopped, and
   * clock_adjust() on exit moves clock_time() forward by the whole sleep.
   * Restarting the cycles here keeps that jump from being scored as a
   * budget violation, which it is not: it is the sleep, not a stall.
   */
  kern_flow_reset_all();

  current_pm = LPM_PM1;

  HAL_PWR_EnterSTOPMode(PWR_MAINREGULATOR_ON, PWR_STOPENTRY_WFI);

  /* lpm_exit() has run inside the interrupt that ended the Stop. */
  current_pm = LPM_PM0;

  ENERGEST_SWITCH(ENERGEST_TYPE_DEEP_LPM, ENERGEST_TYPE_CPU);

  stop_entries++;
}
#endif /* LPM_CONF_STOP_WAKEUP_VERIFIED */
/*---------------------------------------------------------------------------*/
void
lpm_exit(void)
{
  /* Sleep, or never asleep at all: no clock was stopped, so there is
   * nothing to restore and no software clock to catch up. */
  if(current_pm == LPM_PM0) {
    return;
  }

#if LPM_CONF_STOP_WAKEUP_VERIFIED
  restore_clock();

  /* The SysTick was stopped for the whole sleep; bring the software clock
   * back in line with the rtimer, which never stopped. */
  clock_adjust();

#if LPM_CONF_STATS
  LPM_STATS_ADD(LPM_PM1, RTIMER_NOW() - sleep_enter_time);
#endif
#endif

  current_pm = LPM_PM0;
}
/*---------------------------------------------------------------------------*/
void
lpm_enter(void)
{
  if(max_pm == LPM_PM0) {
    enter_pm0();
    return;
  }

  if(!periph_permit_pm1()) {
    /* Reported rather than silent: "a peripheral said no" is the
     * answer an engineer needs from the lpm command. */
    refuse_stop("peripheral refuses PM1");
    enter_pm0();
    return;
  }

  if(process_nevents() != 0) {
    enter_pm0();
    return;
  }

#if LPM_CONF_STOP_WAKEUP_VERIFIED
  {
    rtimer_clock_t now;
    rtimer_clock_t target;
    rtimer_clock_t until_target;
    rtimer_clock_t wakeup;
    unsigned long sleep_ms;

    target = rtimer_arch_next_trigger();
    now = RTIMER_NOW();

    if(target == 0U) {
      /* Nothing scheduled: only an unsolicited interrupt could end the
       * sleep, which is exactly what PM0 gives us. */
      refuse_stop("no rtimer scheduled");
      enter_pm0();
      return;
    }

    /* Distance to the scheduled target. The rtimer is monotonic and the
     * trigger is always ahead of now, so the subtraction cannot wrap into
     * a huge value unless the target has already passed. */
    until_target = (rtimer_clock_t)(target - now);
    if((until_target > (rtimer_clock_t)LPM_CONF_STOP_MAX_SLEEP_TICKS) ||
       (until_target < (rtimer_clock_t)LPM_CONF_STOP_MIN_SLEEP_TICKS)) {
      refuse_stop("target outside stop range");
      enter_pm0();
      return;
    }

    wakeup = stop_wakeup_distance();
    if(wakeup > until_target) {
      /* The hardware could only wake us after the moment we are supposed
       * to act. Sleeping would miss the deadline; staying in PM0 does not
       * stop anything but the core clock. */
      refuse_stop("wake-up later than target");
      enter_pm0();
      return;
    }

    sleep_ms = ticks_to_ms(wakeup);
    if(!watchdog_admits(sleep_ms)) {
      refuse_stop("sleep exceeds watchdog window");
      enter_pm0();
      return;
    }

    enter_pm1();
    return;
  }
#else
  refuse_stop("stop wake-up not verified on this board");
  enter_pm0();
#endif
}
/*---------------------------------------------------------------------------*/
void
lpm_set_max_pm(uint8_t pm)
{
  max_pm = (pm > LPM_CONF_MAX_PM) ? (uint8_t)LPM_CONF_MAX_PM : pm;
}
/*---------------------------------------------------------------------------*/
void
lpm_register_peripheral(lpm_periph_permit_pm1_func_t permit_pm1_func)
{
  int i;

  for(i = 0; i < LPM_PERIPH_PERMIT_PM1_FUNCS_MAX; i++) {
    if(periph_permit_pm1_funcs[i] == permit_pm1_func) {
      break;
    } else if(periph_permit_pm1_funcs[i] == NULL) {
      periph_permit_pm1_funcs[i] = permit_pm1_func;
      break;
    }
  }
}
/*---------------------------------------------------------------------------*/
unsigned int
lpm_max_pm(void)
{
  return (unsigned int)max_pm;
}
/*---------------------------------------------------------------------------*/
unsigned long
lpm_stop_refusals(void)
{
  return stop_refusals;
}
/*---------------------------------------------------------------------------*/
const char *
lpm_stop_refusal_reason(void)
{
  return (stop_reason != NULL) ? stop_reason : "none";
}
/*---------------------------------------------------------------------------*/
void
lpm_init(void)
{
  PWR_StopModeConfigTypeDef stop_cfg;

  __HAL_RCC_PWR_CLK_ENABLE();

  /*
   * Stop settings chosen for the shortest, most predictable wake-up rather
   * than for the last microamp: main regulator stays on during Stop
   * (Stop 0), the HSI is re-enabled immediately instead of waiting for the
   * regulator, and the Flash is given the standard wake-up delay. A slow
   * wake-up costs the watchdog window, which is the scarcer resource.
   */
  memset(&stop_cfg, 0, sizeof(stop_cfg));
  stop_cfg.LPVoltSelection = PWR_STOPMOD_LPR_VOLT_SCALE1;
  stop_cfg.WakeUpHsiEnableTime = PWR_WAKEUP_HSIEN_IMMEDIATE;
  stop_cfg.FlashDelay = PWR_WAKEUP_FLASH_DELAY_1US;
  (void)HAL_PWR_ConfigStopMode(&stop_cfg);

  /* Sleep, not Stop, until something asks for more. */
  CLEAR_BIT(SCB->SCR, ((uint32_t)SCB_SCR_SLEEPDEEP_Msk));

  max_pm = (uint8_t)LPM_CONF_MAX_PM;
  current_pm = LPM_PM0;

  LPM_STATS_INIT();
}
/*---------------------------------------------------------------------------*/
#endif /* LPM_CONF_ENABLE != 0 */
/** @} */

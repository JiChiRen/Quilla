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
 * \addtogroup arm
 * @{
 *
 * \file
 * Software clock for the GD32F502VE port.
 *
 * The tick source is the Cortex-M33 SysTick, configured at CLOCK_SECOND.
 * The 64-bit tick counter is *derived* from the rtimer rather than counted by
 * the SysTick handler, exactly as on the PY32F040xB port: SysTick interrupts can
 * be missed (interrupt masking, low-power) and the rtimer keeps running
 * regardless, so it is the more trustworthy of the two.
 */

#include "contiki.h"
#include "sys/kern-status.h"

#include "sys/clock.h"
#include "sys/energest.h"
#include "sys/etimer.h"
#include "sys/rtimer.h"

#include "gd32f50x.h"
#include "gd32f50x_fmc.h"

#include <stdint.h>
/*---------------------------------------------------------------------------*/
static volatile uint64_t rt_ticks_startup = 0, rt_ticks_epoch = 0;
/*---------------------------------------------------------------------------*/
kern_status_t
clock_init(void)
{
  /* Flash wait-state configuration for 160 MHz AHB.
   *
   * The GD32F50x FMC is the F4xx-compatible controller: the *code* fetch path
   * is kept alive at 160 MHz by the "no waiting time area" feature
   * (FMC_CTL0_NWLDE, set in system_gd32f50x.c), but the *data/erase* path is
   * governed by WSCNT in FMC_WS (offset 0x00) and is left at its reset value
   * (0) otherwise. Per the GD32F4xx/E50 reference, <=180 MHz AHB requires
   * WSCNT = 4. The first flash erase on real silicon fails (NAK 0x04 ERASE)
   * until this is set. Writing it here (AHB already 160 MHz) is safe: it only
   * adds wait states to the data/erase path and cannot disturb the NWA code
   * fetch. The strictly-correct placement is before the AHB clock is raised;
   * doing it here is equivalent because no flash data access happens between
   * SystemInit and the first QIAP erase. */
  FMC_WS = 4U;

  /* SystemCoreClock is maintained by system_gd32f50x.c: 160 MHz from the
   * 8 MHz HXTAL through PLL0 (see __SYSTEM_CLOCK_160M_PLL_HXTAL). */
  if(SystemCoreClock < CLOCK_SECOND) {
    kern_raise(KERN_ERR_INTERNAL, KERN_MOD_CLOCK, __LINE__);
    return KERN_ERR_INTERNAL;
  }
  /* SysTick_Config() returns 0 on success. It also installs the lowest
   * possible priority for the SysTick exception, which is what we want: the
   * rtimer ISR (the real time base) must be able to preempt it. */
  if(SysTick_Config(SystemCoreClock / CLOCK_SECOND) != 0U) {
    kern_raise(KERN_ERR_INTERNAL, KERN_MOD_CLOCK, __LINE__);
    return KERN_ERR_INTERNAL;
  }
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
clock_time_t
clock_time(void)
{
  return rt_ticks_startup / (RTIMER_SECOND / CLOCK_SECOND);
}
/*---------------------------------------------------------------------------*/
void
clock_set_seconds(unsigned long sec)
{
  rt_ticks_epoch = (uint64_t)sec * RTIMER_SECOND;
}
/*---------------------------------------------------------------------------*/
unsigned long
clock_seconds(void)
{
  return rt_ticks_epoch / RTIMER_SECOND;
}
/*---------------------------------------------------------------------------*/
void
clock_wait(clock_time_t i)
{
  clock_time_t start;

  start = clock_time();
  while(clock_time() - start < (clock_time_t)i) {
    /* spin */
  }
}
/*---------------------------------------------------------------------------*/
/**
 * \brief Obsolete delay function but we implement it here since some code
 * still uses it
 *
 * Busy-waits roughly i milliseconds using the rtimer (1 us per tick). The
 * QBOOT image has no rtimer, and nothing in that image calls this.
 */
void
clock_delay(unsigned int i)
{
#ifndef QBOOT_BUILD
  rtimer_clock_t start = RTIMER_NOW();
  while((rtimer_clock_t)(RTIMER_NOW() - start) <
        ((rtimer_clock_t)i * (RTIMER_SECOND / 1000U))) {
    /* spin */
  }
#else
  (void)i;
#endif
}
/*---------------------------------------------------------------------------*/
/**
 * \brief Update the software clock ticks and seconds
 *
 * Extends the rtimer counter to 64 bits by detecting wraparounds, and polls
 * the etimer process when an etimer may have expired.
 */
#ifndef QBOOT_BUILD
static void
clock_update_ticks(void)
{
  rtimer_clock_t now;
  uint64_t prev_rt_ticks_startup, cur_rt_ticks_startup;
  uint32_t cur_rt_ticks_startup_hi;

  now = RTIMER_NOW();
  prev_rt_ticks_startup = rt_ticks_startup;

  cur_rt_ticks_startup_hi = prev_rt_ticks_startup >> 32;
  if(now < (rtimer_clock_t)prev_rt_ticks_startup) {
    cur_rt_ticks_startup_hi++;
  }
  cur_rt_ticks_startup = (uint64_t)cur_rt_ticks_startup_hi << 32 | now;
  rt_ticks_startup = cur_rt_ticks_startup;

  rt_ticks_epoch += cur_rt_ticks_startup - prev_rt_ticks_startup;
  if(etimer_pending()) {
    etimer_request_poll();
  }
}
#endif /* !QBOOT_BUILD */
/*---------------------------------------------------------------------------*/
/**
 * \brief Adjust the clock following missed SysTick ISRs
 *
 * Only meant to be used by lpm_exit().
 */
void
clock_adjust(void)
{
#ifndef QBOOT_BUILD
  SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk;
  clock_update_ticks();
  SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;
#endif /* !QBOOT_BUILD */
}
/*---------------------------------------------------------------------------*/
/**
 * \brief The clock Interrupt Service Routine
 */
void
SysTick_Handler(void)
{
#ifndef QBOOT_BUILD
  clock_update_ticks();
#endif
}
/*---------------------------------------------------------------------------*/

/**
 * @}
 * @}
 */

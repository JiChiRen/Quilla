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
 * rtimer for the GD32F502VE: TIMER0 (APB2, 160 MHz) prescaled to 1 MHz.
 *
 * TIMER0 is a 16-bit timer, so the hardware counter alone would wrap every
 * 65.5 ms. The port keeps a 16-bit overflow count in the update ISR and
 * composes the two into the 32-bit rtimer_clock_t the generic layer expects,
 * the same trick the PY32F040xB port uses with its LPTIM.
 *
 * Scheduling consequence (worth knowing before relying on tight rtimer
 * deadlines): the target is compared against the full 32-bit counter inside
 * the *overflow* handler, so a scheduled rtimer fires on the first overflow
 * at or after its target. Worst-case lateness is one counter period, 65.5 ms.
 * A compare-match channel would remove that, but nothing in this port needs
 * sub-period accuracy yet and the software path is the one already proven in
 * this tree.
 */

#include "contiki.h"
#include "sys/rtimer.h"
#include "rtimer-arch.h"

#include "gd32f50x.h"

#include <stdint.h>
#include <stdbool.h>
/*---------------------------------------------------------------------------*/
#ifndef RTIMER_CLOCK_LT
#define RTIMER_CLOCK_LT(a,b) ((rtimer_clock_t)((a)-(b)) > 0x7FFFFFFFU)
#endif
#ifndef RTIMER_CLOCK_GE
#define RTIMER_CLOCK_GE(a,b) (!RTIMER_CLOCK_LT(a,b))
#endif

/* TIMER0 is an advanced timer whose counter only runs while the counter
 * enable bit is set; there is no master-output gate to open because no
 * channel drives a pin. */
#define RT_TIMER          TIMER0
#define RT_TIMER_RCU      RCU_TIMER0
#define RT_TIMER_PERIOD   0xFFFFU
/*---------------------------------------------------------------------------*/
static volatile uint16_t rtimer_overflow_cnt;
static volatile rtimer_clock_t target_tick;
static volatile bool rtimer_pending;
/*---------------------------------------------------------------------------*/
/**
 * \brief Compose the 16-bit hardware counter and the software overflow count.
 *
 * The overflow count is read either side of the counter read and the pair is
 * only accepted when both reads agree, so a wrap that lands inside the window
 * cannot produce a value that jumps backwards.
 */
static rtimer_clock_t
rtimer_read_full_counter(void)
{
  uint16_t ov1, ov2;
  uint32_t cnt;

  do {
    ov1 = rtimer_overflow_cnt;
    cnt = timer_counter_read(RT_TIMER);
    ov2 = rtimer_overflow_cnt;
  } while(ov1 != ov2);

  return ((rtimer_clock_t)ov1 << 16) | (cnt & 0x0000FFFFU);
}
/*---------------------------------------------------------------------------*/
void
rtimer_arch_init(void)
{
  timer_parameter_struct tps;

  rcu_periph_clock_enable(RT_TIMER_RCU);

  timer_struct_para_init(&tps);
  tps.prescaler         = (uint16_t)(GD32_RTIMER_PRESCALER - 1U);
  tps.alignedmode       = TIMER_COUNTER_EDGE;
  tps.counterdirection  = TIMER_COUNTER_UP;
  tps.clockdivision     = TIMER_CKDIV_DIV1;
  tps.period            = RT_TIMER_PERIOD;
  tps.repetitioncounter = 0U;
  timer_init(RT_TIMER, &tps);

  rtimer_overflow_cnt = 0U;
  target_tick = 0U;
  rtimer_pending = false;

  timer_interrupt_flag_clear(RT_TIMER, TIMER_INT_FLAG_UP);
  timer_interrupt_enable(RT_TIMER, TIMER_INT_UP);
  /* Higher priority than SysTick: this counter is the time base the software
   * clock is derived from. */
  NVIC_SetPriority(TIMER0_UP_IRQn, 1U);
  NVIC_ClearPendingIRQ(TIMER0_UP_IRQn);
  NVIC_EnableIRQ(TIMER0_UP_IRQn);

  timer_enable(RT_TIMER);
}
/*---------------------------------------------------------------------------*/
void
rtimer_arch_schedule(rtimer_clock_t t)
{
  rtimer_clock_t now = rtimer_arch_now();

  if(RTIMER_CLOCK_LT(t, now + RTIMER_GUARD_TIME)) {
    t = now + RTIMER_GUARD_TIME;
  }
  target_tick = t;
  rtimer_pending = true;
}
/*---------------------------------------------------------------------------*/
rtimer_clock_t
rtimer_arch_now(void)
{
  return rtimer_read_full_counter();
}
/*---------------------------------------------------------------------------*/
rtimer_clock_t
rtimer_arch_next_trigger(void)
{
  return target_tick;
}
/*---------------------------------------------------------------------------*/
void
TIMER0_UP_IRQHandler(void)
{
  if(timer_interrupt_flag_get(RT_TIMER, TIMER_INT_FLAG_UP) != RESET) {
    timer_interrupt_flag_clear(RT_TIMER, TIMER_INT_FLAG_UP);
    rtimer_overflow_cnt++;

    if(rtimer_pending) {
      if(RTIMER_CLOCK_GE(rtimer_read_full_counter(), target_tick)) {
        rtimer_pending = false;
        target_tick = 0U;
        rtimer_run_next();
      }
    }
  }
}
/*---------------------------------------------------------------------------*/
/** @} */

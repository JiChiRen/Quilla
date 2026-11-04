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
 * \addtogroup riscv
 * @{
 *
 * \file
 * rtimer for the CH32V305VB: TIM1 (APB2, 96 MHz) prescaled to 1 MHz.
 *
 * TIM1 is a 16-bit timer, so the hardware counter alone would wrap every
 * 65.5 ms. The port keeps a 16-bit overflow count in the update ISR and
 * composes the two into the 32-bit rtimer_clock_t the generic layer expects,
 * the same trick the GD32F502VE port uses with TIMER0.
 */

#include "contiki.h"
#include "sys/rtimer.h"
#include "rtimer-arch.h"

#include "ch32v30x.h"

#include <stdint.h>
#include <stdbool.h>
/*---------------------------------------------------------------------------*/
#ifndef RTIMER_CLOCK_LT
#define RTIMER_CLOCK_LT(a,b) ((rtimer_clock_t)((a)-(b)) > 0x7FFFFFFFU)
#endif
#ifndef RTIMER_CLOCK_GE
#define RTIMER_CLOCK_GE(a,b) (!RTIMER_CLOCK_LT(a,b))
#endif

#define RT_TIMER          TIM1
#define RT_TIMER_RCU      RCC_APB2Periph_TIM1
#define RT_TIMER_IRQn     TIM1_UP_IRQn
#define RT_TIMER_PERIOD   0xFFFFU
/*---------------------------------------------------------------------------*/
static volatile uint16_t rtimer_overflow_cnt;
static volatile rtimer_clock_t target_tick;
static volatile bool rtimer_pending;
/*---------------------------------------------------------------------------*/
static rtimer_clock_t
rtimer_read_full_counter(void)
{
  uint16_t ov1, ov2;
  uint32_t cnt;

  do {
    ov1 = rtimer_overflow_cnt;
    cnt = TIM_GetCounter(RT_TIMER);
    ov2 = rtimer_overflow_cnt;
  } while(ov1 != ov2);

  return ((rtimer_clock_t)ov1 << 16) | (cnt & 0x0000FFFFU);
}
/*---------------------------------------------------------------------------*/
void
rtimer_arch_init(void)
{
  TIM_TimeBaseInitTypeDef tps;

  RCC_APB2PeriphClockCmd(RT_TIMER_RCU, ENABLE);

  TIM_TimeBaseStructInit(&tps);
  tps.TIM_Prescaler         = (uint16_t)(CH32V305_RTIMER_PRESCALER - 1U);
  tps.TIM_CounterMode       = TIM_CounterMode_Up;
  tps.TIM_Period            = RT_TIMER_PERIOD;
  tps.TIM_ClockDivision     = TIM_CKD_DIV1;
  tps.TIM_RepetitionCounter = 0U;
  TIM_TimeBaseInit(RT_TIMER, &tps);

  rtimer_overflow_cnt = 0U;
  target_tick = 0U;
  rtimer_pending = false;

  TIM_ClearITPendingBit(RT_TIMER, TIM_IT_Update);
  TIM_ITConfig(RT_TIMER, TIM_IT_Update, ENABLE);
  /* Higher priority than SysTick: this counter is the time base the software
   * clock is derived from. */
  NVIC_SetPriority(RT_TIMER_IRQn, 1U);
  NVIC_ClearPendingIRQ(RT_TIMER_IRQn);
  NVIC_EnableIRQ(RT_TIMER_IRQn);

  TIM_Cmd(RT_TIMER, ENABLE);
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
TIM1_UP_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void
TIM1_UP_IRQHandler(void)
{
  if(TIM_GetITStatus(RT_TIMER, TIM_IT_Update) != RESET) {
    TIM_ClearITPendingBit(RT_TIMER, TIM_IT_Update);
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

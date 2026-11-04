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
 * Software clock for the CH32V305VB (RISC-V) port.
 *
 * The tick source is the WCH RV SysTick (0xE000F000), configured at
 * CLOCK_SECOND. The 64-bit tick counter is *derived* from the rtimer rather
 * than counted by the SysTick handler, exactly as on the GD32F502VE port:
 * SysTick interrupts can be missed and the rtimer keeps running regardless,
 * so it is the more trustworthy of the two. SysTick only triggers the poll.
 *
 * SysTick register map (core_riscv.h): CTLR bit0 = STE (enable), bit4 = STIE
 * (interrupt), bit5 = INIT (reload CNT to 0); SR bit0 = CNTIF (compare match);
 * CMP = compare value; the counter counts up and auto-reloads from 0 on match.
 * The handler clears CNTIF and lets the hardware reload.
 */
#include "contiki.h"
#include "sys/kern-status.h"

#include "sys/clock.h"
#include "sys/energest.h"
#include "sys/etimer.h"
#include "sys/rtimer.h"

#include "ch32v30x.h"

#include <stdint.h>
/*---------------------------------------------------------------------------*/
/* SysTick CTLR bits (WCH RV SysTick). */
#define SYSTICK_CTLR_STE   0x00000001UL   /* counter enable            */
#define SYSTICK_CTLR_STIE  0x00000010UL   /* interrupt enable          */
#define SYSTICK_CTLR_INIT  0x00000020UL   /* reload CNT to 0           */
#define SYSTICK_SR_CNTIF   0x00000001UL   /* compare-match flag        */
/*---------------------------------------------------------------------------*/
static volatile uint64_t rt_ticks_startup = 0, rt_ticks_epoch = 0;
/*---------------------------------------------------------------------------*/
kern_status_t
clock_init(void)
{
  /* SystemCoreClock is maintained by system_ch32v30x.c: 96 MHz from the 8 MHz
   * HSE through the PLL (SYSCLK_FREQ_96MHz_HSE). */
  if(SystemCoreClock < CLOCK_SECOND) {
    kern_raise(KERN_ERR_INTERNAL, KERN_MOD_CLOCK, __LINE__);
    return KERN_ERR_INTERNAL;
  }

  /* Periodic SysTick at CLOCK_SECOND. Reload the counter (INIT), arm the
   * interrupt (STIE), then enable (STE). CNTIF is cleared by the handler. */
  SysTick->CMP  = SystemCoreClock / CLOCK_SECOND;
  SysTick->SR  &= ~SYSTICK_SR_CNTIF;
  SysTick->CTLR = SYSTICK_CTLR_INIT | SYSTICK_CTLR_STIE | SYSTICK_CTLR_STE;

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
void
clock_adjust(void)
{
#ifndef QBOOT_BUILD
  SysTick->CTLR &= ~SYSTICK_CTLR_STIE;
  clock_update_ticks();
  SysTick->CTLR |= SYSTICK_CTLR_STIE;
#endif /* !QBOOT_BUILD */
}
/*---------------------------------------------------------------------------*/
/**
 * \brief The clock Interrupt Service Routine
 */
void
SysTick_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void
SysTick_Handler(void)
{
#ifndef QBOOT_BUILD
  SysTick->SR &= ~SYSTICK_SR_CNTIF;   /* clear compare-match flag */
  clock_update_ticks();
#endif
}
/*---------------------------------------------------------------------------*/
/**
 * @}
 */

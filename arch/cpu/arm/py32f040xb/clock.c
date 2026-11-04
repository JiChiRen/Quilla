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

#include "contiki.h"
#include "sys/kern-status.h"
#include "arm-def.h"
#include "cpu.h"

#include "py32f0xx_hal.h"

#include "sys/clock.h"
#include "sys/energest.h"
#include "sys/etimer.h"
#include "sys/rtimer.h"

#include <stdint.h>
/*---------------------------------------------------------------------------*/

/* Wedge probe: last-resort detector for "the watchdog bit but everything
 * looked healthy". The main loop touches the probe at every checkpoint and
 * at every watchdog feed; this ISR-side monitor fires when the touch chain
 * has been silent for ~375 ms (12 ticks @ 32 Hz) - still inside the shortest
 * IWDG window (~454 ms), so the probe wins the race and reports WHERE the
 * loop died before resetting. Compiled only for the APP image and only when
 * QUILLA_WEDGE_PROBE is set; inert otherwise. */
#ifndef QUILLA_WEDGE_PROBE
#define QUILLA_WEDGE_PROBE 0
#endif

#if !defined(QBOOT_BUILD) && QUILLA_WEDGE_PROBE
#include "shellport.h"

#define WEDGE_PROBE_TICKS (clock_time_t)12U  /* 375 ms @ CLOCK_SECOND=32 */

volatile clock_time_t wedge_probe_feed = 0;
volatile uint8_t wedge_probe_last = 0xFFU;   /* 0xFF = "not armed yet" */
static volatile uint8_t wedge_probe_armed = 0U;

void wedge_probe_arm(void)
{
  wedge_probe_feed = clock_time();
  wedge_probe_armed = 1U;
}

void wedge_probe_touch(unsigned int cp)
{
  wedge_probe_last = (uint8_t)cp;
  wedge_probe_feed = clock_time();
}
#endif /* !QBOOT_BUILD && QUILLA_WEDGE_PROBE */
/*---------------------------------------------------------------------------*/

//extern uint32_t uwTickFreq;

static volatile uint64_t rt_ticks_startup = 0, rt_ticks_epoch = 0;

/* PY32F040xB: switch the core from the power-on 8 MHz HSI to 48 MHz
 * (HSI 24 MHz + PLL x2). FLASH needs one wait state above 24 MHz.
 *
 * This MUST run after HAL_Init() so that uwTickFreq is defined:
 * HAL_RCC_ClockConfig() recomputes the SysTick reload from
 * SystemCoreClock / uwTickFreq, and with uwTickFreq still 0 (HAL not
 * initialised) that reload comes out wrong and the PLL path silently
 * fails to take effect while SystemCoreClock is left claiming 48 MHz -
 * which is exactly the previous bring-up bug (garbled UART). See
 * doc/port-layout / change record for the root cause. */
static void
quilla_clock_set_48mhz(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /* Oscillator: HSI 24 MHz (internal RC, factory-trimmed from OTP 0x1FFF3220),
   * PLL ON, x2 -> 48 MHz. HSE/LSI/LSE are not fitted on this board: leave OFF. */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI |
                                    RCC_OSCILLATORTYPE_LSI | RCC_OSCILLATORTYPE_LSE;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_24MHz;
  RCC_OscInitStruct.HSEState = RCC_HSE_OFF;
  RCC_OscInitStruct.LSIState = RCC_LSI_OFF;
  RCC_OscInitStruct.LSEState = RCC_LSE_OFF;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
    kern_raise(KERN_ERR_INTERNAL, KERN_MOD_CLOCK, __LINE__);
    return;
  }

  /* Clock tree: PLL output as SYSCLK, no AHB/APB dividers -> 48 MHz everywhere. */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK) {
    kern_raise(KERN_ERR_INTERNAL, KERN_MOD_CLOCK, __LINE__);
    return;
  }
}

kern_status_t
clock_init(void)
{
  /* HAL_Init() sets uwTickFreq = 1000 and arms the default 8 MHz SysTick.
   * It MUST precede the 48 MHz switch (see quilla_clock_set_48mhz). */
  HAL_Init();

  quilla_clock_set_48mhz();

  /* HAL_RCC_ClockConfig() already re-arms SysTick at 48 MHz / 1000 Hz; repeat
   * the explicit init as a belt-and-suspenders re-arm and keep the error path. */
  if (HAL_InitTick(TICK_INT_PRIORITY) != HAL_OK) {
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
  while(clock_time() - start < (clock_time_t)i);
}

/*---------------------------------------------------------------------------*/
/**
 * \brief Obsolete delay function but we implement it here since some code
 * still uses it
 */
void
clock_delay(unsigned int i)
{
  HAL_Delay((uint32_t)i);
}
/*---------------------------------------------------------------------------*/
/**
 * \brief Update the software clock ticks and seconds
 *
 * This function is used to update the software tick counters whenever the
 * system clock might have changed, which can occur upon a SysTick ISR or upon
 * wake-up from PM1/2.
 *
 * For the software clock ticks counter, the Sleep Timer counter value is used
 * as the base tick value, and extended to a 64-bit value thanks to a detection
 * of wraparounds.
 *
 * For the seconds counter, the changes of the Sleep Timer counter value are
 * added to the reference time, which is either the startup time or the value
 * passed to clock_set_seconds().
 *
 * This function polls the etimer process if an etimer has expired.
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
  /*
   * Inform the etimer library that the system clock has changed and that an
   * etimer might have expired.
   */
  if(etimer_pending()) {
    etimer_request_poll();
  }
}
#endif /* !QBOOT_BUILD */
/*---------------------------------------------------------------------------*/
/**
 * \brief Adjust the clock following missed SysTick ISRs
 *
 * This function is useful when coming out of PM1/2, during which the system
 * clock is stopped. We adjust the clock counters like after any SysTick ISR.
 *
 * \note This function is only meant to be used by lpm_exit(). Applications
 * should really avoid calling this
 */
void
clock_adjust(void)
{
#ifndef QBOOT_BUILD
  /* Halt the SysTick while adjusting */
  SysTick->CTRL &= ~SysTick_CTRL_TICKINT_Msk;//HAL_SuspendTick();//

  clock_update_ticks();

  /* Re-Start the SysTick */
  SysTick->CTRL |= SysTick_CTRL_TICKINT_Msk;//HAL_ResumeTick();//
#endif /* !QBOOT_BUILD */
}
/*---------------------------------------------------------------------------*/
/**
 * \brief The clock Interrupt Service Routine
 *
 * It polls the etimer process if an etimer has expired. It also updates the
 * software clock tick and seconds counter.
 */
void
SysTick_Handler(void)
{
  HAL_IncTick();
#ifndef QBOOT_BUILD
  clock_update_ticks();
#endif
#if !defined(QBOOT_BUILD) && QUILLA_WEDGE_PROBE
  if(wedge_probe_armed != 0U) {
    clock_time_t now = clock_time();
    if((clock_time_t)(now - wedge_probe_feed) > WEDGE_PROBE_TICKS) {
      char cs[2];
      /* Dying gasp, bypassing the shell: which checkpoint was last touched. */
      shellport_emergency_write("\r\nwedge: cp=");
      cs[0] = (wedge_probe_last < 10U) ? (char)('0' + wedge_probe_last) : '?';
      cs[1] = '\0';
      shellport_emergency_write(cs);
      shellport_emergency_write("\r\n");
      /* Controlled reset: lands as "reset: SFT", distinguishable from the
       * IWDG bite that would otherwise have followed. */
      NVIC_SystemReset();
    }
  }
#endif
}
/*---------------------------------------------------------------------------*/

/**
 * @}
 * @}
 */

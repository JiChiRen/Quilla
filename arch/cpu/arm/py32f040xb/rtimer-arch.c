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
#include "contiki.h"
#include "cpu.h"
#include "arm-def.h"
#include "sys/rtimer.h"
#include "lpm.h"

#include <stdint.h>
#include <stdbool.h>
#define Error_Handler() 

#ifndef RTIMER_CLOCK_LT
#define RTIMER_CLOCK_LT(a,b) ((rtimer_clock_t)((a)-(b)) > 0x7FFFFFFFU)
#endif
#ifndef RTIMER_CLOCK_GE
#define RTIMER_CLOCK_GE(a,b) (!RTIMER_CLOCK_LT(a,b))
#endif

static LPTIM_HandleTypeDef hlptim1;
static volatile uint32_t lptim_overflow_cnt;
static volatile rtimer_clock_t target_tick;
static volatile bool rtimer_pending;

static rtimer_clock_t lptim_read_full_counter(void)
{
    uint32_t ov1, ov2;
    uint16_t cnt;
    do {
        ov1 = lptim_overflow_cnt;
        cnt = (uint16_t)HAL_LPTIM_ReadCounter(&hlptim1);
        ov2 = lptim_overflow_cnt;
    } while (ov1 != ov2);

    return ((rtimer_clock_t)ov1 << 16) | cnt;
}

void HAL_LPTIM_AutoReloadMatchCallback(LPTIM_HandleTypeDef *hlptim)
{
    lptim_overflow_cnt++;
    bool trigger_task = false;

    if(rtimer_pending)
    {
        rtimer_clock_t now = lptim_read_full_counter();
        if(RTIMER_CLOCK_GE(now, target_tick))
        {
            rtimer_pending = false;
            target_tick = 0;
            trigger_task = true;
        }
    }

    if(trigger_task)
    {
        rtimer_run_next();
        lpm_exit();
    }
}

void rtimer_arch_init(void)
{
  RCC_OscInitTypeDef osc_cfg = {0};
  RCC_PeriphCLKInitTypeDef periph_cfg = {0};

  osc_cfg.OscillatorType = RCC_OSCILLATORTYPE_LSI;
  osc_cfg.LSIState = RCC_LSI_ON;
  if(HAL_RCC_OscConfig(&osc_cfg) != HAL_OK)
    Error_Handler();

  periph_cfg.PeriphClockSelection = RCC_PERIPHCLK_LPTIM;
  periph_cfg.LptimClockSelection = RCC_LPTIMCLKSOURCE_LSI;
  if(HAL_RCCEx_PeriphCLKConfig(&periph_cfg) != HAL_OK)
    Error_Handler();

  __HAL_RCC_LPTIM_CLK_ENABLE();
  hlptim1.Instance = LPTIM1;
  hlptim1.Init.Prescaler = LPTIM_PRESCALER_DIV1;
  hlptim1.Init.UpdateMode = LPTIM_UPDATE_IMMEDIATE;
  if(HAL_LPTIM_Init(&hlptim1) != HAL_OK)
    Error_Handler();

  lptim_overflow_cnt = 0;
  target_tick = 0;
  rtimer_pending = false;

  HAL_LPTIM_SetContinue_Start_IT(&hlptim1, 0xFFFFU);
}

void rtimer_arch_schedule(rtimer_clock_t t)
{
  rtimer_clock_t now = rtimer_arch_now();

  if(RTIMER_CLOCK_LT(t, now + RTIMER_GUARD_TIME))
  {
    t = now + RTIMER_GUARD_TIME;
  }
  target_tick = t;
  rtimer_pending = true;

  if(HAL_LPTIM_GetState(&hlptim1) == HAL_LPTIM_STATE_READY)
  {
      HAL_LPTIM_SetContinue_Start_IT(&hlptim1, 0xFFFFU);
  }
}

rtimer_clock_t rtimer_arch_now(void)
{
  return lptim_read_full_counter();
}

rtimer_clock_t rtimer_arch_next_trigger(void)
{
  return target_tick;
}

void TIM6_LPTIM1_IRQHandler(void)
{
  HAL_LPTIM_IRQHandler(&hlptim1);
  lpm_exit();
}

void HAL_LPTIM_MspInit(LPTIM_HandleTypeDef *hlptim)
{
  if(hlptim->Instance == LPTIM1)
  {
    __HAL_RCC_LPTIM_CLK_ENABLE();
    HAL_NVIC_SetPriority(TIM6_LPTIM1_IRQn, TICK_INT_PRIORITY - 1U, 0U);
    HAL_NVIC_EnableIRQ(TIM6_LPTIM1_IRQn);
  }
}

void HAL_LPTIM_MspDeInit(LPTIM_HandleTypeDef *hlptim)
{
  if(hlptim->Instance == LPTIM1)
  {
    HAL_LPTIM_SetContinue_Stop_IT(hlptim);
    __HAL_RCC_LPTIM_CLK_DISABLE();
    HAL_NVIC_DisableIRQ(TIM6_LPTIM1_IRQn);
  }
}
/*---------------------------------------------------------------------------*/
/** @} */

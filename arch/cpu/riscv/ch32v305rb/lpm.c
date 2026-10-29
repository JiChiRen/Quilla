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

#include "contiki.h"
#include "lpm.h"

/* For __WFI(), which core_riscv.h declares for the WCH PFIC. */
#include "ch32v30x.h"

#if LPM_CONF_MAX_PM > 0
#error "CH32V305VB port implements PM0 (Sleep) only; see lpm.h for why."
#endif

#if LPM_SHELL_ENABLED
#include "../../../../os/services/shell/quilla-shell.h"
#endif

#include <stdint.h>
/*---------------------------------------------------------------------------*/
uint32_t lpm_stats_sleeps;
/*---------------------------------------------------------------------------*/
void
lpm_init(void)
{
  lpm_stats_sleeps = 0U;
}
/*---------------------------------------------------------------------------*/
void
lpm_enter(void)
{
  lpm_stats_sleeps++;
  /* Sleep: core clock gated until an exception arrives. USART2 RX, TIM1 and
   * the SysTick all keep running, so no clock state has to be rebuilt and
   * clock_adjust() must NOT be called on the way out. */
  __WFI();
}
/*---------------------------------------------------------------------------*/
void
lpm_exit(void)
{
  /* Nothing to restore for PM0. Left non-empty on purpose: the symbol has to
   * exist because the generic layer and future PM1 code call it. */
  return;
}
/*---------------------------------------------------------------------------*/
/** @} */

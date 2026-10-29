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
#ifndef RTIMER_ARCH_H_
#define RTIMER_ARCH_H_

#include "contiki.h"

/* ---------------------------------------------------------------------------
 * rtimer tick rate.
 *
 * TIM1 lives on APB2. With the 96 MHz configuration produced by
 * system_ch32v30x.c (SYSCLK_FREQ_96MHz_HSE, PPRE2 = DIV1) CK_TIM1 is 96 MHz
 * with no x2 timer multiplier to reason about. Divided by the prescaler below
 * that is 1 MHz, i.e. one rtimer tick per microsecond - the same 1 us/tick
 * granularity the GD32F502VE port uses.
 *
 * This is deliberately defined here and not in a shared header: os/sys/rtimer.h
 * includes this header before it expands RTIMER_GUARD_TIME and RTIMER_SECOND,
 * so every translation unit sees the same value without depending on include
 * order.
 * ------------------------------------------------------------------------ */
#define CH32V305_RTIMER_INPUT_HZ   96000000UL
#define CH32V305_RTIMER_PRESCALER  96U          /* 96 MHz / 96 = 1 MHz */
#define RTIMER_ARCH_SECOND     (CH32V305_RTIMER_INPUT_HZ / CH32V305_RTIMER_PRESCALER)

rtimer_clock_t rtimer_arch_now(void);
rtimer_clock_t rtimer_arch_next_trigger(void);

void rtimer_arch_schedule(rtimer_clock_t t);
void rtimer_arch_init(void);

#endif /* RTIMER_ARCH_H_ */

/**
 * @}
 * @}
 */

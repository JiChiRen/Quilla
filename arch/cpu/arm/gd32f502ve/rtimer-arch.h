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
 * TIMER0 lives on APB2. With the 160 MHz configuration produced by
 * system_gd32f50x.c, APB2 is AHB/1 = 160 MHz, and because the APB2 prescaler
 * is 1 there is no x2 timer multiplier to reason about -- CK_TIMER0 is
 * 160 MHz either way. Divided by the prescaler below that is 1 MHz, i.e. one
 * rtimer tick per microsecond.
 *
 * This is deliberately defined here and not in the shared arch/cpu/arm/
 * arm-def.h (which ties RTIMER_ARCH_SECOND to the LSI rate the PY32F040xB port
 * uses): os/sys/rtimer.h includes this header before it expands
 * RTIMER_GUARD_TIME and RTIMER_SECOND, so every translation unit sees the
 * same value without depending on include order.
 * ------------------------------------------------------------------------ */
#define GD32_RTIMER_INPUT_HZ   160000000UL
#define GD32_RTIMER_PRESCALER  160U        /* 160 MHz / 160 = 1 MHz        */
#define RTIMER_ARCH_SECOND     (GD32_RTIMER_INPUT_HZ / GD32_RTIMER_PRESCALER)

rtimer_clock_t rtimer_arch_now(void);
rtimer_clock_t rtimer_arch_next_trigger(void);

void rtimer_arch_schedule(rtimer_clock_t t);
void rtimer_arch_init(void);

#endif /* RTIMER_ARCH_H_ */

/**
 * @}
 * @}
 */

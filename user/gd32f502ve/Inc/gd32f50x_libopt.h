/*!
    \file    gd32f50x_libopt.h
    \brief   library optional for gd32f50x (Quilla port: minimal set)

    The GD32 firmware library ships no libopt header: it is a per-project
    header that selects which peripheral drivers are compiled. This one
    deliberately enables only what the GD32F502VE port uses:

      rcu    clock control        (clock.c, rtimer-arch.c, shellport.c)
      gpio   pin mux / AF         (shellport.c)
      usart  console              (shellport.c)
      dma    console RX          (shellport.c)
      timer  rtimer time base     (rtimer-arch.c)
      fmc    flash erase/program  (quilla-qboot-hal.c, QIAP)
      pmu    low-power entry      (lpm.c)
      rtc    clock cross-check    (qclk.c: the RTC divider counts IRC40K)
      fwdgt  independent watchdog (qwdt.c)
      misc   NVIC helpers         (forced, see 1 below)
      syscfg timer trigger cfg    (forced, see 2 below)

    Adding a peripheral here adds its driver .c to the link as well -- see
    C_SOURCES in user/gd32f502ve/EIDE/Makefile. Keep the two in step.

    TWO entries are forced by code we do not own, not by anything in Quilla:

    1. misc - no Quilla file needs it: NVIC configuration in this port goes
       through the CMSIS NVIC_* intrinsics in core_cm33.h, so the port does
       not depend on the vendor's priority-grouping model. It is listed anyway
       because the vendor's system_gd32f50x.c calls nvic_vector_table_set()
       from SystemInit(), and that symbol only exists when misc is enabled.

    2. syscfg - gd32f50x_timer.c uses SYSCFG_TIMER0/1/2..., SYSCFG_TIMERCFG()
       and the TIMER_SLAVE_MODE_* / TIMER_SMCFG_TRGSEL_* constants, all of
       which live in gd32f50x_syscfg.h rather than in gd32f50x_timer.h. The
       timer driver does not compile without it.
*/

/*
    Copyright (c) 2026, JiChiRen Info. Tech.
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:
    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.
    3. Neither the name of the copyright holder nor the names of its
       contributors may be used to endorse or promote products derived from
       this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef GD32F50X_LIBOPT_H
#define GD32F50X_LIBOPT_H

#include "gd32f50x_dma.h"
#include "gd32f50x_fmc.h"
#include "gd32f50x_fwdgt.h"
#include "gd32f50x_gpio.h"
#include "gd32f50x_misc.h"
#include "gd32f50x_pmu.h"
#include "gd32f50x_rcu.h"
#include "gd32f50x_rtc.h"
#include "gd32f50x_syscfg.h"
#include "gd32f50x_timer.h"
#include "gd32f50x_usart.h"

#endif /* GD32F50X_LIBOPT_H */

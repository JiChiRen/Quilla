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

#ifndef QUILLA_ARM_CONTIKI_CONF_H
#define QUILLA_ARM_CONTIKI_CONF_H

#include <stdint.h>
#include <string.h>
#include <inttypes.h>


/* Use 32-bit rtimer (default in Contiki-NG is 32) */
/* leaverage the global variable "uwTick" used as application time base*/
//#define RTIMER_CONF_CLOCK_SIZE       4

#define PROCESS_CONF_NUMEVENTS       8
#define PROCESS_CONF_STATS           1

/* Kernel safety layer: fixed size object registries, no dynamic growth. */
#define KERN_CONF_MAX_PROCESSES      8
#define KERN_CONF_MAX_TIMERS         8

/* Stack checker (os/sys/stack-check.c) requires a platform watchdog driver and
 * the linker symbols _stack / _stack_origin, neither of which this PY32F040xB port
 * provides. Disabled to avoid pulling in an unwired watchdog subsystem.
 *
 * Stack monitoring is not dropped because of it: os/sys/kern-stack.c paints a
 * watermark over the stack reservation and guards its lower boundary with a
 * canary, and kern_stack_check() reports a lost canary through kern_raise().
 * That is the software substitute for the missing MPU, so the project error
 * hook (KERN_CONF_ERROR_HOOK) is what defines the safe state on overflow.
 */
#define STACK_CHECK_CONF_ENABLED     0

/* Kernel stack watermark. The region boundaries come from the linker:
 * Stack_Mem / __initial_sp on armlink, _stack_origin / _estack on GNU ld.
 * Define KERN_CONF_STACK_BOTTOM and/or KERN_CONF_STACK_TOP to override them.
 */
#ifndef KERN_CONF_STACK_ENABLED
#define KERN_CONF_STACK_ENABLED      1
#endif
#ifndef KERN_CONF_STACK_SHELL
#define KERN_CONF_STACK_SHELL        0
#endif

/* Program-flow sequence monitoring (os/sys/kern-flow.c). Checkpoints prove
 * that the instrumented path ran, in order, inside its budget. */
#ifndef KERN_CONF_FLOW_MAX_TRACKS
#define KERN_CONF_FLOW_MAX_TRACKS    4
#endif

/* Independent watchdog (arch/cpu/arm/dev/qwdt.c).
 *
 * The timeout below is a starting point, not a result: it only becomes
 * defensible once QWDT_CONF_SAFE_TIME_MS carries the process safety time
 * from the hazard analysis, and once the LSI bounds used by dev/qwdt.h are
 * replaced with datasheet or measured values for this board. Until then
 * qwdt_describe() reports "safe=NOT STATED", which is the honest answer. */
#ifndef QWDT_CONF_TIMEOUT_MS
#define QWDT_CONF_TIMEOUT_MS         500U
#endif
#ifndef QWDT_CONF_SAFE_TIME_MS
#define QWDT_CONF_SAFE_TIME_MS       0U
#endif
/*#define PROCESS_CONF_FASTPOLL      4*/
// #define CLOCK_CONF_SECOND 100//32
/* Low-power modes (arch/cpu/arm/lpm.c), Quilla Arm M0 / PY32F040xB.
 *
 * 1 = the idle loop sleeps. PM0 (Sleep) is entered whenever there is
 * nothing to do: it stops only the core clock, so nothing has to be
 * restored and the watchdog is unaffected.
 *
 * PM1 (Stop 0) is a different matter. Stop keeps LSI and LSE alive and
 * switches everything else off, and the IWDG, which runs off the LSI,
 * keeps counting. The rtimer drives LPTIM1 with ARR = 0xFFFF and compares
 * the scheduled target in software, so the earliest interrupt the hardware
 * can deliver is the counter rollover, about two seconds - longer than the
 * watchdog window. lpm_enter() compares the moment the hardware can wake
 * the core (not the moment the software would like to wake) against that
 * window and takes PM0 whenever the comparison fails, which today means
 * always.
 *
 * PM1 therefore also needs LPM_CONF_STOP_WAKEUP_VERIFIED, and that stays 0
 * until this board has been shown to come out of Stop on the LPTIM
 * interrupt. While it is 0 the PM1 path is not even compiled.
 */
#ifndef LPM_CONF_ENABLE
#define LPM_CONF_ENABLE         1
#endif
/* Highest PM admitted: 1 = Stop 0, 0 = Sleep only. There is no PM2: this
 * port does not enter Standby, which drops the VCORE domain and returns
 * through reset rather than through the running image. */
#ifndef LPM_CONF_MAX_PM
#define LPM_CONF_MAX_PM         1
#endif
#ifndef LPM_CONF_STOP_WAKEUP_VERIFIED
#define LPM_CONF_STOP_WAKEUP_VERIFIED 0
#endif
/* 开启LPM运行统计 */
#define LPM_CONF_STATS          1
/* 外设回调最大数量 */
#define LPM_CONF_PERIPH_PERMIT_PM1_FUNCS_MAX 5

/* Quilla shell over DMA USART2 (PA2/PA3). When enabled, main.c calls
 * shellport_init() which brings up USART2 + ping-pong DMA and starts the
 * shell process. */
#define SHELLPORT_CONF_ENABLED        1

#endif /* QUILLA_ARM_CONTIKI_CONF_H */

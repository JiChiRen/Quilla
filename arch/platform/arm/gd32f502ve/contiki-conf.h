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
 *
 * ----------------------------------------------------------------------------
 * Platform configuration: GD32F502VE (Cortex-M33, 160 MHz from 8 MHz HXTAL).
 *
 * Shared by both images of this MCU (APP and QBOOT); do not put image-specific
 * values here, pass them as -D on the relevant Makefile instead.
 * ----------------------------------------------------------------------------
 */

#ifndef QUILLA_ARM_CONTIKI_CONF_H
#define QUILLA_ARM_CONTIKI_CONF_H

#include <stdint.h>
#include <string.h>
#include <inttypes.h>

/* ---- scheduler / kernel object registries -------------------------------- */

#define PROCESS_CONF_NUMEVENTS       8
#define PROCESS_CONF_STATS           1

/* Kernel safety layer: fixed size object registries, no dynamic growth. */
#define KERN_CONF_MAX_PROCESSES      8
#define KERN_CONF_MAX_TIMERS         8

/* Stack checker (os/sys/stack-check.c) requires a platform watchdog driver and
 * the linker symbols _stack / _stack_origin, neither of which this port
 * provides. Stack monitoring is not dropped because of it: os/sys/kern-stack.c
 * paints a watermark over the stack reservation and guards its lower boundary
 * with a canary, and kern_stack_check() reports a lost canary through
 * kern_raise().
 *
 * NOTE: the GD32F502VE has an ARMv8-M MPU, so unlike the PY32F040xB port this
 * part could enforce the partition in hardware. That is not wired up yet;
 * until it is, the canary remains the only overflow detector. */
#define STACK_CHECK_CONF_ENABLED     0

/* Kernel stack watermark. The region boundaries come from the linker:
 * _stack_origin / _estack on GNU ld. Define KERN_CONF_STACK_BOTTOM and/or
 * KERN_CONF_STACK_TOP to override them. */
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

/* ---- time bases ----------------------------------------------------------- */

/* RTIMER_ARCH_SECOND is NOT defined here. It comes from
 * arch/cpu/arm/gd32f502ve/rtimer-arch.h, which os/sys/rtimer.h includes before
 * it expands RTIMER_GUARD_TIME and RTIMER_SECOND. The shared
 * arch/cpu/arm/arm-def.h ties the same symbol to the LSI rate the PY32F040xB
 * port runs on, so this port simply does not include that header.
 * Current value: 1 MHz (TIMER0 off APB2, prescaled by 160). */

/* ---- low power ------------------------------------------------------------ */

/* 1 = the idle loop sleeps. PM0 is __WFI(): only the core clock is gated, so
 * USART1 RX, TIMER0 and SysTick all keep running and nothing has to be
 * restored on wake-up.
 *
 * LPM_CONF_MAX_PM is 0 on purpose. Deep-sleep would stop the APB clocks - and
 * with them the console and the rtimer - and this port has no verified
 * wake-up source for it. lpm.c rejects a higher value at compile time rather
 * than silently degrading to Sleep, so a build cannot claim a saving it does
 * not get. */
#ifndef LPM_CONF_ENABLE
#define LPM_CONF_ENABLE         1
#endif
#ifndef LPM_CONF_MAX_PM
#define LPM_CONF_MAX_PM         0
#endif
#define LPM_CONF_STATS          1

/* ---- console -------------------------------------------------------------- */

/* Quilla shell over USART1 (PA2/PA3). When enabled, main.c calls
 * shellport_init() which brings up USART1 and starts the shell process. */
#define SHELLPORT_CONF_ENABLED        1

#endif /* QUILLA_ARM_CONTIKI_CONF_H */

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
 * \defgroup quilla-riscv-cpu Quilla RISC-V CPU
 *
 * CPU-specific dispatch for the Quilla RISC-V (WCH CH32V30x / QingKe-class)
 * ports. Mirrors arch/cpu/arm/cpu.h: it pulls in the device header of the MCU
 * currently being built (which drags in the RISC-V core access layer,
 * core_riscv.h) and exposes the arch-neutral interrupt master macros the rest
 * of the tree uses. No generic file may hard-code a single vendor HAL.
 *
 * \file
 * Header file with prototypes for interrupt control on the Quilla RISC-V ports.
 */
#ifndef CPU_H_
#define CPU_H_

/*
 * Pull in the device header of the MCU currently being built. It drags in the
 * matching RISC-V core header (core_riscv.h), which is what __enable_irq() /
 * __disable_irq() below actually need (WCH PFIC / mstatus intrinsics). The
 * device macro comes from the per-MCU build (user/<mcu>/EIDE/Makefile*); no
 * generic file may hard-code a single vendor HAL.
 */
#if defined(CH32V30x_D8C) || defined(CH32V30x_D8)
#include "ch32v30x.h"
#else
#error "cpu.h: unknown target MCU - define the device macro (CH32V30x_D8C for CH32V305x) in the build"
#endif

/** \brief Enables all CPU interrupts (sets mstatus.MIE via the WCH PFIC) */
#define INTERRUPTS_ENABLE()  __enable_irq()

/** \brief Disables all CPU interrupts (clears mstatus.MIE via the WCH PFIC). */
#define INTERRUPTS_DISABLE() __disable_irq()

#endif /* CPU_H_ */

/**
 * @}
 * @}
 */

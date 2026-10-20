/*
 * Copyright (c) 2012, Texas Instruments Incorporated - http://www.ti.com/
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
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * \addtogroup arm
 * @{
 *
 * \defgroup quilla-arm-m0-cpu Quilla Arm M0 CPU
 *
 * CPU-specific functions for the Quilla Arm M0 (Cortex-M0+) core
 * @{
 *
 * \file
 * Header file with prototypes for interrupt control on the Quilla Arm M0
 * Cortex-M0+ micro (PY32F040xB)
 *
 */
#ifndef CPU_H_
#define CPU_H_

/*
 * Pull in the device header of the MCU currently being built. It drags in the
 * matching CMSIS core header (core_cm0plus.h / core_cm33.h), which is what
 * __enable_irq() / __disable_irq() below actually need. The MCU macro comes
 * from the per-MCU build (user/<mcu>/EIDE/Makefile*, .uvprojx); no generic
 * file may hard-code a single vendor HAL.
 */
#if defined(PY32F040xB)
#include "py32f0xx_hal.h"
#elif defined(GD32F50X)
#include "gd32f50x.h"
#else
#error "cpu.h: unknown target MCU - define the device macro (e.g. PY32F040xB, GD32F50X) in the build"
#endif

/** \brief Enables all CPU interrupts */
#define INTERRUPTS_ENABLE()  __enable_irq()

/** \brief Disables all CPU interrupts. */
#define INTERRUPTS_DISABLE() __disable_irq()

#endif /* CPU_H_ */

/**
 * @}
 * @}
 */

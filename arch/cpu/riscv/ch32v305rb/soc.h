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
/**
 * \addtogroup riscv
 * @{
 *
 * \defgroup quilla-ch32v305rb-soc Quilla CH32V305VB SoC
 *
 * SoC-level definitions for the CH32V305VB (WCH CH32V30x_D8C, RISC-V QingKe
 * core). 128 KiB Flash + 32 KiB SRAM; the boot images are linked at the
 * 0x00000000 Flash alias (the WCH startup/linker convention), which is why the
 * per-build QUILLA_QBOOT_* macros for this MCU are 0x0000-based rather than the
 * 0x0800-based values the Cortex-M ports use.
 * @{
 *
 * \file
 * Header file with macro and function declarations for the Quilla CH32V305VB SoC
 */
#ifndef SOC_H_
#define SOC_H_

#include "contiki.h"

#include <stdint.h>
/*----------------------------------------------------------------------------*/
/** \name SoC features
 * @{
 */
#define SOC_FEATURE_AES_SHA     0x00000002 /**< reserved: no SHA HW on CH32V305 */
#define SOC_FEATURE_ECC_RSA     0x00000001 /**< reserved: no ECC HW on CH32V305 */
/** @} */
/*----------------------------------------------------------------------------*/
/** \name Flash geometry
 * @{
 */
/** \brief Last address (inclusive) of the internal Flash.
 *
 *  This is a pure MCU hardware property and is the single source for
 *  QUILLA_QBOOT_APP_END (see os/services/quilla-qboot/quilla-qboot-hal.h).
 *  Every Quilla MCU port MUST define this in its arch/cpu/<isa>/<mcu>/soc.h.
 *
 *  CH32V305RB/VB = 128 KiB Flash linked at the 0x00000000 alias, so the last
 *  address is 0x0001FFFF (NOT the 0x0801FFFF the alias region would suggest:
 *  the boot images and the whole QBOOT/APP scheme for this MCU live at 0x0).
 */
#define SOC_FLASH_END          0x0001FFFFUL  /**< CH32V305VB: 128 KiB flash @ 0x0 */
/** @} */
/*----------------------------------------------------------------------------*/
/** \name SoC functions
 * @{
 */
uint8_t  soc_get_rev(void);
uint32_t soc_get_sram_size(void);
uint32_t soc_get_features(void);
void     soc_print_info(void);
void     soc_init(void);
/** @} */

#endif /* SOC_H_ */

/**
 * @}
 * @}
 */

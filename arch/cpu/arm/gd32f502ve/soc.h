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
 * \addtogroup arm
 * @{
 *
 * \defgroup quilla-gd32f502ve-soc Quilla GD32F502VE SoC
 *
 * Driver for the GD32F502VE (Cortex-M33, 160 MHz from an 8 MHz HXTAL)
 * @{
 *
 * \file
 * Header file with macro and function declarations for the Quilla GD32F502VE SoC
 */
#ifndef SOC_H_
#define SOC_H_

#include "contiki.h"

#include <stdint.h>
/*----------------------------------------------------------------------------*/
/** \name SoC features
 * @{
 */
#define SOC_FEATURE_AES_SHA     0x00000002 /**< Security HW AES/SHA */
#define SOC_FEATURE_ECC_RSA     0x00000001 /**< Security HW ECC/RSA */
/** @} */
/*----------------------------------------------------------------------------*/
/** \name Flash geometry
 * @{
 */
/** \brief Last address (inclusive) of the internal Flash.
 *  This is a pure MCU hardware property and is the single source for
 *  QUILLA_QBOOT_APP_END (see os/services/quilla-qboot/quilla-qboot-hal.h).
 *  Every Quilla MCU port MUST define this in its arch/cpu/arm/<mcu>/soc.h; do
 *  not pass it on the build command line.
 */
#define SOC_FLASH_END          0x0807FFFFUL  /**< GD32F502VE: 512 KiB flash */
/** @} */
/*----------------------------------------------------------------------------*/
/** \name OTP (one-time-programmable) geometry
 * @{
 */
/** \brief Whether this MCU provides a genuine securted-programmable region.
 *
 *  This is a pure MCU hardware property, the single source for
 *  QUILLA_QBOOT_SECPROG_AVAILABLE (see os/services/quilla-qboot/
 *  quilla-qboot-hal.h). Every Quilla MCU port MUST define it in its
 *  arch/cpu/<arch>/<mcu>/soc.h: 1 = the manual documents an OTP area whose
 *  cells can be programmed once and never erased; 0 = no such area. The boot
 *  boot derives the hardware-counter feature level from this flag and the
 *  WRP availability flag (see QUILLA_QBOOT_HW_COUNTER_LEVEL in
 *  quilla-qboot-secure.h).
 *
 *  GD32F502VE: GD32F50x user manual, FMC chapter. OTP2 data block,
 *  256 one-time-programmable bytes at 0x1FF20000. Byte programming is
 *  supported (fmc_otp_byte_program); each byte cell accepts exactly ONE
 *  program operation and can never be erased, which is exactly the property
 *  the hardware monotonic counter needs.
 */
#define SOC_OTP_AVAILABLE      1UL
#define SOC_OTP_BASE           0x1FF20000UL  /**< OTP2 data block base */
#define SOC_OTP_SIZE           256UL         /**< OTP2 data block, 256 bytes */

/**
 * Counter cell granularity for the hardware monotonic counter, in bytes.
 * GD32 OTP cells are byte-granular and accept exactly one program operation,
 * so one cell is one byte.
 */
#define SOC_OTP_CELL_SIZE      1UL
/** @} */
/*----------------------------------------------------------------------------*/
/** \name SoC functions
 * @{
 */

/** \brief Gets the SoC revision
 * \return The SoC revision as a byte with nibbles representing the major and
 * minor revisions
 */
uint8_t soc_get_rev(void);

/** \brief Gets the SRAM size of the SoC
 * \return The SRAM size in bytes
 */
uint32_t soc_get_sram_size(void);

/** \brief Gets the hardware features of the SoC that are enabled
 * \return The enabled hardware features as a bitmask of \c SOC_FEATURE_x values
 */
uint32_t soc_get_features(void);

/** \brief Prints SoC information */
void soc_print_info(void);

/** \brief Common initialisation routine for this platform */
void soc_init(void);

/** @} */

#endif /* SOC_H_ */

/**
 * @}
 * @}
 */

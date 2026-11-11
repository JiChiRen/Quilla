/*
 * Copyright (c) 2016, Benoît Thébaudeau <benoit.thebaudeau.dev@gmail.com>
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
 * \defgroup quilla-arm-m0-soc Quilla Arm M0 SoC
 *
 * Driver for the Quilla Arm M0 SoC (PY32F040xB)
 * @{
 *
 * \file
 * Header file with macro and function declarations for the Quilla Arm M0 SoC
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
#define SOC_FLASH_END          0x0801FFFFUL  /**< PY32F040xB: 128 KiB flash */
/** @} */
/*----------------------------------------------------------------------------*/
/** \name OTP (one-time-programmable) geometry
 * @{
 */
/** \brief No genuine OTP area on this MCU.
 *
 *  Every Quilla MCU port MUST define SOC_OTP_AVAILABLE in its
 *  arch/cpu/<arch>/<mcu>/soc.h (see the GD32F502VE soc.h for the full
 *  contract). The PY32F040 reference manual documents only the Information
 *  block (FT bytes / UID / Option bytes / system memory) -- none of it is a
 *  one-time-programmable user area, so the hardware monotonic counter is not
 *  available and the QBOOT build emits a warning. The anti-rollback floor
 *  then lives solely in the rewritable META flash log (quilla-qboot-meta.c).
 */
#define SOC_OTP_AVAILABLE      0UL

/**
 * \name Write-protected NV counter region (WRP-protected main flash)
 *
 * PY32F040 has no true OTP, but its main flash supports write protection
 * (WRP) with an 8 KB granularity. When the WPP level is selected
 * (QUILLA_QBOOT_HW_COUNTER_LEVEL = QUILLA_QBOOT_HW_COUNTER_WPP), QBOOT
 * reserves the TOP 8 KB sector as the counter region and keeps it
 * write-protected at all times except inside the burn window
 * (option-byte change + reload reset).
 *
 * Selecting the WPP level therefore RESERVES the top 8 KB of main flash:
 * application images must not extend into it (QIAP erase/write of the
 * protected sector fails loudly with WRPERR). Without the WPP level selected
 * the reservation costs nothing -- no resets, no option-byte wear, no flash
 * loss beyond what the linker already places.
 *
 * This is CONFIGURABLE hardware, not silicon physics: an attacker able to
 * reprogram the option bytes can drop the protection. See
 * doc/anti-rollback-counter.md section 2.4 for the security annotation.
 *
 * Every Quilla MCU port that lacks true OTP MUST declare either
 * SOC_WPP_AVAILABLE=1 (with BASE/SIZE/CELL_SIZE, one whole WRP
 * sector) or SOC_WPP_AVAILABLE=0.
 * @{
 */
#define SOC_WPP_AVAILABLE  1UL
#define SOC_OTP_WRPROT_BASE       0x0801E000UL  /**< top 8 KB WRP sector (16th of 16) */
#define SOC_OTP_WRPROT_SIZE       0x00002000UL  /**< one whole WRP sector, 8 KB */
/** Programming granularity: PY32F040 flash programs whole 256-byte pages
 * (FLASH_PAGE_SIZE); one counter cell is therefore one page. */
#define SOC_OTP_CELL_SIZE         256UL

/* The hardware-counter enablement LEVEL is a Quilla system-level decision
 * (QUILLA_QBOOT_HW_COUNTER_LEVEL), defined in
 * os/services/quilla-qboot/quilla-qboot-secure.h and defaulting to WPP for this
 * WRP-only part. It is intentionally NOT set here: keeping the switch at the
 * system level lets GCC and Keil share one source of truth with no
 * build-command macro. The APP partition is shrunk to match the reserved
 * counter region in quilla-qboot-hal.h. */
/** @} */
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

/** \brief Common initialisation routine for all Quilla Arm M0-based platforms */
void soc_init(void);

/** @} */

#endif /* SOC_H_ */

/**
 * @}
 * @}
 */

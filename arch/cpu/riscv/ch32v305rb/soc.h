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
/** \name OTP (one-time-programmable) geometry
 * @{
 */
/** \brief No genuine OTP area on this MCU.
 *
 *  Every Quilla MCU port MUST define SOC_OTP_AVAILABLE in its
 *  arch/cpu/<arch>/<mcu>/soc.h (see the GD32F502VE soc.h for the full
 *  contract). The CH32V305 datasheet documents a 128-byte user selection-word
 *  area (0x1FFFF800), but it is ordinary rewritable flash -- WCH states that
 *  a full-chip programming erases it -- so it does NOT qualify as
 *  one-time-programmable and must not back a hardware monotonic counter. The
 *  QBOOT build emits a warning; the anti-rollback floor then lives solely in
 *  the rewritable META flash log (quilla-qboot-meta.c).
 */
#define SOC_OTP_AVAILABLE      0UL

/**
 * \name Write-protected NV counter region (WRP-protected main flash)
 *
 * CH32V305 has no true OTP (see above), but its main flash supports write
 * protection: 32 protection sectors of 4 KB each (FLASH_WRProt_SectorsN,
 * bits stored in the user selection words, applied after an option-byte
 * reload). When the WPP level is selected
 * (QUILLA_QBOOT_HW_COUNTER_LEVEL = QUILLA_QBOOT_HW_COUNTER_WPP), QBOOT
 * reserves the TOP 4 KB sector as the counter region and keeps it
 * write-protected at all times except inside the burn window.
 *
 * Selecting the WPP level therefore RESERVES the top 4 KB of main flash:
 * application images must not extend into it (QIAP erase/write of the
 * protected sector fails loudly with WRPRTERR). Without the WPP level
 * selected the reservation costs nothing -- no resets, no option-byte wear,
 * no flash loss beyond what the linker already places.
 *
 * This is CONFIGURABLE hardware, not silicon physics: an attacker able to
 * reprogram the user selection words can drop the protection. Note also
 * that a WCH forum report claims the selection words become read-only
 * after read protection is enabled; whether a soft reset reloads the
 * selection words (vs. power-on only) must be confirmed on hardware.
 * See doc/anti-rollback-counter.md section 2.4 for the security annotation.
 *
 * Every Quilla MCU port that lacks true OTP MUST declare either
 * SOC_WPP_AVAILABLE=1 (with BASE/SIZE/CELL_SIZE, one whole WRP
 * sector) or SOC_WPP_AVAILABLE=0.
 * @{
 */
#define SOC_WPP_AVAILABLE  1UL
#define SOC_OTP_WRPROT_BASE       0x0001F000UL  /**< top 4 KB WRP sector (32nd of 32) */
#define SOC_OTP_WRPROT_SIZE       0x00001000UL  /**< one whole WRP sector, 4 KB */
/** Programming granularity: the standard programming mode writes 16-bit
 * halfwords, but the vendor FLASH_ProgramWord() (two halfword operations)
 * is the established primitive in this port, so one counter cell is one
 * 32-bit word. */
#define SOC_OTP_CELL_SIZE         4UL

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

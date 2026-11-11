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
 * \addtogroup quilla-qboot
 * @{
 *
 * \defgroup quilla-qboot-hal Boot hardware abstraction (platform-implemented)
 *
 * The boot modules under os/services/quilla-qboot are hardware independent: the
 * flag store and the OTA state machine deal in addresses, byte buffers and
 * CRCs only. Every operation that actually touches the machine is declared
 * here and implemented once per platform (PY32F040xB: arch/cpu/arm/
 * quilla-qboot-hal.c) - the same split the OS already uses for int-master.h
 * and clock.h.
 *
 * A platform must provide all five functions below. The partition layout is
 * also platform property; the defaults here describe PY32F040xB (128 KB) and
 * may be overridden per build (e.g. -DQUILLA_QBOOT_APP_START=...) rather than
 * being edited in this file.
 *
 * @{
 */

#ifndef QUILLA_QBOOT_HAL_H_
#define QUILLA_QBOOT_HAL_H_

#include <stdint.h>

#include "sys/int-master.h"
#include "soc.h"
#include "quilla-qboot-secure.h"

/* ---- partition layout (platform property, overridable per build) -------- */

/* Programmable Flash erase/program granularity in bytes. Both the flag page
 * and the OTA staging buffers are sized from this. The boot path erases Flash
 * only at this granularity, so partition boundaries only need to be
 * page-aligned, not sector-aligned. Override per build (GD32F502VE uses 4096).
 */
#ifndef QUILLA_QBOOT_PAGE_SIZE
#define QUILLA_QBOOT_PAGE_SIZE  256U
#endif

/* META region (flag record + secure-boot manifest) sits immediately AFTER the
 * QBOOT image and BEFORE the APP. Because it is anchored at a fixed offset
 * from flash base that depends only on the QBOOT size (24 KiB), the APP image
 * needs no per-MCU address adjustment beyond the flash size itself. The boot
 * path erases Flash only at page granularity, so the META region is not
 * required to be sector-aligned -- only page-aligned. */
#ifndef QUILLA_QBOOT_META_SIZE
#define QUILLA_QBOOT_META_SIZE   0x1000U   /* 4 KiB: wear-levelled flag + manifest log */
#endif

#ifndef QUILLA_QBOOT_META_BASE
#define QUILLA_QBOOT_META_BASE  0x08006000U  /* QBOOT end (24 KiB) */
#endif

#ifndef QUILLA_QBOOT_APP_START
#define QUILLA_QBOOT_APP_START  (QUILLA_QBOOT_META_BASE + QUILLA_QBOOT_META_SIZE)
#endif
/* -> 0x08007000 */

/* Last address of the APP region, inclusive. Single point of definition -- no
 * #undef/redefine ordering trick. With the WPP counter enabled, the top WRP
 * sector is reserved as the monotonic-counter region, so the APP region must
 * end before the counter sector (APP_END is inclusive: one byte below the
 * counter sector base; a QIAP erase/write there fails loudly with WRPERR).
 * Otherwise it equals the last address of the internal Flash, supplied by the
 * port's soc.h as SOC_FLASH_END -- never hard-coded on the build command line.
 * A port that forgets SOC_FLASH_END fails loudly here. An explicit
 * -DQUILLA_QBOOT_APP_END still overrides (e.g. for a custom flash map). */
#if QUILLA_QBOOT_OTP_COUNTER && QUILLA_QBOOT_WPP
#ifndef QUILLA_QBOOT_APP_END
#define QUILLA_QBOOT_APP_END  (QUILLA_QBOOT_OTP_BASE - 1U)
#endif
#else /* no WPP counter region reserved */
#ifndef QUILLA_QBOOT_APP_END
#ifndef SOC_FLASH_END
#error "QUILLA_QBOOT_APP_END not defined: add SOC_FLASH_END (= last flash address) to arch/cpu/arm/<mcu>/soc.h"
#endif
#define QUILLA_QBOOT_APP_END    SOC_FLASH_END
#endif
#endif

/* Base of the internal flash the boot images are linked into. */
#ifndef QUILLA_QBOOT_FLASH_BASE
#define QUILLA_QBOOT_FLASH_BASE  FLASH_BASE
#endif

/* Byte offset of the APP image from the flash base. This is the single source
 * of truth for the partition boundary: the APP side uses it for its vector
 * table relocation (SCB->VTOR = FLASH_BASE | QUILLA_QBOOT_APP_OFFSET) instead
 * of hard-coding the distance to the QIAP image. */
#ifndef QUILLA_QBOOT_APP_OFFSET
#define QUILLA_QBOOT_APP_OFFSET (QUILLA_QBOOT_APP_START - QUILLA_QBOOT_FLASH_BASE)
#endif

/* The boot path erases Flash only at page granularity (PAGE_SIZE), never at
 * sector granularity, so the APP region only needs to be page-aligned. A
 * non-page-aligned APP_START would make the QIAP page-coalescing logic
 * (quilla-qiap.c) split incoming frames across a page boundary incorrectly.
 * If a sector-erase optimisation is ever introduced, tighten this back to a
 * sector check.
 *
 * This check has to sit after QUILLA_QBOOT_FLASH_BASE: an identifier that is
 * not yet defined evaluates to 0 in #if, which would make the arithmetic
 * silently meaningless. */
#if (QUILLA_QBOOT_APP_OFFSET % QUILLA_QBOOT_PAGE_SIZE) != 0U
#error "QUILLA_QBOOT_APP_START is not on a page boundary"
#endif

/* RAM range, used to validate the APP initial stack pointer before jumping. */
#ifndef QUILLA_QBOOT_RAM_START
#define QUILLA_QBOOT_RAM_START  0x20000000U
#endif
#ifndef QUILLA_QBOOT_RAM_SIZE
#define QUILLA_QBOOT_RAM_SIZE   0x4000U   /* 16 KB on PY32F040xB */
#endif


/* Attribute applied to page-sized staging buffers. Flash programmers on
 * Cortex-M0+ fault on unaligned word accesses, so callers must hand
 * quilla_qboot_hal_flash_write_page() a word-aligned buffer. Override for
 * compilers that spell this differently. */
#ifndef QUILLA_QBOOT_ALIGNED
#define QUILLA_QBOOT_ALIGNED  __attribute__((aligned(4)))
#endif

/* Unlock key presented by the host in the ENTER frame (default "QULLOTA1").
 * Override per product/build. */
#ifndef QUILLA_QBOOT_UNLOCK_KEY
#define QUILLA_QBOOT_UNLOCK_KEY {0x51,0x55,0x4C,0x4C,0x4F,0x54,0x41,0x31}
#endif

/* ---- platform-provided operations --------------------------------------- */

/**
 * \brief     Send raw bytes on the upgrade link (blocking)
 * \param data Bytes to send
 * \param len  Byte count
 *
 * Used by the OTA state machine for ACK/NAK frames. On the QIAP image this is
 * the same UART as the shell; platforms that do not wire an upgrade link may
 * provide an empty body.
 */
void quilla_qboot_hal_transmit(const uint8_t *data, uint16_t len);

/**
 * \brief     Copy len bytes from a raw (Flash) address into dst
 * \param addr Source address, any alignment
 * \param dst  Destination buffer, at least len bytes
 * \param len  Byte count
 *
 * Program Flash on this family is memory mapped, but going through the HAL
 * keeps the boot modules portable to parts that need a read routine.
 */
void quilla_qboot_hal_flash_read(uint32_t addr, uint8_t *dst, uint32_t len);

/**
 * \brief          Erase and reprogram exactly one Flash page
 * \param page_base Page-aligned start address (multiple of PAGE_SIZE)
 * \param src      Buffer holding a full page of bytes
 * \retval 0  success
 * \retval <0 failure (unlock / erase / program)
 *
 * The caller stages the page in RAM, so a partial-page update is expressed as
 * read-modify-erase-write by the caller - not by the platform.
 */
int quilla_qboot_hal_flash_write_page(uint32_t page_base, const uint8_t *src);

/**
 * \brief          Program already-erased Flash, WITHOUT erasing
 * \param addr     Word-aligned destination (must lie in an erased region)
 * \param src      Buffer of \p len bytes (len multiple of 4)
 * \param len      Byte count to program
 * \retval 0  success
 * \retval <0 failure (unlock / program / alignment)
 *
 * Used by the meta-store to APPEND records into a slot: the destination must
 * already be erased (a slot is pre-erased before it becomes the write target),
 * so this routine never touches the erase controller and therefore never
 * disturbs valid data that shares the same physical page. That is what lets the
 * active (only-valid) slot be written without an erase window.
 */
int quilla_qboot_hal_flash_program(uint32_t addr, const uint8_t *src, uint32_t len);

/**
 * \brief          Erase every page covering [base, base+size)
 * \param base     Page-aligned start address
 * \param size     Byte count (multiple of PAGE_SIZE)
 * \retval 0  success
 * \retval <0 failure
 *
 * Used to clear a whole standby slot before it becomes the write target.
 */
int quilla_qboot_hal_flash_erase_range(uint32_t base, uint32_t size);

/**
 * \brief Request a system reset
 *
 * Disables interrupts first so no handler runs while the flag page has just
 * been rewritten. Does not return.
 */
void quilla_qboot_hal_reboot(void);

/**
 * \brief          Hand control over to the application image
 * \param app_start Base address of the APP vector table
 *
 * Relocates the vector table, loads the APP stack pointer and branches to its
 * reset handler. Does not return.
 */
void quilla_qboot_hal_jump_to_app(uint32_t app_start);

/**
 * \brief          Mechanical sanity test of the APP image at QUILLA_QBOOT_APP_START
 * \retval 1  the region looks like a runnable image
 * \retval 0  blank / corrupted / not an image
 *
 * Cortex-M ports check the initial stack pointer (inside RAM) and the reset
 * vector (inside the APP region, Thumb bit set); the RISC-V port checks the
 * JAL opcode of the entry word. This is a presence test only -- when a key is
 * provisioned the signature gate (quilla_qboot_secure_verify_app()) has the
 * final say. Implemented per port because the test is CPU-arch specific.
 */
int quilla_qboot_hal_app_image_valid(void);

/**
 * \brief          Program ONE byte into the MCU OTP area, WITHOUT erasing
 * \param addr     Absolute OTP address (inside [QUILLA_QBOOT_OTP_BASE,
 *                 QUILLA_QBOOT_OTP_BASE + QUILLA_QBOOT_OTP_SIZE))
 * \param value    Byte to program (0x00 burns the cell)
 * \retval 0  success
 * \retval <0 failure (not OTP / cell already programmed / program error)
 *
 * OTP cells accept exactly ONE program operation and can never be erased, so
 * the caller must target a still-erased (0xFF) cell; this routine refuses a
 * cell that does not read 0xFF beforehand. Used only by the hardware monotonic
 * counter (quilla-qboot-otp.c) and only when QUILLA_QBOOT_OTP_COUNTER is
 * enabled on a part with QUILLA_QBOOT_SECPROG_AVAILABLE = 1. Ports without an
 * OTP area implement this as a stub returning -1; the service layer never
 * calls it there.
 */
int quilla_qboot_hal_otp_program_byte(uint32_t addr, uint8_t value);

/**
 * \brief          Program ONE counter cell (all zeros), WITHOUT erasing
 * \param addr     Absolute cell address (inside [QUILLA_QBOOT_OTP_BASE,
 *                 QUILLA_QBOOT_OTP_BASE + QUILLA_QBOOT_OTP_SIZE)), cell
 *                 aligned (QUILLA_QBOOT_OTP_CELL_SIZE)
 * \retval 0  success
 * \retval <0 failure (cell not virgin / program error / write-protection
 *            error)
 *
 * The cell is the MCU's programming granularity (QUILLA_QBOOT_OTP_CELL_SIZE):
 * one OTP byte on GD32, one whole 256-byte page on PY32, one 32-bit word on
 * CH32. The caller must target a still-erased cell; this routine refuses a
 * cell that does not read all-0xFF beforehand and verifies the burned value
 * by read-back. Used only by the hardware monotonic counter
 * (quilla-qboot-otp.c) when QUILLA_QBOOT_OTP_COUNTER is enabled.
 */
int quilla_qboot_hal_otp_program_cell(uint32_t addr);

/**
 * \brief  Current write-protection state of the counter region
 * \retval 1 the region is write-protected (or requested to be: for parts
 *           whose option bytes reload on reset, this reflects the stored
 *           configuration, not the live hardware latch)
 * \retval 0 the region is NOT write-protected
 *
 * Only meaningful on parts with SOC_WPP_AVAILABLE=1 (write-protected NV counter);
 * true-OTP parts implement this as a constant 0 and never see it called.
 */
int quilla_qboot_hal_otp_wrprot_status(void);

/**
 * \brief     Open or close the write-protection window of the counter region
 * \param protect 1 = write-protect the region, 0 = lift the protection
 *
 * Implemented by rewriting the option bytes (WRP configuration) and then
 * triggering the option-byte reload, which on parts like PY32 happens
 * through OBL_LAUNCH and RESETS THE CHIP. This routine therefore NEVER
 * RETURNS in normal operation; callers must treat it as a reset point. If
 * the option-byte reload only happens on power-on reset on some parts, the
 * request still lands in the stored configuration (verified by reading the
 * raw option bytes back) and takes effect at the next power cycle.
 */
void quilla_qboot_hal_otp_wrprot_set(int protect);

/**
 * \brief     Enter and leave a critical section around the RX ring
 * \return   Opaque previous interrupt state, for quilla_qboot_hal_int_restore()
 *
 * Thin forwarder onto the platform interrupt master (int_master_*) so the QIAP
 * entry does not need CMSIS intrinsics.
 */
int_master_status_t quilla_qboot_hal_int_disable(void);

/**
 * \brief    Restore the interrupt state returned by quilla_qboot_hal_int_disable()
 * \param st Previously saved state
 */
void quilla_qboot_hal_int_restore(int_master_status_t st);

#endif /* QUILLA_QBOOT_HAL_H_ */
/** @} */
/** @} */

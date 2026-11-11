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
 *
 * ----------------------------------------------------------------------------
 * GD32F502VE implementation of the boot hardware abstraction declared in
 * os/services/quilla-qboot/quilla-qboot-hal.h.
 *
 * This is the only place in the boot path that talks to the FMC. It is
 * deliberately free of protocol and policy logic so the OS-side modules stay
 * portable.
 *
 * Flash geometry: the GD32F502VE is a 512 KiB single-bank part whose bank0
 * erases in 2 KiB pages and programs 32-bit words (GD32F502xx Datasheet,
 * section 3.2: "page size is 2KB for bank0"). The page size therefore comes
 * from the build (-DQUILLA_QBOOT_PAGE_SIZE=2048) and not from this file, so
 * the image and the linker script cannot disagree. The 4 KiB META region is a
 * wear-levelled dual-slot log (quilla-qboot-meta.c): SLOT_A at 0x08006000 and
 * SLOT_B at 0x08006800, each one 2 KiB page. Records are appended with
 * quilla_qboot_hal_flash_program (no erase) into whichever slot is active; only
 * the standby slot is ever erased. The flag, the anti-rollback floor, the
 * sealed-image record and the secure-boot manifest are all fields / blocks of a
 * record -- none of them owns a dedicated page any more.
 * ----------------------------------------------------------------------------
 */

#include "../../../../os/services/quilla-qboot/quilla-qboot-hal.h"

#include <stdint.h>
#include <string.h>

#include "gd32f50x.h"
#include "gd32f50x_fmc.h"
#include "sys/int-master.h"

#ifdef QBOOT_BUILD
#include "shellport.h"
#endif

void
quilla_qboot_hal_flash_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
  const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)addr;
  uint32_t i;

  for(i = 0U; i < len; i++) {
    dst[i] = src[i];
  }
}

/* ----------------------------------------------------------------------------
 * Flash page writer for the GD32F502VE (512 KiB, single bank0, 2 KiB pages).
 *
 * TRUE ROOT CAUSE OF THE ORIGINAL FAILURE: page-size mismatch. The F502VE's
 * bank0 erases in 2 KiB pages, but the build passed QUILLA_QBOOT_PAGE_SIZE=4096U.
 * The FMC erases only the 2 KiB physical page at the address, so the second 2 KiB
 * of every 4 KiB "page" was never erased; programming/blank-checking it then hit
 * non-erased cells (PGERR / "not blank"). Fixing the page size to 2048 makes this
 * routine erase and program exactly the physical page -- which is the real fix.
 *
 * DEFENSIVE MEASURES RETAINED: the whole sequence still runs from SRAM with IRQs
 * masked, and NWLDE (zero-wait area) is cleared inside the routine. These were
 * added while investigating and are kept as defence-in-depth against any residual
 * read-while-write hazard on the M33 part (the silicon tolerates flash-resident
 * flash ops, but a safety-critical bootloader gains nothing by relying on that).
 * They are NOT the fix; the page size is. They can be removed once the page-size
 * fix is confirmed on hardware, if Ali prefers the simpler flash-resident path.
 *
 * gd32_flash_page_write_ram() MUST stay self-contained: no calls into flash-
 * resident code, no references to static/global data. It is copied verbatim into
 * g_flash_ram_buf and executed there.
 * ----------------------------------------------------------------------------
 */

/* GD32 FMC unlock keys (public silicon constants; hard-coded so the RAM copy
 * carries no dependency on header macros). */
#define GD32_FMC_UNLOCK0  0x45670123U
#define GD32_FMC_UNLOCK1  0xCDEF89ABU

typedef int (*gd32_flash_ram_fn_t)(uint32_t, const uint8_t *, uint32_t);

/* SRAM scratch where the flash writer is relocated before execution. */
static uint8_t g_flash_ram_buf[1024U] __attribute__((aligned(4)));

__attribute__((section(".text.gd32_flash_ram"), used, noinline))
static int
gd32_flash_page_write_ram(uint32_t page_base, const uint8_t *src, uint32_t page_size)
{
  uint32_t off;
  uint32_t to;

  /* Return codes (diagnostic encoding, surfaced through the QIAP NAK reason):
   *   0 = success
   *   2 = erase phase, PGERR          3 = erase phase, WPERR
   *   4 = erase phase, busy wedge     5 = erase "done" but page not blank
   *   6 = program phase, PGERR        7 = program phase, WPERR
   *   8 = program phase, busy wedge
   * Code 5 means the FMC reported a clean erase (BUSY cleared, no error flag)
   * yet the page still reads non-0xFFFFFFFF: a genuine erase failure that is no
   * longer expected once the page size matches the 2 KiB physical page. */

  /* Clear any latched error flags (write-1-to-clear on STAT0). */
  FMC_STAT0 |= (uint32_t)(FMC_STAT0_PGERR | FMC_STAT0_WPERR | FMC_STAT0_ENDF);

  /* Unlock bank0 if locked. */
  if((FMC_CTL0 & FMC_CTL0_LK) != 0U) {
    FMC_KEY0 = GD32_FMC_UNLOCK0;
    FMC_KEY0 = GD32_FMC_UNLOCK1;
  }

  /* Disable the "no-wait area" (NWLDE). On the F50x the NWA keeps the flash
   * bank it is itself erasing/programming at zero wait, which conflicts with
   * the operation and with the read-back used to verify it: the read returns
   * stale (erased-looking) data while the underlying cells were never blanked,
   * so the program phase later fails with PGERR on non-erased bits. With NWA
   * off, flash goes through FMC_WS wait states (set to 4 for 160 MHz AHB in
   * clock_init) and every read is truthful. We leave it off afterwards; the
   * rest of QBOOT simply runs at 4 wait states, which is correct at 160 MHz. */
  FMC_CTL0 &= ~FMC_CTL0_NWLDE;

  /* Wait until not busy (guards against a wedged controller). */
  to = 0x04000000U;
  while(((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) && (to != 0U)) { to--; }
  if((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) {
    FMC_CTL0 |= FMC_CTL0_LK;
    return 4;
  }

  /* Page erase. */
  FMC_CTL0 |= FMC_CTL0_PER;
  FMC_ADDR0 = page_base;
  FMC_CTL0 |= FMC_CTL0_START;
  __NOP();
  __NOP();
  to = 0x04000000U;
  while(((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) && (to != 0U)) { to--; }
  if((FMC_STAT0 & (FMC_STAT0_PGERR | FMC_STAT0_WPERR)) != 0U) {
    FMC_CTL0 &= ~FMC_CTL0_PER;
    FMC_CTL0 |= FMC_CTL0_LK;
    return ((FMC_STAT0 & FMC_STAT0_PGERR) != 0U) ? 2 : 3;
  }
  if((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) {
    FMC_CTL0 &= ~FMC_CTL0_PER;
    FMC_CTL0 |= FMC_CTL0_LK;
    return 4;
  }
  FMC_CTL0 &= ~FMC_CTL0_PER;
  FMC_STAT0 |= FMC_STAT0_ENDF;

  /* Verify the erase genuinely blanked the WHOLE page before programming. With
   * NWA disabled the reads are now truthful; if any word is not 0xFFFFFFFF the
   * erase was silently aborted (read-while-write) and we report it (5) rather
   * than let the program phase fail cryptically with PGERR. */
  for(off = 0U; off < page_size; off += 4U) {
    if(REG32(page_base + off) != 0xFFFFFFFFU) {
      FMC_CTL0 |= FMC_CTL0_LK;
      return 5;
    }
  }

  /* Program one 32-bit word at a time. */
  for(off = 0U; off < page_size; off += 4U) {
    uint32_t word;

    word = ((uint32_t)src[off]) |
           ((uint32_t)src[off + 1U] << 8) |
           ((uint32_t)src[off + 2U] << 16) |
           ((uint32_t)src[off + 3U] << 24);

    to = 0x00040000U;
    while(((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) && (to != 0U)) { to--; }
    FMC_CTL0 |= FMC_CTL0_PG;
    REG32(page_base + off) = word;
    to = 0x00040000U;
    while(((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) && (to != 0U)) { to--; }
    FMC_CTL0 &= ~FMC_CTL0_PG;
    if((FMC_STAT0 & (FMC_STAT0_PGERR | FMC_STAT0_WPERR)) != 0U) {
      FMC_CTL0 |= FMC_CTL0_LK;
      return ((FMC_STAT0 & FMC_STAT0_PGERR) != 0U) ? 6 : 7;
    }
    if((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) {
      FMC_CTL0 |= FMC_CTL0_LK;
      return 8;
    }
    FMC_STAT0 |= FMC_STAT0_ENDF;
  }

  FMC_CTL0 |= FMC_CTL0_LK;
  return 0;
}

/* End marker for gd32_flash_page_write_ram. ONLY the growth guard in
 * gd32_run_ram -- never the copy length: LTO may place it before the routine
 * (see GD32_FLASH_RAM_BOX below). */
__attribute__((section(".text.gd32_flash_ram"), used))
static void
gd32_flash_ram_end(void)
{
  __NOP();
}

/* Program-only RAM routine: write len bytes (word-aligned, len % 4 == 0) at
 * addr into ALREADY-ERASED cells. No erase, so it never disturbs sibling data
 * in the same physical 2 KiB page -- that is what lets the meta-store append a
 * record to the active (only-valid) slot without an erase window. Same NWLDE /
 * busy-guard discipline as the erase+program routine above; the erase and
 * blank-verify phases are simply omitted. */
__attribute__((section(".text.gd32_flash_ram"), used, noinline))
static int
gd32_flash_prog_ram(uint32_t addr, const uint8_t *src, uint32_t len)
{
  uint32_t off;
  uint32_t to;

  FMC_STAT0 |= (uint32_t)(FMC_STAT0_PGERR | FMC_STAT0_WPERR | FMC_STAT0_ENDF);

  if((FMC_CTL0 & FMC_CTL0_LK) != 0U) {
    FMC_KEY0 = GD32_FMC_UNLOCK0;
    FMC_KEY0 = GD32_FMC_UNLOCK1;
  }
  FMC_CTL0 &= ~FMC_CTL0_NWLDE;

  to = 0x04000000U;
  while(((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) && (to != 0U)) { to--; }
  if((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) {
    FMC_CTL0 |= FMC_CTL0_LK;
    return 4;
  }

  for(off = 0U; off < len; off += 4U) {
    uint32_t word;

    word = ((uint32_t)src[off]) |
           ((uint32_t)src[off + 1U] << 8) |
           ((uint32_t)src[off + 2U] << 16) |
           ((uint32_t)src[off + 3U] << 24);

    to = 0x00040000U;
    while(((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) && (to != 0U)) { to--; }
    FMC_CTL0 |= FMC_CTL0_PG;
    REG32(addr + off) = word;
    to = 0x00040000U;
    while(((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) && (to != 0U)) { to--; }
    FMC_CTL0 &= ~FMC_CTL0_PG;
    if((FMC_STAT0 & (FMC_STAT0_PGERR | FMC_STAT0_WPERR)) != 0U) {
      FMC_CTL0 |= FMC_CTL0_LK;
      return 6;
    }
    if((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) {
      FMC_CTL0 |= FMC_CTL0_LK;
      return 8;
    }
    FMC_STAT0 |= FMC_STAT0_ENDF;
  }

  FMC_CTL0 |= FMC_CTL0_LK;
  return 0;
}

/* End marker for gd32_flash_prog_ram; growth guard only, see
 * GD32_FLASH_RAM_BOX below. */
__attribute__((section(".text.gd32_flash_ram"), used))
static void
gd32_flash_prog_end(void)
{
  __NOP();
}

/* Erase-only RAM routine: erase the single page at page_base (page_size bytes).
 * Used by the meta-store to clear a standby slot before it becomes the write
 * target; never the active slot. */
__attribute__((section(".text.gd32_flash_ram"), used, noinline))
static int
gd32_flash_erase_ram(uint32_t page_base, const uint8_t *src, uint32_t page_size)
{
  uint32_t to;

  (void)src;

  FMC_STAT0 |= (uint32_t)(FMC_STAT0_PGERR | FMC_STAT0_WPERR | FMC_STAT0_ENDF);

  if((FMC_CTL0 & FMC_CTL0_LK) != 0U) {
    FMC_KEY0 = GD32_FMC_UNLOCK0;
    FMC_KEY0 = GD32_FMC_UNLOCK1;
  }
  FMC_CTL0 &= ~FMC_CTL0_NWLDE;

  to = 0x04000000U;
  while(((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) && (to != 0U)) { to--; }
  if((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) {
    FMC_CTL0 |= FMC_CTL0_LK;
    return 4;
  }

  FMC_CTL0 |= FMC_CTL0_PER;
  FMC_ADDR0 = page_base;
  FMC_CTL0 |= FMC_CTL0_START;
  __NOP();
  __NOP();
  to = 0x04000000U;
  while(((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) && (to != 0U)) { to--; }
  if((FMC_STAT0 & (FMC_STAT0_PGERR | FMC_STAT0_WPERR)) != 0U) {
    FMC_CTL0 &= ~FMC_CTL0_PER;
    FMC_CTL0 |= FMC_CTL0_LK;
    return 2;
  }
  if((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) {
    FMC_CTL0 &= ~FMC_CTL0_PER;
    FMC_CTL0 |= FMC_CTL0_LK;
    return 4;
  }
  FMC_CTL0 &= ~FMC_CTL0_PER;
  FMC_STAT0 |= FMC_STAT0_ENDF;
  FMC_CTL0 |= FMC_CTL0_LK;
  return 0;
}

/* End marker for gd32_flash_erase_ram; growth guard only, see
 * GD32_FLASH_RAM_BOX below. */
__attribute__((section(".text.gd32_flash_ram"), used))
static void
gd32_flash_erase_end(void)
{
  __NOP();
}

/* ----------------------------------------------------------------------------
 * OTP byte program (hardware monotonic counter primitive).
 *
 * GD32F50x FMC chapter: OTP cells accept exactly ONE program operation and
 * can never be erased, so the routine refuses any cell that does not read
 * 0xFF beforehand and verifies the written value by read-back. The program
 * sequence is the vendor fmc_otp_byte_program() flow (set PG, store the byte,
 * wait BUSY, clear PG) with the same unlock / NWLDE / busy-guard discipline
 * as the main-flash routines above, and it runs from the SRAM box like them.
 * No erase phase exists -- that is the entire point of OTP.
 * -------------------------------------------------------------------------- */
__attribute__((section(".text.gd32_flash_ram"), used, noinline))
static int
gd32_otp_prog_ram(uint32_t addr, const uint8_t *src, uint32_t len)
{
  uint32_t i;
  uint32_t to;

  FMC_STAT0 |= (uint32_t)(FMC_STAT0_PGERR | FMC_STAT0_WPERR | FMC_STAT0_ENDF);

  if((FMC_CTL0 & FMC_CTL0_LK) != 0U) {
    FMC_KEY0 = GD32_FMC_UNLOCK0;
    FMC_KEY0 = GD32_FMC_UNLOCK1;
  }
  FMC_CTL0 &= ~FMC_CTL0_NWLDE;

  to = 0x04000000U;
  while(((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) && (to != 0U)) { to--; }
  if((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) {
    FMC_CTL0 |= FMC_CTL0_LK;
    return 4;
  }

  for(i = 0U; i < len; i++) {
    uint8_t cur = REG8(addr + i);

    if(cur != 0xFFU) {
      /* One program per cell: a cell that is no longer 0xFF has already been
       * used (by us or by anyone) and must never be programmed again. */
      FMC_CTL0 |= FMC_CTL0_LK;
      return 9;
    }

    to = 0x00040000U;
    while(((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) && (to != 0U)) { to--; }
    FMC_CTL0 |= FMC_CTL0_PG;
    REG8(addr + i) = src[i];
    to = 0x00040000U;
    while(((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) && (to != 0U)) { to--; }
    FMC_CTL0 &= ~FMC_CTL0_PG;
    if((FMC_STAT0 & (FMC_STAT0_PGERR | FMC_STAT0_WPERR)) != 0U) {
      FMC_CTL0 |= FMC_CTL0_LK;
      return ((FMC_STAT0 & FMC_STAT0_PGERR) != 0U) ? 6 : 7;
    }
    if((FMC_STAT0 & FMC_STAT0_BUSY) != 0U) {
      FMC_CTL0 |= FMC_CTL0_LK;
      return 8;
    }
    FMC_STAT0 |= FMC_STAT0_ENDF;

    /* Read-back verify (the OTP cell is the only copy, so trust nothing). */
    if(REG8(addr + i) != src[i]) {
      FMC_CTL0 |= FMC_CTL0_LK;
      return 9;
    }
  }

  FMC_CTL0 |= FMC_CTL0_LK;
  return 0;
}

/* End marker for gd32_otp_prog_ram; growth guard only, see
 * GD32_FLASH_RAM_BOX below. */
__attribute__((section(".text.gd32_flash_ram"), used))
static void
gd32_otp_prog_end(void)
{
  __NOP();
}

/* ----------------------------------------------------------------------------
 * Fixed-size copy box for the RAM-resident flash routines.
 *
 * WHY NOT "end - start": all routines and end markers share ONE section
 * (.text.gd32_flash_ram), and armclang's LTO (the KeilC build) reorders
 * functions inside a section. It does -- in QBOOT_CHECK.map the markers
 * gd32_flash_prog_end / gd32_flash_erase_end land BEFORE their routines, so
 * "end - start" underflows to ~4 GiB and gd32_run_ram returned -5 before
 * touching the Flash. That is exactly the observed failure: the first ENTER
 * after a chip erase answered NAK(0xC1) reason=0x05 (PROG) on the KeilC
 * build. GCC keeps source order, so the GNU builds never showed it.
 *
 * The box copy is layout independent: the routine is entered at offset 0, so
 * whatever the linker placed after it is copied but never executed. The end
 * markers survive only as the growth guard inside gd32_run_ram.
 *
 * Invariants the box relies on (checked in gd32_run_ram where possible):
 *   - box > size of the largest routine (gd32_flash_page_write_ram, ~462 B);
 *   - [start, start + box) is always readable: the routines are linked at the
 *     start of the 512 KiB bank, far from its end.
 * If a routine ever outgrows the box, raise GD32_FLASH_RAM_BOX and
 * g_flash_ram_buf together.
 * ----------------------------------------------------------------------------
 */
#define GD32_FLASH_RAM_BOX  ((uint32_t)sizeof(g_flash_ram_buf))

/* Run a RAM-resident flash routine: copy a fixed-size box into
 * g_flash_ram_buf and branch into it with IRQs masked (M33 read-while-write
 * hazard). hi_sym is the routine's end marker, used ONLY to detect a routine
 * that outgrew the box; a marker placed before the routine (LTO reordering)
 * is not an error because the box copy does not depend on the order. */
static int
gd32_run_ram(uintptr_t lo_sym, uintptr_t hi_sym,
             uint32_t a0, const uint8_t *a1, uint32_t a2)
{
  gd32_flash_ram_fn_t fn;

  if((lo_sym == 0U) || (hi_sym == 0U)) {
    return -5;
  }
  /* Growth guard: a sane layout puts the marker just past the routine; if it
   * lands beyond the box the routine no longer fits and a truncated copy must
   * not run. hi_sym < lo_sym means reordering, not overflow -- tolerate it. */
  if((hi_sym > lo_sym) && ((hi_sym - lo_sym) > GD32_FLASH_RAM_BOX)) {
    return -5;
  }
  (void)memcpy((void *)g_flash_ram_buf, (const void *)lo_sym,
               GD32_FLASH_RAM_BOX);
  __DSB();
  __ISB();
  __disable_irq();
  /* Cortex-M33 is Thumb-only: an indirect BLX with bit0 clear attempts an ARM
   * state switch and takes an INVSTATE fault (the board then never answers the
   * ENTER ACK). Data pointers never carry the bit, so set it explicitly. */
  fn = (gd32_flash_ram_fn_t)((uintptr_t)g_flash_ram_buf | (uintptr_t)1U);
  /* Stop GCC from folding the indirect call into a direct BL to the data symbol
   * g_flash_ram_buf (which the linker cannot resolve as code). The asm barrier
   * makes fn opaque so the call must stay an indirect BLX into SRAM. */
  __asm__ volatile("" : "+r"(fn));
  {
    int rc = fn(a0, a1, a2);
    __enable_irq();
    return rc;
  }
}

int
quilla_qboot_hal_flash_write_page(uint32_t page_base, const uint8_t *src)
{
  int rc;

  if((page_base % QUILLA_QBOOT_PAGE_SIZE) != 0U) {
    return -1;
  }
  rc = gd32_run_ram(((uintptr_t)gd32_flash_page_write_ram) & ~(uintptr_t)1U,
                    ((uintptr_t)gd32_flash_ram_end) & ~(uintptr_t)1U,
                    page_base, src, QUILLA_QBOOT_PAGE_SIZE);
  if(rc == 0) {
    return 0;
  }
  /* Keep the documented HAL contract: -2 = erase-phase failure,
   * -3 = program-phase failure. */
  return (rc >= 6) ? -3 : -2;
}

int
quilla_qboot_hal_flash_program(uint32_t addr, const uint8_t *src, uint32_t len)
{
  int rc;

  if((addr % 4U) != 0U) {
    return -1;
  }
  if((len % 4U) != 0U) {
    return -1;
  }
  rc = gd32_run_ram(((uintptr_t)gd32_flash_prog_ram) & ~(uintptr_t)1U,
                    ((uintptr_t)gd32_flash_prog_end) & ~(uintptr_t)1U,
                    addr, src, len);
  return (rc == 0) ? 0 : -3;
}

int
quilla_qboot_hal_flash_erase_range(uint32_t base, uint32_t size)
{
  uint32_t pages = size / QUILLA_QBOOT_PAGE_SIZE;
  uint32_t i;

  if((base % QUILLA_QBOOT_PAGE_SIZE) != 0U) {
    return -1;
  }
  if(pages == 0U) {
    return -1;
  }
  for(i = 0U; i < pages; i++) {
    int rc = gd32_run_ram(((uintptr_t)gd32_flash_erase_ram) & ~(uintptr_t)1U,
                          ((uintptr_t)gd32_flash_erase_end) & ~(uintptr_t)1U,
                          base + i * QUILLA_QBOOT_PAGE_SIZE, NULL,
                          QUILLA_QBOOT_PAGE_SIZE);
    if(rc != 0) {
      return -2;
    }
  }
  return 0;
}

int
quilla_qboot_hal_otp_program_byte(uint32_t addr, uint8_t value)
{
#if QUILLA_QBOOT_SECPROG_AVAILABLE
  int rc;

  if((addr < QUILLA_QBOOT_OTP_BASE) ||
     (addr >= (QUILLA_QBOOT_OTP_BASE + QUILLA_QBOOT_OTP_SIZE))) {
    return -1;
  }
  rc = gd32_run_ram(((uintptr_t)gd32_otp_prog_ram) & ~(uintptr_t)1U,
                    ((uintptr_t)gd32_otp_prog_end) & ~(uintptr_t)1U,
                    addr, &value, 1U);
  /* 9 = cell-not-blank or read-back mismatch; surface as the HAL contract's
   * generic program failure so callers cannot confuse it with an erase code. */
  return (rc == 0) ? 0 : -3;
#else
  /* The F502VE always has OTP (SOC_OTP_AVAILABLE=1 in soc.h); this branch
   * exists so the file still compiles if that macro is ever cleared. */
  (void)addr;
  (void)value;
  return -1;
#endif
}

int
quilla_qboot_hal_otp_program_cell(uint32_t addr)
{
#if QUILLA_QBOOT_SECPROG_AVAILABLE
  /* GD32 OTP cells are byte-granular (SOC_OTP_CELL_SIZE=1): one cell is one
   * byte, burned by the existing one-time-program primitive. */
  return quilla_qboot_hal_otp_program_byte(addr, 0x00U);
#else
  (void)addr;
  return -1;
#endif
}

int
quilla_qboot_hal_otp_wrprot_status(void)
{
  /* True-OTP part: no WRP-protected counter region exists and the deferred
   * sync dance never runs, so the state is a constant. */
  return 0;
}

void
quilla_qboot_hal_otp_wrprot_set(int protect)
{
  /* True-OTP part: never called (quilla_qboot_otp_sync compiles to a no-op
   * without a write-protected NV counter region). A stub keeps the HAL contract complete. */
  (void)protect;
}

void
quilla_qboot_hal_reboot(void)
{
  /* Mask interrupts first: the flag page has just been rewritten and no
   * handler should run against a half-updated image. */
  __disable_irq();
  NVIC_SystemReset();
  while(1) {
  }
}

void
quilla_qboot_hal_jump_to_app(uint32_t app_start)
{
  uint32_t sp;
  void (*app_reset)(void);

  __disable_irq();

#ifdef QBOOT_BUILD
  /* Hand-over hygiene.
   *
   * (1) The QBOOT owns USART0 and its DMA RX channel; the staging buffers live
   *     in the same low RAM the APP is about to claim for its own .data/.bss.
   *     A channel left running would keep writing into what is now APP state,
   *     so park the request, the channel and their IRQs.
   * (2) Pending IRQ state must not survive the branch: a stale pending flag
   *     would fire into a half-initialised image.
   *
   * The USART itself stays enabled on purpose. The last thing the QIAP does
   * before the branch is push its final reply out of a polling writer, and
   * usart_disable() here would abort whatever is still shifting out, leaving
   * the host waiting for a frame that never arrives. Disabling reception is
   * enough: nothing notifies us after this point anyway. */
  NVIC_DisableIRQ(USART0_IRQn);
  NVIC_DisableIRQ(DMA0_Channel4_IRQn);
  usart_interrupt_disable(USART0, USART_INT_IDLE);
  usart_dma_receive_config(USART0, USART_DENR_DISABLE);
  dma_channel_disable(DMA0, DMA_CH4);
  NVIC_ClearPendingIRQ(USART0_IRQn);
  NVIC_ClearPendingIRQ(DMA0_Channel4_IRQn);
#endif

  /* The APP also sets VTOR in its own start-up; relocating here means the
   * APP vectors are live from the first instruction after the branch. */
  SCB->VTOR = app_start;
  sp = *(uint32_t *)app_start;
  app_reset = (void (*)(void))(*(uint32_t *)(app_start + 4U));

  __set_MSP(sp);

  /* Leave the core as the APP would find it after a real reset: PRIMASK
   * clear. Masking interrupts across the branch freezes SysTick - and with it
   * clock_time()/etimers and every peripheral IRQ - so the APP would finish
   * its start-up and then silently stop scheduling. The QBOOT's own interrupt
   * sources are off by now, so re-enabling here is safe. */
  __enable_irq();

  app_reset();
  while(1) {
  }
}

int_master_status_t
quilla_qboot_hal_int_disable(void)
{
  return int_master_read_and_disable();
}

void
quilla_qboot_hal_int_restore(int_master_status_t st)
{
  int_master_status_set(st);
}

void
quilla_qboot_hal_transmit(const uint8_t *data, uint16_t len)
{
#ifdef QBOOT_BUILD
  shellport_write_raw(data, len);
#else
  /* The APP image never runs the OTA state machine; keep the symbol defined
   * so the module links in both images. */
  (void)data;
  (void)len;
#endif
}

int
quilla_qboot_hal_app_image_valid(void)
{
  uint32_t vec[2];

  quilla_qboot_hal_flash_read(QUILLA_QBOOT_APP_START, (uint8_t *)vec, sizeof(vec));

  if(vec[0] < QUILLA_QBOOT_RAM_START ||
     vec[0] >= (QUILLA_QBOOT_RAM_START + QUILLA_QBOOT_RAM_SIZE)) {
    return 0;                            /* initial SP outside RAM */
  }
  if(vec[1] < QUILLA_QBOOT_APP_START ||
     vec[1] > QUILLA_QBOOT_APP_END ||
     (vec[1] & 1U) == 0U) {
    return 0;                            /* reset vector outside APP, or not Thumb */
  }
  return 1;
}

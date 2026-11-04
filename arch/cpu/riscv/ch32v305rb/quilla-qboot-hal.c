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
 * CH32V305VB (RISC-V) implementation of the boot hardware abstraction declared
 * in os/services/quilla-qboot/quilla-qboot-hal.h.
 *
 * Flash geometry: the CH32V305VB is a 128 KiB single-bank part. The WCH EVT
 * library programs 32-bit words and erases in 256-byte pages, so the page size
 * comes from the build (-DQUILLA_QBOOT_PAGE_SIZE=256) and not from this file,
 * keeping the image and the linker script in agreement. The 4 KiB META region
 * is a wear-levelled dual-slot log (quilla-qboot-meta.c): SLOT_A at 0x00006000
 * and SLOT_B at 0x00006800; records are appended with
 * quilla_qboot_hal_flash_program (no erase) into the active slot, and only the
 * standby slot is ever erased.
 *
 * KNOWN LIMITATION (needs on-hardware validation, unlike the GD32F502VE port
 * which runs its writer from SRAM): these routines execute from Flash with
 * IRQs masked for the duration of each page operation. The QBOOT only ever
 * erases/programs the META and APP regions (never the page it runs from), and
 * the watchdog is APP-side only, so masking is safe here. If a read-while-write
 * hazard shows up on silicon - the class of bug the GD32 port hit - migrate the
 * erase/program cores to SRAM-resident routines (the WCH library exposes
 * FLASH_ROM_ERASE / FLASH_ROM_WRITE for exactly that).
 * ----------------------------------------------------------------------------
 */

#include "../../../../os/services/quilla-qboot/quilla-qboot-hal.h"

#include <stdint.h>
#include <string.h>

#include "ch32v30x.h"
#include "sys/int-master.h"

#ifdef QBOOT_BUILD
#include "shellport.h"
#endif

/* Bounded busy-wait for the flash controller (page erase / word program).
 * Far larger than any single operation, small enough to hang the boot rather
 * than spin forever if the controller wedges. */
#define HAL_FLASH_BUSY_SPIN  0x00400000U

/*---------------------------------------------------------------------------*/
static uint32_t
hal_wait_not_busy(void)
{
  uint32_t to = HAL_FLASH_BUSY_SPIN;
  while((FLASH_GetFlagStatus(FLASH_FLAG_BSY) != RESET) && (to != 0U)) {
    to--;
  }
  return to;
}
/*---------------------------------------------------------------------------*/
void
quilla_qboot_hal_flash_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
  const volatile uint8_t *src = (const volatile uint8_t *)(uintptr_t)addr;
  uint32_t i;

  for(i = 0U; i < len; i++) {
    dst[i] = src[i];
  }
}
/*---------------------------------------------------------------------------*/
/* Erase exactly one page (QUILLA_QBOOT_PAGE_SIZE bytes) at page_base. The FPEC
 * must already be unlocked. Returns 0 on success, negative on failure. */
static int
hal_erase_page(uint32_t page_base)
{
  if(hal_wait_not_busy() == 0U) {
    return -2;
  }
  FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR | FLASH_FLAG_OPTERR);
  if(FLASH_ErasePage(page_base) != FLASH_COMPLETE) {
    return -2;
  }
  if(hal_wait_not_busy() == 0U) {
    return -2;
  }
  if(FLASH_GetFlagStatus(FLASH_FLAG_WRPRTERR) != RESET) {
    return -2;
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
/* Program len bytes (word-aligned, len % 4 == 0) at addr into ALREADY-ERASED
 * cells. The FPEC must already be unlocked. Returns 0 on success, negative on
 * failure. */
static int
hal_program_words(uint32_t addr, const uint8_t *src, uint32_t len)
{
  uint32_t off;

  for(off = 0U; off < len; off += 4U) {
    uint32_t word = ((uint32_t)src[off]) |
                    ((uint32_t)src[off + 1U] << 8) |
                    ((uint32_t)src[off + 2U] << 16) |
                    ((uint32_t)src[off + 3U] << 24);

    if(hal_wait_not_busy() == 0U) {
      return -3;
    }
    if(FLASH_ProgramWord(addr + off, word) != FLASH_COMPLETE) {
      return -3;
    }
    if(hal_wait_not_busy() == 0U) {
      return -3;
    }
    if(FLASH_GetFlagStatus(FLASH_FLAG_WRPRTERR) != RESET) {
      return -3;
    }
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
int
quilla_qboot_hal_flash_write_page(uint32_t page_base, const uint8_t *src)
{
  int rc;
  uint32_t off;
  int_master_status_t st = quilla_qboot_hal_int_disable();

  if((page_base % QUILLA_QBOOT_PAGE_SIZE) != 0U) {
    quilla_qboot_hal_int_restore(st);
    return -1;
  }

  FLASH_Unlock();
  rc = hal_erase_page(page_base);
  if(rc == 0) {
    /* Verify the erase genuinely blanked the whole page before programming. */
    for(off = 0U; off < QUILLA_QBOOT_PAGE_SIZE; off += 4U) {
      if(*((const volatile uint32_t *)(uintptr_t)(page_base + off)) != 0xFFFFFFFFU) {
        rc = -2;
        break;
      }
    }
  }
  if(rc == 0) {
    rc = hal_program_words(page_base, src, QUILLA_QBOOT_PAGE_SIZE);
  }
  FLASH_Lock();
  quilla_qboot_hal_int_restore(st);

  return (rc == 0) ? 0 : ((rc >= -3) ? rc : -2);
}
/*---------------------------------------------------------------------------*/
int
quilla_qboot_hal_flash_program(uint32_t addr, const uint8_t *src, uint32_t len)
{
  int rc;
  int_master_status_t st = quilla_qboot_hal_int_disable();

  if((addr % 4U) != 0U) {
    quilla_qboot_hal_int_restore(st);
    return -1;
  }
  if((len % 4U) != 0U) {
    quilla_qboot_hal_int_restore(st);
    return -1;
  }

  FLASH_Unlock();
  rc = hal_program_words(addr, src, len);
  FLASH_Lock();
  quilla_qboot_hal_int_restore(st);

  return (rc == 0) ? 0 : -3;
}
/*---------------------------------------------------------------------------*/
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
    int_master_status_t st = quilla_qboot_hal_int_disable();
    int rc;
    FLASH_Unlock();
    rc = hal_erase_page(base + i * QUILLA_QBOOT_PAGE_SIZE);
    FLASH_Lock();
    quilla_qboot_hal_int_restore(st);
    if(rc != 0) {
      return -2;
    }
  }
  return 0;
}
/*---------------------------------------------------------------------------*/
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
/*---------------------------------------------------------------------------*/
void
quilla_qboot_hal_jump_to_app(uint32_t app_start)
{
  void (*app_entry)(void);

  __disable_irq();

#ifdef QBOOT_BUILD
  /* Hand-over hygiene. (1) The QBOOT owns USART2 and its DMA RX channel; the
   * staging buffers live in the same low RAM the APP is about to claim for its
   * own .data/.bss, so park the request, the channel and their IRQs. (2) No
   * stale pending IRQ may survive the branch.
   * The USART itself stays enabled on purpose: the last thing the QIAP does is
   * push its final reply out of a polling writer, and disabling the USART here
   * would abort whatever is still shifting out. */
  NVIC_DisableIRQ(USART2_IRQn);
  NVIC_DisableIRQ(DMA1_Channel6_IRQn);
  USART_ITConfig(USART2, USART_IT_IDLE, DISABLE);
  USART_DMACmd(USART2, USART_DMAReq_Rx, DISABLE);
  DMA_Cmd(DMA1_Channel6, DISABLE);
  NVIC_ClearPendingIRQ(USART2_IRQn);
  NVIC_ClearPendingIRQ(DMA1_Channel6_IRQn);
#endif

  /* RISC-V has no VTOR and no "initial SP in the vector table": the APP's
   * startup (handle_reset in startup_ch32v30x_D8C.S) loads its own sp from
   * _eusrstack, points mtvec at its own _vector_base and sets mstatus, then
   * mret's to main. So the hand-over is a plain branch to the APP entry word,
   * which is _start ("j handle_reset"). We deliberately leave the master
   * interrupt DISABLED across the branch - exactly the state a real reset
   * leaves the core in - and let handle_reset enable it after mtvec is live.
   * (This differs from the Cortex-M ports, which must re-enable before the
   * branch because their SysTick has no equivalent self-arming startup.) */
  app_entry = (void (*)(void))(uintptr_t)app_start;
  /* Keep the branch indirect and opaque so the linker cannot fold it. */
  __asm__ volatile("" : : "r"(app_entry) : "memory");
  app_entry();
  while(1) {
  }
}
/*---------------------------------------------------------------------------*/
int_master_status_t
quilla_qboot_hal_int_disable(void)
{
  return int_master_read_and_disable();
}
/*---------------------------------------------------------------------------*/
void
quilla_qboot_hal_int_restore(int_master_status_t st)
{
  int_master_status_set(st);
}
/*---------------------------------------------------------------------------*/
void
quilla_qboot_hal_transmit(const uint8_t *data, uint16_t len)
{
#ifdef QBOOT_BUILD
  shellport_write_raw(data, len);
#else
  (void)data;
  (void)len;
#endif
}
/*---------------------------------------------------------------------------*/
int
quilla_qboot_hal_app_image_valid(void)
{
  uint32_t w0;

  quilla_qboot_hal_flash_read(QUILLA_QBOOT_APP_START, (uint8_t *)&w0, sizeof(w0));
  /* RISC-V has no Cortex-M vector table (no initial-SP-in-vector, no Thumb
   * bit). The APP entry word at APP_START is _start, a "j handle_reset" (JAL,
   * opcode 0x6F). An erased/half-written region reads 0xFFFFFFFF (opcode 0x7F)
   * and fails. Mechanical presence test only. */
  if((w0 & 0x7FUL) != 0x6FUL) {
    return 0;
  }
  return 1;
}

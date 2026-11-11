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
 * PY32F040xB implementation of the boot hardware abstraction declared in
 * os/services/quilla-qboot/quilla-qboot-hal.h.
 *
 * This is the only place in the boot path that talks to the HAL. It is
 * deliberately free of protocol and policy logic so the OS-side modules stay
 * portable and unit-testable.
 * ----------------------------------------------------------------------------
 */

#include "../../../../os/services/quilla-qboot/quilla-qboot-hal.h"

#include <stdint.h>

#include "py32f0xx_hal.h"
#include "py32f040_hal_flash.h"
#include "py32f040_hal_cortex.h"
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

int
quilla_qboot_hal_flash_write_page(uint32_t page_base, const uint8_t *src)
{
  FLASH_EraseInitTypeDef erase;
  uint32_t page_error;

  if(HAL_FLASH_Unlock() != HAL_OK) {
    return -1;
  }

  /* Both fields are mandatory: the erase loop is bounded by NbPages, so a
   * zero NbPages would silently erase nothing. */
  erase.TypeErase = FLASH_TYPEERASE_PAGEERASE;
  erase.PageAddress = page_base;
  erase.NbPages = 1U;

  if(HAL_FLASH_Erase(&erase, &page_error) != HAL_OK) {
    (void)HAL_FLASH_Lock();
    return -2;
  }
  if(HAL_FLASH_PageProgram(page_base, (uint32_t *)(uintptr_t)src) != HAL_OK) {
    (void)HAL_FLASH_Lock();
    return -3;
  }
  (void)HAL_FLASH_Lock();
  return 0;
}

/* Program already-erased Flash WITHOUT erasing. On PY32 the meta record is
 * exactly one erase page (QUILLA_QBOOT_PAGE_SIZE == 256), so this maps directly
 * onto HAL_FLASH_PageProgram of a single page. It never touches the erase
 * controller, so sibling records in other pages of the slot are undisturbed.
 *
 * Readback verification: HAL_FLASH_PageProgram does NOT report a failure when a
 * target bit was not erased (it simply cannot be set, leaving a 0), so a program
 * onto a dirty page passes silently and only surfaces as a corrupt record at the
 * next boot. After programming we re-read every byte and compare against the
 * only valid outcome (old & src); a mismatch means the page was not erased and
 * the commit would be silently broken -- fail loud here so the QIAP SIGN path
 * can NAK instead of handing the host a fictitious success. */
int
quilla_qboot_hal_flash_program(uint32_t addr, const uint8_t *src, uint32_t len)
{
  uint32_t done = 0U;
  uint8_t old[QUILLA_QBOOT_PAGE_SIZE];

  if((addr % QUILLA_QBOOT_PAGE_SIZE) != 0U) {
    return -1;
  }
  if((len % QUILLA_QBOOT_PAGE_SIZE) != 0U) {
    return -1;
  }
  if(HAL_FLASH_Unlock() != HAL_OK) {
    return -1;
  }
  while(done < len) {
    uint32_t page = addr + done;
    uint32_t i;

    quilla_qboot_hal_flash_read(page, old, QUILLA_QBOOT_PAGE_SIZE);
    if(HAL_FLASH_PageProgram(page,
                             (uint32_t *)(uintptr_t)(src + done)) != HAL_OK) {
      (void)HAL_FLASH_Lock();
      return -3;
    }
    for(i = 0U; i < QUILLA_QBOOT_PAGE_SIZE; i++) {
      uint8_t rb = 0U;
      quilla_qboot_hal_flash_read(page + i, &rb, 1U);
      if(rb != (uint8_t)(old[i] & src[done + i])) {
        (void)HAL_FLASH_Lock();
        return -3;
      }
    }
    done += QUILLA_QBOOT_PAGE_SIZE;
  }
  (void)HAL_FLASH_Lock();
  return 0;
}

/* Erase every page covering [base, base+size). */
int
quilla_qboot_hal_flash_erase_range(uint32_t base, uint32_t size)
{
  FLASH_EraseInitTypeDef erase;
  uint32_t page_error;
  uint32_t pages;

  if((base % QUILLA_QBOOT_PAGE_SIZE) != 0U) {
    return -1;
  }
  if((size % QUILLA_QBOOT_PAGE_SIZE) != 0U) {
    return -1;
  }
  pages = size / QUILLA_QBOOT_PAGE_SIZE;
  if(pages == 0U) {
    return -1;
  }
  if(HAL_FLASH_Unlock() != HAL_OK) {
    return -1;
  }
  erase.TypeErase = FLASH_TYPEERASE_PAGEERASE;
  erase.PageAddress = base;
  erase.NbPages = pages;
  if(HAL_FLASH_Erase(&erase, &page_error) != HAL_OK) {
    (void)HAL_FLASH_Lock();
    return -2;
  }
  (void)HAL_FLASH_Lock();
  return 0;
}

int
quilla_qboot_hal_otp_program_byte(uint32_t addr, uint8_t value)
{
  /* PY32F040xB has no OTP area (SOC_OTP_AVAILABLE=0 in soc.h): the reference
   * manual documents only the Information block, none of which is a
   * one-time-programmable user area. The hardware monotonic counter uses the
   * WRP-protected write-protected NV counter region instead (see otp_program_cell below);
   * this OTP-byte primitive remains a stub. */
  (void)addr;
  (void)value;
  return -1;
}

#if QUILLA_QBOOT_WPP
/* One zeroed programming cell. PY32F040 flash programs whole 256-byte pages
 * (SOC_OTP_CELL_SIZE), so burning one counter cell means writing one page of
 * zeros onto a still-erased page. Kept in rodata: no stack pressure inside
 * the boot path, and the cost only exists when QUILLA_QBOOT_OTP_COUNTER is
 * enabled. */
static const uint8_t otp_zero_cell[QUILLA_QBOOT_OTP_CELL_SIZE]
    QUILLA_QBOOT_ALIGNED = {0U};

/* Which FLASH->WRPR bit guards the counter region: PY32 WRP sectors are
 * 8 KB and the region is exactly one sector at the top of flash. */
static uint32_t
otp_wrp_bit(void)
{
  return (SOC_OTP_WRPROT_BASE - 0x08000000UL) / SOC_OTP_WRPROT_SIZE;
}
#endif

int
quilla_qboot_hal_otp_program_cell(uint32_t addr)
{
#if QUILLA_QBOOT_WPP
  uint32_t i;
  uint8_t b;

  if((addr < SOC_OTP_WRPROT_BASE) ||
     (addr >= (SOC_OTP_WRPROT_BASE + SOC_OTP_WRPROT_SIZE)) ||
     ((addr % (uint32_t)SOC_OTP_CELL_SIZE) != 0U)) {
    return -1;
  }
  /* Programming an already-programmed page silently leaves it unchanged
   * (flash can only clear bits), so refuse any cell that is not virgin. */
  for(i = 0U; i < (uint32_t)SOC_OTP_CELL_SIZE; i++) {
    quilla_qboot_hal_flash_read(addr + i, &b, 1U);
    if(b != 0xFFU) {
      return -3;
    }
  }
  if(HAL_FLASH_Unlock() != HAL_OK) {
    return -1;
  }
  if(HAL_FLASH_PageProgram(addr, (uint32_t *)(uintptr_t)otp_zero_cell)
     != HAL_OK) {
    (void)HAL_FLASH_Lock();
    return -3;
  }
  (void)HAL_FLASH_Lock();
  /* Read-back confirmation: a burned cell MUST read all zero. */
  for(i = 0U; i < (uint32_t)SOC_OTP_CELL_SIZE; i++) {
    quilla_qboot_hal_flash_read(addr + i, &b, 1U);
    if(b != 0x00U) {
      return -3;
    }
  }
  return 0;
#else
  (void)addr;
  return -1;
#endif
}

int
quilla_qboot_hal_otp_wrprot_status(void)
{
#if QUILLA_QBOOT_WPP
  /* FLASH->WRPR mirrors the loaded option bytes after reset; the WRP bits
   * are active-low: bit cleared = sector write-protected. */
  uint32_t bit = otp_wrp_bit();

  return (((FLASH->WRPR >> bit) & 1UL) == 0UL) ? 1 : 0;
#else
  return 0;
#endif
}

void
quilla_qboot_hal_otp_wrprot_set(int protect)
{
#if QUILLA_QBOOT_WPP
  uint32_t wrpr;
  uint32_t bit = otp_wrp_bit();
  uint32_t guard;
  int_master_status_t st = quilla_qboot_hal_int_disable();

  if(HAL_FLASH_OB_Unlock() != HAL_OK) {
    quilla_qboot_hal_int_restore(st);
    return;                     /* caller retries on the next boot */
  }

  /* Rewrite only the WRP field; FLASH->OPTR / FLASH->SDKR still hold the
   * currently loaded values, and the option-byte program copies them all
   * back unchanged. */
  wrpr = FLASH->WRPR;
  if(protect != 0) {
    wrpr &= ~(1UL << bit);      /* active-low: clear = protected */
  } else {
    wrpr |= (1UL << bit);
  }
  FLASH->WRPR = wrpr;

  FLASH->CR |= FLASH_CR_OPTSTRT;
  /* Program trigger, same mechanism as the vendor option-byte routines. */
  (*(volatile uint32_t *)(uintptr_t)0x40022080UL) = 0xFFU;
  for(guard = 0U; guard < 8000000U; guard++) {
    if((FLASH->SR & FLASH_SR_BSY) == 0U) {
      break;
    }
  }
  if((FLASH->SR & FLASH_SR_BSY) != 0U) {
    /* Program never completed: do NOT launch a reload -- returning leaves
     * the caller in a state that retries on the next boot instead of
     * resetting in a loop. */
    (void)HAL_FLASH_OB_Lock();
    quilla_qboot_hal_int_restore(st);
    return;
  }
  (void)HAL_FLASH_OB_Lock();

  /* Reload the option bytes: OBL_LAUNCH resets the chip and this call never
   * returns (the vendor routine documents "Option byte launch generates
   * Option byte reset"). */
  (void)HAL_FLASH_OB_Launch();
  quilla_qboot_hal_reboot();    /* belt and braces; never reached */
  while(1) {
  }
#else
  (void)protect;
#endif
}

void
quilla_qboot_hal_reboot(void)
{
  /* Mask interrupts first: the flag page has just been rewritten and no
   * handler should run against a half-updated image. */
  __disable_irq();
  HAL_NVIC_SystemReset();
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
   * (1) The QIAP owns the console USART + its RX DMA channel, and its RX
   *     buffers live in the same low RAM the APP is about to claim for its
   *     own .data/.bss. A channel left running would keep writing into what
   *     is now APP state, so park both the DMA request and the channel and
   *     mask their IRQs. Reference wiring USART2 / DMA1_Channel6.
   * (2) Pending IRQ state must not survive the branch: the APP re-installs
   *     its own handlers after start-up, and a stale pending flag would fire
   *     into a half-initialised image. */
  NVIC_DisableIRQ(USART2_IRQn);
  NVIC_DisableIRQ(DMA1_Channel4_5_6_7_IRQn);
  NVIC_ClearPendingIRQ(USART2_IRQn);
  NVIC_ClearPendingIRQ(DMA1_Channel4_5_6_7_IRQn);
  USART2->CR3 &= ~USART_CR3_DMAR;          /* stop USART2 RX DMA requests */
  DMA1_Channel6->CCR &= ~DMA_CCR_EN;       /* and the channel itself       */
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
   * its start-up and then silently stop scheduling. The QIAP's own interrupt
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

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

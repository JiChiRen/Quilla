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

#include "contiki.h"
#include "lpm.h"
#include "soc.h"
#include "ch32v30x.h"

#include <stdint.h>
/*----------------------------------------------------------------------------*/
/* Log configuration */
#include "sys/log.h"
#define LOG_MODULE "QuillaCh32v305 SoC"
#define LOG_LEVEL LOG_LEVEL_NONE
/*----------------------------------------------------------------------------*/
uint8_t
soc_get_rev(void)
{
  /* No die-revision register is read on this port; the value stays 0 rather
   * than reporting something the code did not actually measure. */
  return 0U;
}
/*----------------------------------------------------------------------------*/
uint32_t
soc_get_sram_size(void)
{
  /* CH32V305RB/VB: 32 KiB SRAM at 0x20000000 (see the *_app.ld / *_qboot.ld). */
  return 0x8000U;
}
/*----------------------------------------------------------------------------*/
uint32_t
soc_get_features(void)
{
  return 0U;
}
/*----------------------------------------------------------------------------*/
void
soc_print_info(void)
{
  (void)soc_get_rev();
  (void)soc_get_features();
}
/*----------------------------------------------------------------------------*/
void
soc_init(void)
{
  /* SystemInit() (called from handle_reset in startup_ch32v30x_D8C.S) has
   * already brought HSE(8 MHz) through the PLL to 96 MHz, so only the OS-side
   * time bases are left. */
  clock_init();
  rtimer_init();
  lpm_init();
}
/*----------------------------------------------------------------------------*/
/** @} */

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

#include "contiki.h"
#include "sys/int-master.h"

/*
 * This is the RISC-V implementation of int-master.h. There is no PRIMASK on a
 * RISC-V core: the master interrupt gate is mstatus.MIE (bit 3), reached
 * through the WCH core_riscv.h intrinsics __get_MSTATUS / __enable_irq /
 * __disable_irq. Include cpu.h explicitly instead of hoping some other header
 * drags in the device header (core_riscv.h); the Arm port learned that lesson
 * the hard way.
 */
#include "cpu.h"

#include <stdbool.h>
/*---------------------------------------------------------------------------*/
/* mstatus.MIE - the single global interrupt-enable bit this port manages.
 * The other bits __disable_irq() clears (MPIE) are exception-entry state and
 * are deliberately not saved/restored here. */
#define RISCV_MSTATUS_MIE  0x00000008UL
/*---------------------------------------------------------------------------*/
void
int_master_enable(void)
{
  __enable_irq();
}
/*---------------------------------------------------------------------------*/
int_master_status_t
int_master_read_and_disable(void)
{
  int_master_status_t mie = (int_master_status_t)(__get_MSTATUS() & RISCV_MSTATUS_MIE);

  __disable_irq();

  return mie;
}
/*---------------------------------------------------------------------------*/
void
int_master_status_set(int_master_status_t status)
{
  if((status & RISCV_MSTATUS_MIE) != 0U) {
    __enable_irq();
  } else {
    __disable_irq();
  }
}
/*---------------------------------------------------------------------------*/
bool
int_master_is_enabled(void)
{
  return (__get_MSTATUS() & RISCV_MSTATUS_MIE) != 0U;
}
/*---------------------------------------------------------------------------*/
/**
 * @}
 * @}
 */

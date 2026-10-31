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
 *
 * This file is part of the Quilla operating system.
 */

/**
 * \addtogroup arm
 * @{
 *
 * \defgroup arm-wdt PY32F040xB independent watchdog driver
 *
 * Adapter from the Contiki os/dev/watchdog.h interface onto dev/qwdt.c,
 * which owns the peripheral and decides the timeout. Nothing about the
 * timeout is decided here, deliberately: the upstream header is left
 * untouched and every policy question is answered in one place.
 *
 * \file
 *     Watchdog adapter for the PY32F040xB.
 *
 * \author
 *     Quilla kernel safety layer.
 */

#include "contiki.h"
#include "dev/watchdog.h"
#include "dev/qwdt.h"

/*---------------------------------------------------------------------------*/
void
watchdog_init(void)
{
  (void)qwdt_init();
}
/*---------------------------------------------------------------------------*/
void
watchdog_start(void)
{
  (void)qwdt_start();
}
/*---------------------------------------------------------------------------*/
/** \brief Reload the IWDG counter (feed the dog).
 *
 * Must not be called from an interrupt context (per os/dev/watchdog.h).
 *
 * Feeding here is unconditional, because this is the legacy Contiki entry
 * point and it is called from inside long loops all over the tree. The
 * safety-relevant decision lives one level up: the idle loop reaches this
 * point only after the flow, guard, timer and stack checks have passed, and
 * it is that ordering which must not be weakened.
 */
void
watchdog_periodic(void)
{
  (void)qwdt_refresh();
}
/*---------------------------------------------------------------------------*/
/** \brief Stop the IWDG.
 *
 * Not supported: once the PY32F040xB IWDG is started it can only be disabled by
 * a system reset (hardware limitation, documented in the HAL driver). Kept for
 * API compatibility with os/dev/watchdog.h.
 */
void
watchdog_stop(void)
{
  /* Intentionally a no-op. */
}
/*---------------------------------------------------------------------------*/
void
watchdog_reboot(void)
{
  qwdt_reboot();
}
/*---------------------------------------------------------------------------*/

/**
 * @}
 * @}
 */

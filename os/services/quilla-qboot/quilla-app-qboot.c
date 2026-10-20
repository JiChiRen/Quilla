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

/**
 * \addtogroup quilla-app-qboot
 * @{
 */

#include "quilla-app-qboot.h"

#include <stdint.h>

#include "../shell/quilla-shell.h"
#include "quilla-qboot-flag.h"
#include "quilla-qboot-hal.h"

static void
swqboot_handler(const char *args)
{
  (void)args;

  shell_output("swqboot: set flag=QBOOT, reset to QBOOT shell...\r\n");

  if(quilla_qboot_flag_set(QUILLA_QBOOT_FLAG_QBOOT) != 0) {
    shell_output("swqboot: FAILED to set flag\r\n");
    return;
  }

  /* Reset so the QBOOT boots next. quilla_qboot_hal_reboot() masks interrupts
   * first and does not return. */
  quilla_qboot_hal_reboot();
  while(1) {
  }
}

static const shell_cmd_t swqboot_cmd = {
  "swqboot", swqboot_handler, "stay in QBOOT shell (no upgrade until 'qiap')"
};

void
quilla_app_qboot_init(void)
{
  shell_register_cmd(&swqboot_cmd);
}

/** @} */

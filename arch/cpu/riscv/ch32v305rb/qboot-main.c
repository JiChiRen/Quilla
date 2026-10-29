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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
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
 * QBOOT (bootloader) entry point for the CH32V305VB (RISC-V) port. Compiled
 * into the QBOOT image only. Boot policy and flow mirror the GD32F502VE port;
 * the only MCU-specific piece is qboot_app_image_valid() below (RISC-V has no
 * Cortex-M vector table).
 *
 * The serial link (USART2 on PA2/PA3, DMA assisted RX) is shared with the
 * shell: while the QIAP state machine is active, received bytes are routed to
 * it instead of the shell line buffer so Flash erase/program never runs inside
 * an interrupt. The upgrade protocol lives in os/services/quilla-qboot/ and
 * every machine operation goes through quilla-qboot-hal.h.
 * ----------------------------------------------------------------------------
 */

#include <stdint.h>

#include "sys/clock.h"

#include "../../../../os/services/shell/quilla-shell.h"
#include "../../../../os/services/quilla-qboot/quilla-qboot-hal.h"
#include "../../../../os/services/quilla-qboot/quilla-qboot-flag.h"
#include "../../../../os/services/quilla-qboot/quilla-qiap.h"
#include "../../../../os/services/quilla-qboot/quilla-qboot-secure.h"
#include "contiki.h"
#include "sys/kern-stack.h"
#include "sys/kern-flow.h"
#include "shellport.h"

/* ---- ring buffer bridging DMA ISR -> main-loop QIAP parser ---------------- */
#define QBOOT_RX_RING_SZ  256U
static uint8_t qboot_rx_ring[QBOOT_RX_RING_SZ];
static volatile uint16_t qboot_rx_head;   /* written by ISR (sink) */
static volatile uint16_t qboot_rx_tail;   /* written by main loop  */

#ifdef QBOOT_BUILD
static void
qboot_rx_sink(uint8_t b)
{
  uint16_t next = (uint16_t)((qboot_rx_head + 1U) & (QBOOT_RX_RING_SZ - 1U));
  if(next != qboot_rx_tail) {            /* drop on overflow */
    qboot_rx_ring[qboot_rx_head] = b;
    qboot_rx_head = next;
  }
}
#endif

static void qboot_shell_run(void);

static int qboot_shell_flow = -1;

static int
qboot_stack_ok(void)
{
  static uint8_t overflow_reported;
  static uint8_t unarmed_reported;
  kern_status_t rc = kern_stack_check();

  if(rc == KERN_ERR_STACK) {
    if(overflow_reported == 0U) {
      overflow_reported = 1U;
      shell_puts("\r\n[BOOT] stack overflow: canary lost -");
      shell_puts(" APP handover REFUSED, shell kept\r\n");
    }
    return 0;
  }
  if(rc == KERN_ERR_NOT_READY && unarmed_reported == 0U) {
    unarmed_reported = 1U;
    shell_puts("\r\n[BOOT] stack monitor not armed\r\n");
  }
  return 1;
}

static int
qboot_rx_pop(uint8_t *out)
{
  int r = -1;
  int_master_status_t st = quilla_qboot_hal_int_disable();

  if(qboot_rx_tail != qboot_rx_head) {
    *out = qboot_rx_ring[qboot_rx_tail];
    qboot_rx_tail = (uint16_t)((qboot_rx_tail + 1U) & (QBOOT_RX_RING_SZ - 1U));
    r = 0;
  }
  quilla_qboot_hal_int_restore(st);
  return r;
}

static void
qboot_qiap_run(void)
{
  uint8_t b;

  shellport_set_raw_sink(qboot_rx_sink);
  quilla_qiap_init();
  shell_puts("\r\n[QIAP] enter upgrade mode. Send ENTER frame.\r\n");

  while(1) {
    /* The CIRCULAR RX DMA runs forever and is drained only by
     * shellport_rx_harvest(). While we own the CPU (no process_run() here) the
     * shell process is not scheduled, so harvest the DMA ring ourselves, then
     * drain the whole ring into the parser each pass so it stays bounded. */
    shellport_rx_harvest();

    if(quilla_qiap_is_done()) {
#if KERN_CONF_STACK_SHELL
      kern_stack_report("qiap-done");
#endif
      shell_puts("\r\n[QIAP] success. Rebooting...\r\n");
      quilla_qboot_hal_reboot();
    }
    while(qboot_rx_pop(&b) == 0) {
      quilla_qiap_feed(b);
    }
    if(qboot_stack_ok() == 0) {
      shellport_set_raw_sink(NULL);
      qboot_shell_run();
    }
  }
}

static void
qboot_qiap_cmd_handler(const char *args)
{
  (void)args;
  shell_puts("qiap: switching to upgrade mode\r\n");
  qboot_qiap_run();
}

static const shell_cmd_t qboot_qiap_cmd = {
  "qiap", qboot_qiap_cmd_handler, "enter QIAP serial upgrade"
};

static void
qboot_swapp_cmd_handler(const char *args)
{
  uint32_t flag;

  (void)args;

  flag = quilla_qboot_flag_get();
  if(flag == QUILLA_QBOOT_FLAG_QIAP) {
    shell_puts("swapp: upgrade in progress, switch refused\r\n");
    return;
  }
  shell_puts("swapp: set flag=APP1, reset to APP...\r\n");
  if(quilla_qboot_flag_set(QUILLA_QBOOT_FLAG_APP1) != 0) {
    shell_puts("swapp: FAILED to set flag\r\n");
    return;
  }
  quilla_qboot_hal_reboot();
  while(1) {
  }
}

static const shell_cmd_t qboot_swapp_cmd = {
  "swapp", qboot_swapp_cmd_handler, "set APP1 flag and reset into APP"
};

static void
qboot_pinfo_cmd_handler(const char *args)
{
  uint32_t app_size =
    (uint32_t)(QUILLA_QBOOT_APP_END - QUILLA_QBOOT_APP_START + 1U);
  uint32_t fw_ver = quilla_qboot_secure_app_version();
  (void)args;
  shell_output("[PART] APP_START=0x%08X APP_END=0x%08X APP_SIZE=0x%08X\r\n",
               (unsigned)QUILLA_QBOOT_APP_START,
               (unsigned)QUILLA_QBOOT_APP_END,
               (unsigned)app_size);
  shell_output("[PART] FW_SECVER=0x%08X (%s)\r\n",
               (unsigned)fw_ver,
               (fw_ver == 0U) ? "missing/corrupt" : "present");
}

static const shell_cmd_t qboot_pinfo_cmd = {
  "pinfo", qboot_pinfo_cmd_handler, "show QBOOT partition layout (APP_START/END/SIZE)"
};

/* ----------------------------------------------------------------------------
 * APP sanity check before the handover.
 *
 * RISC-V has no Cortex-M vector table (no initial-SP-in-vector, no Thumb bit).
 * The APP entry word at APP_START is _start, a "j handle_reset" (JAL, opcode
 * 0x6F). An erased/half-written region reads 0xFFFFFFFF (opcode 0x7F) and
 * fails. This is a mechanical presence test only; when a key is provisioned the
 * signature gate has the final say.
 * -------------------------------------------------------------------------- */
static int
qboot_app_image_valid(void)
{
  uint32_t w0;

  quilla_qboot_hal_flash_read(QUILLA_QBOOT_APP_START, (uint8_t *)&w0, sizeof(w0));
  if((w0 & 0x7FUL) != 0x6FUL) {   /* JAL opcode? blank/foreign -> not runnable */
    return 0;
  }
  return 1;
}

static int
qboot_secure_boot_ok(void)
{
  int rc = quilla_qboot_secure_verify_app();

  if(rc == QUILLA_SB_OK || rc == QUILLA_SB_OK_SEALED ||
     rc == QUILLA_SB_OK_INTEGRITY_ONLY || rc == QUILLA_SB_INACTIVE) {
    return 1;
  }

  shell_puts("\r\n[BOOT] secure boot: APP REFUSED - ");
  switch(rc) {
    case QUILLA_SB_ERR_NO_MANIFEST:
      shell_puts("no signed manifest");
      break;
    case QUILLA_SB_ERR_LEN:
      shell_puts("image length out of range");
      break;
    case QUILLA_SB_ERR_HASH:
      shell_puts("image content altered");
      break;
    case QUILLA_SB_ERR_SIG:
      shell_puts("signature invalid");
      break;
    case QUILLA_SB_ERR_ROLLBACK:
      shell_puts("version rollback attempted");
      break;
    default:
      shell_puts("verification failed");
      break;
  }
  shell_puts("\r\n");
  return 0;
}

static void
qboot_shell_run(void)
{
  shell_register_cmd(&qboot_qiap_cmd);
  shell_register_cmd(&qboot_swapp_cmd);
  shell_register_cmd(&qboot_pinfo_cmd);
  qboot_shell_flow = kern_flow_register("qboot-shell", KERN_FLOW_MASK(1),
                                         (clock_time_t)0U);
#ifndef QUILLA_QBOOT_VARIANT_STR
#define QUILLA_QBOOT_VARIANT_STR "OPEN"
#endif
  shell_puts("\r\n[BOOT] Quilla V" QUILLA_VERSION_STRING " [" QUILLA_QBOOT_VARIANT_STR "]\r\n");

  while(1) {
    (void)kern_flow_checkpoint(qboot_shell_flow, KERN_FLOW_TOKEN(0));
    process_run();
    (void)qboot_stack_ok();
    if(kern_flow_check() != KERN_OK) {
      shell_puts("\r\n[BOOT] flow sequence violation\r\n");
    }
  }
}

int
main(void)
{
  uint32_t flag;

  clock_init();
  (void)kern_stack_init();
  quilla_qboot_flag_init();
  shellport_init();

  shell_set_partition_tag("qboot");

  flag = quilla_qboot_flag_get();
  if(flag == QUILLA_QBOOT_FLAG_QIAP) {
    qboot_qiap_run();
  }
  if(flag == QUILLA_QBOOT_FLAG_QBOOT) {
    qboot_shell_run();
  }

  if(qboot_stack_ok() && qboot_app_image_valid() && qboot_secure_boot_ok()) {
#if KERN_CONF_STACK_SHELL
    kern_stack_report("boot-gate");
#endif
    quilla_qboot_hal_jump_to_app(QUILLA_QBOOT_APP_START);
  }

  qboot_shell_run();
  return 0;
}

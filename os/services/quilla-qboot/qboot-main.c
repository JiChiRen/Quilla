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
 *
 * ----------------------------------------------------------------------------
 * QBOOT (bootloader) entry point. Compiled into the QBOOT image only, and
 * shared by every MCU port: this single translation unit owns the boot
 * policy, while the CPU-arch specifics it depends on (clock_init(),
 * shellport_init(), quilla_qboot_hal_app_image_valid()) live in each port's
 * arch/cpu/<mcu>/ tree and are pulled in here. This is the inverse of the old
 * layout, where a copy of this logic lived in every
 * arch/cpu/<mcu>/qboot-main.c.
 *
 * Boot policy:
 *   - flag == QIAP  -> enter the serial upgrade immediately;
 *   - otherwise    -> jump to the APP when its image looks sane; only when
 *                      there is no runnable APP (blank / corrupted image) does
 *                      the QBOOT keep the interactive shell alive, so the "qiap"
 *                      command is still reachable over the serial link. A lost
 *                      stack canary refuses the handover too: RAM is no longer
 *                      trustworthy, so no verdict taken from it counts.
 *
 * The serial link is shared with the shell: while the QIAP state machine is
 * active, received bytes are routed to it via a small ring buffer (harvested
 * from the CIRCULAR RX DMA by shellport_rx_harvest() -- called from the shell
 * process during interactive use, or from this loop while the upgrade owns the
 * CPU -- and drained here in the main loop) so that Flash erase/program never
 * runs inside an interrupt.
 *
 * The upgrade protocol itself lives in quilla-qiap.c and every machine
 * operation goes through quilla-qboot-hal.h.
 * ----------------------------------------------------------------------------
 */

/* This translation unit no longer pulls in a vendor HAL header, so ask for the
 * fixed-width types up front: "sys/clock.h" only includes <inttypes.h> and
 * relies on it for uint64_t. Do not depend on include order. */
#include <stdint.h>

#include "sys/clock.h"

#include "quilla-shell.h"
#include "quilla-qboot-hal.h"
#include "quilla-qboot-flag.h"
#include "quilla-qiap.h"
#include "quilla-qboot-secure.h"
#include "contiki.h"
#include "sys/kern-stack.h"
#include "sys/kern-flow.h"
#include "shellport.h"

/* ---- ring buffer bridging DMA ISR -> main-loop QIAP parser ---------------- */
#define QBOOT_RX_RING_SZ  256U
static uint8_t qboot_rx_ring[QBOOT_RX_RING_SZ];
static volatile uint16_t qboot_rx_head;   /* written by ISR (sink) */
static volatile uint16_t qboot_rx_tail;   /* written by main loop  */

/* QBOOT_BUILD-guarded sink installed on the serial port. Pushes every received
 * byte into the ring; kept tiny so it is safe inside the DMA ISR. */
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

static void qboot_shell_run(void);   /* defined below; the safe landing spot */

/* Id of the shell-loop flow. -1 until registered; kern_flow_checkpoint()
 * rejects it with KERN_ERR_RANGE, which every call site discards. */
static int qboot_shell_flow = -1;

/* ----------------------------------------------------------------------------
 * Stack watermark.
 *
 * The QBOOT has no MPU either, so the canary at the bottom of the stack
 * reservation is the only overflow detector. A lost canary means the stack has
 * already been written through into the static region, so every RAM-resident
 * result - including the outcome of the signature verification itself - is
 * untrustworthy from that point on. The boot gate therefore refuses to hand
 * control to the APP, exactly as it does for a failed signature: no reset, no
 * brick, shell kept so the unit stays recoverable over the serial link.
 *
 * The message is printed once; the caller decides what to do with the verdict.
 * -------------------------------------------------------------------------- */
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
    /* The monitor never armed: the linker symbols do not describe the stack
     * this image runs on, so the device is running with no overflow detection
     * at all. Say so once - it is a finding, not a silent default. Booting is
     * still allowed here so a mis-linked image does not turn into a brick. */
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

/* ----------------------------------------------------------------------------
 * QIAP receive loop: drain the ring into the parser until it reports done, then
 * reset so the (now updated) APP boots next.
 * -------------------------------------------------------------------------- */
static void
qboot_qiap_run(void)
{
  uint8_t b;

  shellport_set_raw_sink(qboot_rx_sink);
  quilla_qiap_init();
  shell_puts("\r\n[QIAP] enter upgrade mode. Send ENTER frame.\r\n");

  while(1) {
    /* The CIRCULAR RX DMA runs forever and is drained only by
     * shellport_rx_harvest(), which the shell process calls during interactive
     * use. While we own the CPU (no process_run() here) the shell process is not
     * scheduled, so harvest the DMA ring ourselves -- otherwise qboot_rx_ring
     * would never fill and every ENTER/DATA frame would time out.
     *
     * CRITICAL: harvest pushes every byte the DMA has written since the last
     * call in one shot, but the parser consumes only ONE byte per loop pass.
     * The loop runs far faster than bytes arrive (~10 bytes land per pass), so a
     * pass that pops just one byte lets qboot_rx_ring grow by ~9 every pass;
     * over the ~62 passes it takes to chew through one 62-byte DATA frame the
     * ring overflows (qboot_rx_sink drops on full), a whole frame is silently
     * lost, and the QIAP parser desyncs permanently -- thereafter every ACK
     * echoes the PREVIOUS chunk's start (offset == one CHUNK). Draining the
     * whole ring each pass keeps it bounded and lossless. */
    shellport_rx_harvest();

    if(quilla_qiap_is_done()) {
      /* Second deepest path: the QIAP state machine with SHA-256 and Flash
       * erase/program. Success resets immediately, so this is the only
       * point where its high-water mark can still be read. */
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
      /* Corrupted stack: stop driving the Flash state machine instead of
       * risking an erase/program issued from bogus parameters. Restore shell
       * input first, or the raw sink would swallow every keystroke. */
      shellport_set_raw_sink(NULL);
      qboot_shell_run();            /* never returns */
    }
  }
}

/* ----------------------------------------------------------------------------
 * Shell command "qiap": switch from interactive shell into the upgrade flow.
 * -------------------------------------------------------------------------- */
static void
qboot_qiap_cmd_handler(const char *args)
{
  (void)args;
  shell_puts("qiap: switching to upgrade mode\r\n");
  qboot_qiap_run();   /* never returns (resets on success) */
}

static const shell_cmd_t qboot_qiap_cmd = {
  "qiap", qboot_qiap_cmd_handler, "enter QIAP serial upgrade"
};

/* ----------------------------------------------------------------------------
 * Shell command "swapp": request a return to the APP. Refuses while an upgrade
 * is in progress (flag == QIAP), otherwise writes APP1 and reboots so the APP
 * boots next.
 * -------------------------------------------------------------------------- */
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

/* ----------------------------------------------------------------------------
 * Shell command "pinfo": report the QBOOT partition layout. The host upgrader
 * queries this so it can size the APP region FROM THE SOC instead of hardcoding
 * a per-MCU value: APP_END equals SOC_FLASH_END, which differs per part (e.g.
 * PY32F040xB = 128 KiB, GD32F502VE = 512 KiB). The pre-upgrade size cap the
 * host applies must track this exactly, so the device is the source of truth.
 * -------------------------------------------------------------------------- */
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
  /* Firmware security version baked into the APP image at the fixed address
   * (0x08007400); 0 means the .quilla_secver region is missing / corrupt. This
   * is the value the anti-rollback floor is compared against. */
  shell_output("[PART] FW_SECVER=0x%08X (%s)\r\n",
               (unsigned)fw_ver,
               (fw_ver == 0U) ? "missing/corrupt" : "present");
}

static const shell_cmd_t qboot_pinfo_cmd = {
  "pinfo", qboot_pinfo_cmd_handler, "show QBOOT partition layout (APP_START/END/SIZE)"
};

/* ----------------------------------------------------------------------------
 * Secure boot gate.
 *
 * A mechanically sane image test is not enough once a verification key is
 * provisioned: the image must also carry a manifest whose signature verifies
 * and whose version is not below the anti-rollback floor. Returns 1 when the
 * APP may be jumped.
 *
 * With no key provisioned the gate reports QUILLA_SB_INACTIVE and stays out of
 * the way, so an unprovisioned device keeps booting unsigned images exactly as
 * before. On refusal the reason is printed and the caller keeps the QBOOT
 * shell alive, so the unit stays recoverable over the serial link instead of
 * becoming a brick.
 * -------------------------------------------------------------------------- */
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

/* ----------------------------------------------------------------------------
 * Interactive shell loop (no runnable APP). Keeps the shell process running so
 * the built-in "part" command answers "qboot" and "qiap" can start an upgrade.
 * -------------------------------------------------------------------------- */
static void
qboot_shell_run(void)
{
  shell_register_cmd(&qboot_qiap_cmd);    /* expose "qiap" to enter upgrade   */
  shell_register_cmd(&qboot_swapp_cmd);  /* expose "swapp" to return to APP  */
  shell_register_cmd(&qboot_pinfo_cmd);  /* expose "pinfo" partition layout  */
  /* budget 0 = order only, no timing check. This is not laziness:
   * QBOOT_BUILD excludes clock_update_ticks() from clock.c, so clock_time()
   * never advances in this image and a time budget would either be inert or
   * wrong. Sequence order still applies and is the part that matters here. */
  qboot_shell_flow = kern_flow_register("qboot-shell", KERN_FLOW_MASK(1),
                                        (clock_time_t)0U);
  /* Boot stamp. Short visible version line "[BOOT] Quilla V<ver> [VARIANT]".
   * The host upgrader (tools/quilla_host_upgrader.py) keys off the "[BOOT]" tag
   * and the "part" command echo (which contains "qboot") to decide whether to
   * send "qiap" first, so the banner stays minimal with no extra "QBOOT shell"
   * line. QUILLA_QBOOT_VARIANT_STR is set per build (SEALED / SEC / CHECK);
   * default "OPEN" must never ship. */
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

  clock_init();                /* minimal init: SysTick only (no LPTIM/lpm) */
  (void)kern_stack_init();     /* paint before anything allocates on it     */
  quilla_qboot_flag_init();
  shellport_init();            /* USART up; shell writer set; shell started */

  shell_set_partition_tag("qboot");

  flag = quilla_qboot_flag_get();
  if(flag == QUILLA_QBOOT_FLAG_QIAP) {
    qboot_qiap_run();          /* boot straight into upgrade (crash recovery) */
  }
  if(flag == QUILLA_QBOOT_FLAG_QBOOT) {
    qboot_shell_run();        /* 'swqboot' asked to stay in QBOOT; do NOT
                                * jump the APP even if its image is sane. */
  }

  /* APP1, or a blank/corrupted flag page (reads back as APP1): hand control to
   * the APP whenever a sane image is present. Only an empty or damaged APP
   * region falls through to the QBOOT shell, keeping 'qiap' reachable.
   *
   * The image test (quilla_qboot_hal_app_image_valid()) is a mechanical sanity
   * test only; when a key is provisioned the signature gate has the final say.
   *
   * The stack verdict comes first and is not advisory: once the canary is gone
   * the RAM content cannot be trusted, so neither the image test nor the
   * signature verdict would mean anything. Checked here rather than left to
   * the shell loop because the jump happens before any loop runs. */
  if(qboot_stack_ok() &&
     quilla_qboot_hal_app_image_valid() &&
     qboot_secure_boot_ok()) {
    /* Deepest path in the image: SHA-256 over the APP plus the signature
     * check. The short-circuit above means it only runs when a mechanically
     * valid APP is present, so a blank board can never measure it. Emit the
     * high-water mark before the jump, or the number is gone. */
#if KERN_CONF_STACK_SHELL
    kern_stack_report("boot-gate");
#endif
    quilla_qboot_hal_jump_to_app(QUILLA_QBOOT_APP_START);
  }

  qboot_shell_run();          /* nothing runnable: interactive shell, 'qiap' */
  return 0;
}

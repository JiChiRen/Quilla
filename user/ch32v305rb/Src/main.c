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
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
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
 * APP entry point for the CH32V305VB (RISC-V). Functional design is aligned to
 * the GD32F502VE port.
 *
 * Deliberate gaps / differences against the Cortex-M ports, recorded so they
 * are not mistaken for oversights:
 *   - No SCB->VTOR relocation: the RISC-V startup (handle_reset) points mtvec
 *     at its own _vector_base (linked at QUILLA_QBOOT_APP_START), so there is
 *     nothing to relocate in main().
 *   - QUILLA_APP_SECVER_SELFCHECK is compiled out (see the RISC-V Makefile).
 *     The APP-side self-check uses Cortex-M inline asm (cpsid i) that has no
 *     RISC-V equivalent; it is obfuscation only, and the real anti-rollback
 *     gate is QBOOT's signature + floor, which is unaffected.
 *   - QWDT_CONF_SAFE_TIME_MS is 0 (process safety time not stated); the
 *     watchdog must not be claimed as a safety measure yet.
 * ----------------------------------------------------------------------------
 */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "contiki.h"
#include "sys/autostart.h"
#include "sys/platform.h"
#include "sys/etimer.h"
#include "sys/kern-status.h"
#include "sys/kern-guard.h"
#include "sys/kern-stack.h"
#include "sys/kern-flow.h"
#include "lpm.h"
#include "shellport.h"
#include "dev/watchdog.h"
#include "dev/qwdt.h"
#include "dev/qclk.h"
#include "../../../os/services/shell/quilla-shell.h"
#include "../../../os/services/quilla-qboot/quilla-app-qboot.h"
#include "../../../os/sys/quilla-app-secver.h"

/* Firmware security-version region for the secure-boot anti-rollback gate.
 * Placed at the fixed flash address the linker reserves (.quilla_secver,
 * 0x00007400 = QUILLA_QBOOT_APP_START + 0x400 on this MCU). QBOOT reads it
 * directly; see quilla-app-secver.h. */
QUILLA_APP_SECVER_PLACE();

/* Checkpoints of the idle-loop flow. The order is the sequence: every one of
 * them must be visited, in this order, inside one pass. See kern-flow.h. */
#define APP_CP_LOOP_BEGIN   KERN_FLOW_TOKEN(0)
#define APP_CP_BURST_DONE   KERN_FLOW_TOKEN(1)
#define APP_CP_GUARD_OK     KERN_FLOW_TOKEN(2)
#define APP_CP_TIMER_OK     KERN_FLOW_TOKEN(3)
#define APP_CP_STACK_OK     KERN_FLOW_TOKEN(4)
#define APP_CP_CLOCK_OK     KERN_FLOW_TOKEN(5)
#define APP_CP_FEED         KERN_FLOW_TOKEN(6)
#define APP_LOOP_MASK       KERN_FLOW_MASK(7)

#define APP_LOOP_BUDGET     (clock_time_t)(2U * CLOCK_SECOND)
#define APP_HEARTBEAT_BUDGET (clock_time_t)(3U * CLOCK_SECOND)

static int app_loop_flow = -1;
static int app_heartbeat_flow = -1;

static void APP_GpioConfig(void);

PROCESS(project_process, "ProjProc");

PROCESS_THREAD(project_process, ev, data)
{
  static struct etimer et;
  PROCESS_BEGIN();

  etimer_set(&et, CLOCK_SECOND);

  while(1) {
    PROCESS_WAIT_EVENT();
    if(ev == PROCESS_EVENT_TIMER && etimer_expired(&et)) {
#if defined(BOARD_LED_PORT) && defined(BOARD_LED_PIN)
      GPIO_ResetBits(BOARD_LED_PORT, BOARD_LED_PIN);
      GPIO_SetBits(BOARD_LED_PORT, BOARD_LED_PIN);
#endif
      etimer_set(&et, CLOCK_SECOND);
      if(app_heartbeat_flow >= 0) {
        (void)kern_flow_checkpoint(app_heartbeat_flow, KERN_FLOW_TOKEN(0));
      }
    }
  }
  PROCESS_END();
}

AUTOSTART_PROCESSES(&project_process);

int main(void)
{
  /* No VTOR to relocate on RISC-V: handle_reset already pointed mtvec at this
   * image's _vector_base. */

  /* Interrupts are unmasked explicitly here: this image can be entered from
   * the QBOOT branch (which leaves mstatus.MIE clear, like a real reset), not
   * only from reset. With interrupts masked SysTick never runs, so
   * clock_time() freezes and no etimer ever expires. */
  __enable_irq();

#if QUILLA_APP_SECVER_SELFCHECK
  quilla_app_secver_selfcheck();
#endif

  (void)kern_stack_init();

  platform_init_stage_one();

  if(process_init() != KERN_OK) {
    APP_ErrorHandler("process_init");
  }
  if(process_start(&etimer_process, NULL) != KERN_OK) {
    APP_ErrorHandler("process_start");
  }

  /* Measure the IWDG's LSI clock before the watchdog is dimensioned. A failed
   * measurement does not stop the boot: the watchdog falls back to its
   * compile-time assumption. */
  if(qclk_init() == KERN_OK) {
    if(qclk_calibrate() == KERN_OK) {
      (void)qwdt_calibrate(qclk_measured_irc40k_hz());
    }
  }

  if(qwdt_init() != KERN_OK) {
    APP_ErrorHandler("qwdt_init");
  }

  APP_GpioConfig();

  autostart_start(autostart_processes);

#if SHELLPORT_CONF_ENABLED
  shellport_init();
  shell_output("[QAPP] " QUILLA_PROJECT_NAME " V" QUILLA_PROJECT_VERSION_STRING "\r\n> ");
  shell_set_partition_tag("app1");
  quilla_app_qboot_init();
#if KERN_CONF_STACK_SHELL
  kern_stack_shell_init();
#endif

#ifdef QUILLA_APP_BOOT_DIAG
  {
    char wdt_line[120];
    if(qwdt_describe(wdt_line, (unsigned int)sizeof(wdt_line)) > 0U) {
      shell_output(wdt_line);
      shell_output("\r\n");
    }
  }
  {
    char clk_line[160];
    if(qclk_describe(clk_line, (unsigned int)sizeof(clk_line)) > 0U) {
      shell_output(clk_line);
      shell_output("\r\n");
    }
  }
#endif
#endif /* SHELLPORT_CONF_ENABLED */

  app_loop_flow = kern_flow_register("app-loop", APP_LOOP_MASK,
                                     APP_LOOP_BUDGET);
  app_heartbeat_flow = kern_flow_register("app-heartbeat", KERN_FLOW_MASK(1),
                                          APP_HEARTBEAT_BUDGET);

  /* Arm last, with everything above proven to have run. The IWDG keeps
   * counting through lpm_enter(): this port's deepest mode is PM0 (sleep),
   * which only gates the core clock, so SysTick resumes the loop. */
  watchdog_start();

  while(1) {
    process_num_events_t r;

    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_LOOP_BEGIN);

    do {
      r = process_run();
      watchdog_periodic();
    } while(r > 0);

    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_BURST_DONE);

    if(kern_guard_check() != KERN_OK) {
      APP_ErrorHandler("kern_guard_check");
    }
    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_GUARD_OK);

    if(etimer_check() != KERN_OK) {
      APP_ErrorHandler("etimer_check");
    }
    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_TIMER_OK);

    if(kern_stack_check() == KERN_ERR_STACK) {
      APP_ErrorHandler("kern_stack_check");
    }
    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_STACK_OK);

    if(qclk_poll() == KERN_ERR_CLOCK) {
      APP_ErrorHandler("qclk_poll");
    }
    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_CLOCK_OK);

    if(kern_flow_check() != KERN_OK) {
      APP_ErrorHandler("kern_flow_check");
    }
    watchdog_periodic();
    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_FEED);

    platform_idle();
  }

  return 0;
}

/* Direct, driver-independent dying gasp, mirroring the GD32F502VE port. QBOOT
 * has already configured USART2 at 115200 before branching into this image and
 * the peripheral registers survive the jump, so this works long before
 * shellport_init() runs. Poll TXE to place each byte, then TC so the string is
 * physically on the wire before the reset. */
static void
emergency_uart_gasp(const char *s)
{
  volatile uint32_t d;

  if(s == NULL) {
    return;
  }
  while(*s != '\0') {
    while(USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET) {
    }
    USART_SendData(USART2, (uint32_t)(uint8_t)(*s++));
  }
  while(USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET) {
  }
  for(d = 0U; d < 400000UL; d++) {
  }
}

static void
APP_GpioConfig(void)
{
#if defined(BOARD_LED_PORT) && defined(BOARD_LED_PIN)
  RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
  GPIO_InitTypeDef g;
  g.GPIO_Pin   = BOARD_LED_PIN;
  g.GPIO_Speed = GPIO_Speed_50MHz;
  g.GPIO_Mode  = GPIO_Mode_Out_PP;
  GPIO_Init(BOARD_LED_PORT, &g);
#endif
}

void APP_ErrorHandler(const char *reason)
{
  emergency_uart_gasp("ERR: ");
  emergency_uart_gasp(reason);
  emergency_uart_gasp("\r\n");
  NVIC_SystemReset();
  while(1) {
  }
}

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
 * APP entry point for GD32F502VE.
 *
 * Deliberate gaps against ideal practice, recorded so they are not mistaken
 * for oversights:
 *   - QWDT_CONF_SAFE_TIME_MS is 0, i.e. the process safety time is NOT STATED.
 *     The watchdog therefore has no defensible relationship to any hazard and
 *     must not be claimed as a safety measure yet; the "wdt" line says so.
 *   - The HXTAL clock monitor (RCU_CTL_HCKMEN) is not enabled. This port does
 *     have one, unlike the PY32F040xB, and it is the only mechanism that turns a
 *     dead crystal into a reportable NMI. Enabling it claims NMI, so the
 *     decision belongs to platform code, not to a driver.
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
 * 0x08007400). QBOOT reads it directly; see quilla-app-secver.h. */
QUILLA_APP_SECVER_PLACE();

/* Checkpoints of the idle-loop flow. The order is the sequence: every one of
 * them must be visited, in this order, inside one pass. See kern-flow.h. */
#define APP_CP_LOOP_BEGIN   KERN_FLOW_TOKEN(0)   /* top of the while(1)     */
#define APP_CP_BURST_DONE   KERN_FLOW_TOKEN(1)   /* event burst drained     */
#define APP_CP_GUARD_OK     KERN_FLOW_TOKEN(2)   /* kern_guard_check passed */
#define APP_CP_TIMER_OK     KERN_FLOW_TOKEN(3)   /* etimer_check passed     */
#define APP_CP_STACK_OK     KERN_FLOW_TOKEN(4)   /* kern_stack_check passed */
#define APP_CP_CLOCK_OK     KERN_FLOW_TOKEN(5)   /* qclk_poll passed        */
#define APP_CP_FEED         KERN_FLOW_TOKEN(6)   /* watchdog refreshed      */
#define APP_LOOP_MASK       KERN_FLOW_MASK(7)

/* Worst admissible time for one idle pass. Very generous: the real cost is a
 * handful of microseconds unless an event handler stalls. Anything that trips
 * this is a hang, not jitter. */
#define APP_LOOP_BUDGET     (clock_time_t)(2U * CLOCK_SECOND)

/* The heart-beat flow proves that this process is still being scheduled. Its
 * budget has to cover the 1 s etimer plus scheduling slack. */
#define APP_HEARTBEAT_BUDGET (clock_time_t)(3U * CLOCK_SECOND)

/* Ids handed out by kern_flow_register(). -1 means "not registered";
 * kern_flow_checkpoint() rejects such an id with KERN_ERR_RANGE, which is
 * discarded at every call site, so the flows simply do not run. */
static int app_loop_flow = -1;
static int app_heartbeat_flow = -1;

/* Private function prototypes -----------------------------------------------*/
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
      gpio_bit_toggle(BOARD_LED_PORT, BOARD_LED_PIN);
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

/**
  * @brief  Main program.
  * @retval int
  */
int main(void)
 {
#ifdef QAPP_BUILD
  /* The vector table of this image is linked at QUILLA_QBOOT_APP_START, so
   * SCB->VTOR has to follow it. The QBOOT already sets VTOR before branching,
   * but this image is also reachable from a debugger or from a mass-erase +
   * load, and an unrelocated table means the first exception vectors into the
   * erased QBOOT region. Do it before any interrupt can fire. */
  SCB->VTOR = QUILLA_QBOOT_APP_START;
#endif

  /* Interrupts are unmasked explicitly here rather than relying on
   * platform_init_stage_two(): this image can be entered from the QBOOT
   * branch, not only from reset, and PRIMASK is part of the hand-over state.
   * With interrupts masked SysTick never runs, so clock_time() freezes and no
   * etimer ever expires - the process would stop right after its first
   * etimer_set(). */
  __enable_irq();

  /* Anti-rollback self-check (APP-side obfuscation). Refuses to run if the
   * in-flash security-version region is internally inconsistent. Honest scope:
   * this is NOT a cryptographic control -- the real gate is QBOOT's signature +
   * floor. See quilla-app-secver.h. Disabled when QUILLA_APP_SECVER_SELFCHECK=0. */
#if QUILLA_APP_SECVER_SELFCHECK
  quilla_app_secver_selfcheck();
#endif

  /* Paint the stack reservation before anything else allocates on it. A
   * failure here means the linker symbols do not describe the stack this
   * image runs on: leave the monitor disarmed rather than trapping the
   * board in a reset loop. The "stack" shell command reports "not armed",
   * which is the intended bring-up check. */
  (void)kern_stack_init();

  platform_init_stage_one();

  if(process_init() != KERN_OK) {
    APP_ErrorHandler("process_init");
  }
  if(process_start(&etimer_process, NULL) != KERN_OK) {
    APP_ErrorHandler("process_start");
  }

  /* Measure IRC40K before the watchdog is dimensioned. qclk reads the rtimer
   * that soc_init() started above, so both sides of the comparison are running
   * by now.
   *
   * A failed measurement does not stop the boot: the watchdog then falls back
   * to its compile-time assumption, qclk_describe() prints the assumption
   * actually used once the shell is up, and the periodic check keeps looking
   * at the ratio. What it costs is width - an assumed rate forces both
   * timeout bounds to be pessimistic at once. */
  if(qclk_init() == KERN_OK) {
    if(qclk_calibrate() == KERN_OK) {
      (void)qwdt_calibrate(qclk_measured_irc40k_hz());
    }
  }

  /* The watchdog is configured, not started, here. qwdt_init() decides the
   * divider and reload and its result matters: a timeout that cannot be
   * expressed with the available prescalers, or one that does not fit the
   * configured process safety time, must stop the boot instead of silently
   * arming whatever interval the peripheral happened to reach. */
  if(qwdt_init() != KERN_OK) {
    APP_ErrorHandler("qwdt_init");
  }

  /* Initialize GPIO */
  APP_GpioConfig();

  autostart_start(autostart_processes);

#if SHELLPORT_CONF_ENABLED
  shellport_init();
  /* Print the project (QAPP) version on boot: "[QAPP] <project-name>
   * V<major.minor.build>". This is the single place the project version appears
   * on the wire for the application image; the shell itself prints no version.
   * Also emits the initial ">" prompt. Mirrors the PY32F040xB bring-up (main.c). */
  shell_output("[QAPP] " QUILLA_PROJECT_NAME " V" QUILLA_PROJECT_VERSION_STRING "\r\n> ");
  /* Tell the (shared) shell which partition we are, and expose the
   * "swqboot" command so the host can request a serial upgrade. */
  shell_set_partition_tag("app1");
  quilla_app_qboot_init();
#if KERN_CONF_STACK_SHELL
  kern_stack_shell_init();   /* "stack": size, peak usage, margin, canary */
#endif

  /* Print the watchdog configuration actually running. "safe=NOT STATED" is
   * not an error, it is the current state of knowledge: no hazard analysis
   * has produced a process safety time yet. A timeout that failed to fit one
   * would not have got past qwdt_init() above. */
#ifdef QUILLA_APP_BOOT_DIAG
  {
    char wdt_line[120];
    if(qwdt_describe(wdt_line, (unsigned int)sizeof(wdt_line)) > 0U) {
      shell_output(wdt_line);
      shell_output("\r\n");
    }
  }
#endif
#ifdef QUILLA_APP_BOOT_DIAG
  {
    char clk_line[160];
    if(qclk_describe(clk_line, (unsigned int)sizeof(clk_line)) > 0U) {
      shell_output(clk_line);
      shell_output("\r\n");
    }
  }
#endif
#endif /* SHELLPORT_CONF_ENABLED */

  /* Register the monitored flows before anything can produce a checkpoint,
   * so no cycle starts from an unregistered id. Registration failure means
   * KERN_CONF_FLOW_MAX_TRACKS is too small; both flows below then simply do
   * not run and kern_flow_track_count() reports zero. */
  app_loop_flow = kern_flow_register("app-loop", APP_LOOP_MASK,
                                     APP_LOOP_BUDGET);
  app_heartbeat_flow = kern_flow_register("app-heartbeat", KERN_FLOW_MASK(1),
                                          APP_HEARTBEAT_BUDGET);

  /* Arm last, with everything above proven to have run: from here a missed
   * cycle is a reset. Note that the FWDGT keeps counting through lpm_enter()
   * - this port's deepest mode is PM0 (sleep), which only gates the core
   * clock, so SysTick resumes the loop and the feed interval is unchanged. */
  watchdog_start();

  while(1) {
    process_num_events_t r;

    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_LOOP_BEGIN);

    do {
      r = process_run();
      /* Fed inside the burst as well, so that a long event chain cannot
       * starve the counter. The sequence check below is what keeps this from
       * degenerating into "something is alive somewhere". */
      watchdog_periodic();
    } while(r > 0);

    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_BURST_DONE);

    /*
     * Kernel integrity is checked once per idle pass, outside of the
     * event burst, so that a corrupted list is reported before it can
     * affect the next scheduling round.
     */
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

    /* Closes a window roughly every QCLK_CONF_WINDOW_IRC40K reference counts
     * and only then compares; most passes do nothing but read the divider. */
    if(qclk_poll() == KERN_ERR_CLOCK) {
      APP_ErrorHandler("qclk_poll");
    }
    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_CLOCK_OK);

    /* Refresh only after every check above has been reached, in order. A
     * branch that bypasses one of them closes no cycle here, so the counter
     * stops being refreshed by this path and the watchdog takes the unit to
     * reset rather than leaving it running without that check. */
    if(kern_flow_check() != KERN_OK) {
      APP_ErrorHandler("kern_flow_check");
    }
    watchdog_periodic();
    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_FEED);

    platform_idle();
  }

  return 0;
}

/* Direct, driver-independent dying gasp, mirroring the PY32F040xB port. QBOOT
 * has already configured USART0 at 115200 before branching into this image
 * and the peripheral registers survive the jump, so this works long before
 * shellport_init() runs. Poll TBE to place each byte, then TC so the string
 * is physically on the wire before the reset. */
static void
emergency_uart_gasp(const char *s)
{
  volatile uint32_t d;

  if(s == NULL) {
    return;
  }
  while(*s != '\0') {
    while((USART_STAT0(USART0) & USART_STAT0_TBE) == 0U) {
      /* Wait until the transmit data register is empty. */
    }
    USART_DATA(USART0) = (uint32_t)(uint8_t)(*s++);
  }
  while((USART_STAT0(USART0) & USART_STAT0_TC) == 0U) {
    /* Wait until the last byte has been shifted out. */
  }
  /* Hold the line open briefly so the host's USB-UART bridge drains before
   * the reset boundary; the reason must not be the thing that gets cut. */
  for(d = 0U; d < 400000UL; d++) {
  }
}

/**
  * @brief  GPIO configuration.
  */
static void APP_GpioConfig(void)
{
#if defined(BOARD_LED_PORT) && defined(BOARD_LED_PIN)
  rcu_periph_clock_enable(BOARD_LED_RCU);
  gpio_mode_set(BOARD_LED_PORT, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE,
                BOARD_LED_PIN);
  gpio_output_options_set(BOARD_LED_PORT, GPIO_OTYPE_PP, GPIO_OSPEED_LEVEL1,
                          BOARD_LED_PIN);
#endif
}

/**
  * @brief  Error executing function.
  * @param  reason: name of the failed check, printed before the reset.
  * @retval None
  */
void APP_ErrorHandler(const char *reason)
{
  /* Dying gasp first: the reason on the wire is the whole point, and it must
   * not depend on Flash or shell state. */
  emergency_uart_gasp("ERR: ");
  emergency_uart_gasp(reason);
  emergency_uart_gasp("\r\n");
  /* Reset so the failure repeats visibly on every boot instead of sitting in
   * a silent spin; the reset reason classifies as a software-triggered one. */
  NVIC_SystemReset();
  while(1) {
  }
}

/**
  ******************************************************************************
  * @file    main.c
  * @author  MCU Application Team
  * @brief   Main program body
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2023 Puya Semiconductor Co.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by Puya under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2016 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
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
#include "dev/watchdog.h"
#include "dev/qwdt.h"
#include "dev/qclk.h"
#include "lpm.h"
#include "shellport.h"
#include "../../../os/services/shell/quilla-shell.h"
#include "../../../os/services/quilla-qboot/quilla-app-qboot.h"
#include "../../../os/services/quilla-qboot/quilla-qboot-hal.h"
#include "py32f040_hal_flash.h"
#include "../../../os/sys/quilla-app-secver.h"

/* Firmware security-version region for the secure-boot anti-rollback gate.
 * Placed at the fixed flash address the linker reserves (.quilla_secver,
 * 0x08007400). QBOOT reads it directly; see quilla-app-secver.h. */
QUILLA_APP_SECVER_PLACE();


/* Private define ------------------------------------------------------------*/
#define LED_GPIO_PIN                 LED3_PIN
#define LED_GPIO_PORT                LED3_GPIO_PORT
#define LED_GPIO_CLK_ENABLE()        LED3_GPIO_CLK_ENABLE()

/* Checkpoints of the idle-loop flow. The order is the sequence: every one of
 * them must be visited, in this order, inside one pass. See kern-flow.h. */
#define APP_CP_LOOP_BEGIN   KERN_FLOW_TOKEN(0)   /* top of the while(1)     */
#define APP_CP_BURST_DONE   KERN_FLOW_TOKEN(1)   /* event burst drained     */
#define APP_CP_GUARD_OK     KERN_FLOW_TOKEN(2)   /* kern_guard_check passed */
#define APP_CP_TIMER_OK     KERN_FLOW_TOKEN(3)   /* etimer_check passed     */
#define APP_CP_STACK_OK     KERN_FLOW_TOKEN(4)   /* kern_stack_check passed */
#define APP_CP_CLOCK_OK     KERN_FLOW_TOKEN(5)   /* clock cross-check made  */
#define APP_CP_FEED         KERN_FLOW_TOKEN(6)   /* watchdog refreshed      */
#define APP_LOOP_MASK       KERN_FLOW_MASK(7)

/* Worst admissible time for one idle pass. Very generous: the real cost is a
 * handful of microseconds unless an event handler stalls. Anything that trips
 * this is a hang, not jitter. */
#define APP_LOOP_BUDGET     (clock_time_t)(2U * CLOCK_SECOND)

/* The heart-beat flow proves that this process is still being scheduled. Its
 * budget has to cover the 1 s etimer plus scheduling slack. */
#define APP_HEARTBEAT_BUDGET (clock_time_t)(3U * CLOCK_SECOND)

/* Wedge probe wiring (APP image only, enabled via -DQUILLA_WEDGE_PROBE=1).
 * Every flow checkpoint and both watchdog feeds touch the probe so the
 * SysTick-side monitor in clock.c knows exactly where the idle loop was
 * last alive. */
#ifndef QUILLA_WEDGE_PROBE
#define QUILLA_WEDGE_PROBE 0
#endif
#if QUILLA_WEDGE_PROBE
extern void wedge_probe_arm(void);
extern void wedge_probe_touch(unsigned int cp);
#define WEDGE_ARM()        wedge_probe_arm()
#define WEDGE_TOUCH(cp)    wedge_probe_touch(cp)
#else
#define WEDGE_ARM()        ((void)0)
#define WEDGE_TOUCH(cp)    ((void)0)
#endif

/* Private variables ---------------------------------------------------------*/
/* Ids handed out by kern_flow_register(). -1 means "not registered";
 * kern_flow_checkpoint() rejects such an id with KERN_ERR_RANGE, which is
 * discarded at every call site, so the flows simply do not run. */
static int app_loop_flow = -1;
static int app_heartbeat_flow = -1;
/* Private user code ---------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
static void APP_GpioConfig(void);

PROCESS(project_process, "ProjProc");

PROCESS_THREAD(project_process, ev, data)
{
  static struct etimer et;
  PROCESS_BEGIN();

  etimer_set(&et, CLOCK_SECOND);

  while(1)
  {
      PROCESS_WAIT_EVENT();
      if(ev == PROCESS_EVENT_TIMER && etimer_expired(&et))
      {
          HAL_GPIO_TogglePin(LED_GPIO_PORT, LED_GPIO_PIN);
          etimer_set(&et, CLOCK_SECOND);
          if(app_heartbeat_flow >= 0) {
            (void)kern_flow_checkpoint(app_heartbeat_flow, KERN_FLOW_TOKEN(0));
          }
      }
  }
  PROCESS_END();
}

AUTOSTART_PROCESSES(&project_process);

#ifdef QUILLA_APP_BOOT_DIAG
/* Print the reset cause latched in RCC->CSR at boot. Both IWDG and the
 * software/system resets land back here, but only the cause tells them apart:
 * the bare "while(1)" in the fault/error paths used to masquerade as an IWDG
 * reset, so this is the first line to read after a mystery reboot. Flags are
 * cleared afterwards so the next boot reports a fresh cause. */
static void
app_print_reset_cause(void)
{
  shell_output("reset: ");
  if(__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST))  shell_output("PIN ");
  if(__HAL_RCC_GET_FLAG(RCC_FLAG_PWRRST))  shell_output("PWR ");
  if(__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST))  shell_output("SFT ");
  if(__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST)) shell_output("IWDG ");
  if(__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST)) shell_output("WWDG ");
  if(__HAL_RCC_GET_FLAG(RCC_FLAG_OBLRST))  shell_output("OBL ");
  shell_output("\r\n");
  __HAL_RCC_CLEAR_RESET_FLAGS();
}
#endif

/**
  * @brief  Main program.
  * @retval int
  */
int main(void)
{
  /* Reset of all peripherals, Initializes the Systick. */
  // HAL_Init();
  platform_init_stage_one();

  /* platform_init_stage_two() owns the only INTERRUPTS_ENABLE() (= __enable_irq())
   * in the platform code, and it is not called here. Make it explicit: this image
   * can be entered from the QIAP branch, not only from reset, and PRIMASK is part
   * of the hand-over state. With interrupts masked SysTick never runs, so
   * clock_time() freezes and no etimer ever expires - the process would stop
   * right after its first etimer_set(). */
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

  if(process_init() != KERN_OK) {
    APP_ErrorHandler("process_init");
  }
  if(process_start(&etimer_process, NULL) != KERN_OK) {
    APP_ErrorHandler("process_start");
  }
  // ctimer_init();

  /* Watchdog first. qwdt_init() is called directly rather than through
   * watchdog_init() because this port wants the result: a timeout that cannot
   * be expressed with the available prescalers, or one that does not fit the
   * configured process safety time, must stop the boot instead of silently
   * arming whatever interval the peripheral happened to reach. */
  /* Measure the LSI before the watchdog is configured. Its timeout bounds
   * are the widest single function of that rate, and an assumed rate forces
   * them to be pessimistic in both directions at once. This needs the LPTIM1
   * that rtimer started inside platform_init_stage_one() above.
   *
  * A failed measurement does not stop the boot: the watchdog then falls
  * back to its compile-time assumption, qclk_describe() prints the
  * assumption actually used once the shell is up, and the periodic check
  * keeps looking at the ratio. */
  if(qclk_init() == KERN_OK) {
    if(qclk_calibrate() == KERN_OK) {
      (void)qwdt_calibrate(qclk_measured_lsi_hz());
    }
  }

  if(qwdt_init() != KERN_OK) {
    APP_ErrorHandler("qwdt_init");
  }

  // energest_init();

#if STACK_CHECK_ENABLED
  stack_check_init();
#endif

  /* Initialize GPIO */
  APP_GpioConfig();
  // platform_init_stage_two();

#if QUEUEBUF_ENABLED
  queuebuf_init();
#endif /* QUEUEBUF_ENABLED */
  // netstack_init();
  // node_id_init();
#if ECC_ENABLED
  ecc_init();
#endif /* ECC_ENABLED */

//   LOG_INFO("Starting " CONTIKI_VERSION_STRING "\n");
//   LOG_DBG("TARGET=%s", CONTIKI_TARGET_STRING);
// #ifdef CONTIKI_BOARD_STRING
//   LOG_DBG_(", BOARD=%s", CONTIKI_BOARD_STRING);
// #endif
//   LOG_DBG_("\n");

  // platform_init_stage_three();

#if BUILD_WITH_RPL_BORDER_ROUTER
  rpl_border_router_init();
  LOG_DBG("With RPL Border Router\n");
#endif /* BUILD_WITH_RPL_BORDER_ROUTER */

#if BUILD_WITH_ORCHESTRA
  orchestra_init();
  LOG_DBG("With Orchestra\n");
#endif /* BUILD_WITH_ORCHESTRA */

#if SHELLPORT_CONF_ENABLED
  shellport_init();
  /* Boot stamp. Printed by main() on every power-on / restart so the running
   * application image is identifiable on the wire: "[QAPP] <project-name>
   * V<major.minor.build>". QUILLA_PROJECT_NAME is overridable per product. If
   * this line does not show the version you just built, the board is NOT
   * running that image. */
  shell_output("[QAPP] " QUILLA_PROJECT_NAME " V" QUILLA_PROJECT_VERSION_STRING "\r\n> ");
  /* Latch and print the reset cause now that the UART is up. Done before any
   * shell command runs so a watchdog/software reset is visible on the wire as
   * "reset: IWDG" / "reset: SFT" instead of a silent mystery reboot. Only
   * emitted when QUILLA_APP_BOOT_DIAG is defined (debug build). */
#ifdef QUILLA_APP_BOOT_DIAG
  app_print_reset_cause();
#endif
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
    char wdt_line[96];
    if(qwdt_describe(wdt_line, (unsigned int)sizeof(wdt_line)) > 0U) {
      shell_output(wdt_line);
      shell_output("\r\n");
    }
  }
#endif
#ifdef QUILLA_APP_BOOT_DIAG
  {
    char clk_line[120];
    if(qclk_describe(clk_line, (unsigned int)sizeof(clk_line)) > 0U) {
      shell_output(clk_line);
      shell_output("\r\n");
    }
  }
#endif
#endif /* SHELLPORT_CONF_ENABLED */

#if BUILD_WITH_COAP
  coap_engine_init();
  LOG_DBG("With CoAP\n");
#endif /* BUILD_WITH_COAP */

#if BUILD_WITH_SNMP
  snmp_init();
  LOG_DBG("With SNMP\n");
#endif /* BUILD_WITH_SNMP */

#if BUILD_WITH_SIMPLE_ENERGEST
  simple_energest_init();
#endif /* BUILD_WITH_SIMPLE_ENERGEST */

#if BUILD_WITH_TSCH_CS
  /* Initialize the channel selection module */
  tsch_cs_adaptations_init();
#endif /* BUILD_WITH_TSCH_CS */

  /* Register the monitored flows before anything can produce a checkpoint,
   * so no cycle starts from an unregistered id. Registration failure means
   * KERN_CONF_FLOW_MAX_TRACKS is too small; both flows below then simply do
   * not run and kern_flow_track_count() reports zero. */
  app_loop_flow = kern_flow_register("app-loop", APP_LOOP_MASK,
                                     APP_LOOP_BUDGET);
  app_heartbeat_flow = kern_flow_register("app-heartbeat", KERN_FLOW_MASK(1),
                                          APP_HEARTBEAT_BUDGET);

  autostart_start(autostart_processes);

  WEDGE_ARM();
  watchdog_start();

#if PLATFORM_PROVIDES_MAIN_LOOP
  platform_main_loop();
#else
  while(1) {
    process_num_events_t r;

    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_LOOP_BEGIN);
    WEDGE_TOUCH(0);

    do {
      r = process_run();
      /* Fed inside the burst as well, so that a long event chain cannot
       * starve the counter. The sequence check below is what keeps this from
       * degenerating into "something is alive somewhere". */
      watchdog_periodic();
      WEDGE_TOUCH(7);
    } while(r > 0);

    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_BURST_DONE);
    WEDGE_TOUCH(1);

    /*
     * Kernel integrity is checked once per idle pass, outside of the
     * event burst, so that a corrupted list is reported before it can
     * affect the next scheduling round.
     */
    if(kern_guard_check() != KERN_OK) {
      APP_ErrorHandler("kern_guard_check");
    }
    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_GUARD_OK);
    WEDGE_TOUCH(2);

    if(etimer_check() != KERN_OK) {
      APP_ErrorHandler("etimer_check");
    }
    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_TIMER_OK);
    WEDGE_TOUCH(3);

    if(kern_stack_check() == KERN_ERR_STACK) {
      APP_ErrorHandler("kern_stack_check");
    }
    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_STACK_OK);
    WEDGE_TOUCH(4);

    /* Closes a window roughly every QCLK_CONF_WINDOW_TICKS reference
     * counts and only then compares; most passes do nothing but read
     * the reference counter. */
    if(qclk_poll() == KERN_ERR_CLOCK) {
      APP_ErrorHandler("qclk_poll");
    }
    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_CLOCK_OK);
    WEDGE_TOUCH(5);

    /* Refresh only after every check above has been reached, in order. A
     * branch that bypasses one of them closes no cycle here, so the counter
     * stops being refreshed by this path and the watchdog takes the unit to
     * reset rather than leaving it running without that check. */
    if(kern_flow_check() != KERN_OK) {
      APP_ErrorHandler("kern_flow_check");
    }
    watchdog_periodic();
    (void)kern_flow_checkpoint(app_loop_flow, APP_CP_FEED);
    WEDGE_TOUCH(6);

    platform_idle();
  }
#endif

  // while (1)
  // {
  //   /* Delay 250ms */
  //   HAL_Delay(250);   

  //   /* LED toggle */
  //   HAL_GPIO_TogglePin(LED_GPIO_PORT, LED_GPIO_PIN);    
  // }
  return 0;
}

/**
  * @brief  GPIO configuration.
  * @param  None
  * @retval None
  */
static void APP_GpioConfig(void)
{
  GPIO_InitTypeDef  GPIO_InitStruct = {0};

  LED_GPIO_CLK_ENABLE();                                 /* Enable GPIO clock */

  GPIO_InitStruct.Pin = LED_GPIO_PIN;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;            /* Push-pull output */
  GPIO_InitStruct.Pull = GPIO_PULLUP;                    /* Enable pull-up */
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;          /* GPIO speed */
  /* GPIO initialization */
  HAL_GPIO_Init(LED_GPIO_PORT, &GPIO_InitStruct);                
}

/**
  * @brief  Error executing function.
  * @param  None
  * @retval None
  */
extern void shellport_emergency_write(const char *s);

/* Direct, HAL-independent dying gasp. The stock shellport_emergency_write()
 * routes through HAL_UART_Transmit(), which returns HAL_BUSY and emits NO
 * bytes if huart2.gState == BUSY_TX -- and that is exactly why the "ERR: "
 * line was silently dropped on the spontaneous reset. Here we bit-bang the
 * string straight into USART2 (poll TXE, stuff DR) and then wait for TC so
 * every byte is physically shifted out BEFORE the reset pulls the rug. A short
 * spin after TC lets the host's USB-UART bridge drain its internal buffer.
 * This is the definitive root-cause channel: the failing check name lands on
 * the wire regardless of Flash state. */
static void
emergency_uart_gasp(const char *s)
{
  USART_TypeDef *u = USART2;
  const char *p = s;
  volatile uint32_t d;
  if (s == NULL) {
    return;
  }
  while (*p != '\0') {
    while ((u->SR & USART_SR_TXE) == 0U) { }
    u->DR = (uint8_t)(*p++);
  }
  while ((u->SR & USART_SR_TC) == 0U) { }
  /* Hold the line open for ~1.5 s so the host CANNOT drop the reason at the
   * reset boundary (the capture tool garbles the bytes right at a reset, see the
   * "garbage" prefix on every reboot line). Kick IWDG periodically so it does
   * not fire and masquerade as the cause; the explicit NVIC_SystemReset() below
   * keeps the classification as "SFT". */
  for (d = 0U; d < 3000000UL; d++) {
    if ((d & 0x3FFFFU) == 0U) {
      IWDG->KR = 0xAAAAU;   /* reload IWDG counter */
    }
  }
}

void APP_ErrorHandler(const char *reason)
{
  /* Dying gasp FIRST, before any Flash access: the raw Flash writer polls BSY
   * with no timeout, so a wedged flash controller would otherwise block this
   * print. The check name on the wire is the whole point. */
  emergency_uart_gasp("ERR: ");
  if (reason != NULL) {
    emergency_uart_gasp(reason);
  }
  emergency_uart_gasp("\r\n");
  NVIC_SystemReset();
  while (1)
  {
  }
}


#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* Users can add their own printing information as needed,
     for example: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* Infinite loop */
  while (1)
  {
  }
}
#endif /* USE_FULL_ASSERT */

/************************ (C) COPYRIGHT Puya *****END OF FILE******************/

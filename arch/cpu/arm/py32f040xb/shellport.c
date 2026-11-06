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

/**
 * \addtogroup shellport
 * @{
 */

#include "contiki.h"
#include "py32f0xx_hal.h"

#include "../../../../os/services/shell/quilla-shell.h"
#include "lpm.h"
#include "shellport.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Pin / peripheral assignment: USART2 on PA2/PA3, GPIO_AF1_USART2.       */
#define SHELLPORT_USART          USART2
#define SHELLPORT_GPIO_PORT      GPIOA
#define SHELLPORT_TX_PIN         GPIO_PIN_2
#define SHELLPORT_RX_PIN         GPIO_PIN_3
#define SHELLPORT_TX_AF          GPIO_AF1_USART2
#define SHELLPORT_RX_AF          GPIO_AF1_USART2
#define SHELLPORT_USART_CLK()      __HAL_RCC_USART2_CLK_ENABLE()

/* DMA: USART2_RX routes to DMA1_Channel6. Channels 1=SPI, 2/3=USART1,
 * 4/5=I2C are taken by other peripherals, so 6/7 are free; we only need RX. */
#define SHELLPORT_DMA_RX_CH      DMA1_Channel6
#define SHELLPORT_DMA_RX_MAP     DMA_CHANNEL_MAP_USART2_RD

#define SHELLPORT_USART_IRQn       USART2_IRQn
#define SHELLPORT_DMA_IRQn         DMA1_Channel4_5_6_7_IRQn
#define SHELLPORT_DMA_HT_FLAG      DMA_FLAG_HT6
#define SHELLPORT_DMA_TC_FLAG      DMA_FLAG_TC6
#define SHELLPORT_DMA_GL_FLAG      DMA_FLAG_GL6
#define SHELLPORT_USART_IRQ        USART2_IRQHandler
#define SHELLPORT_DMA_IRQ          DMA1_Channel4_5_6_7_IRQHandler

/* Circular RX buffer filled by a CIRCULAR-mode DMA. The DMA runs continuously
 * and never re-arms: it just wraps around this ring forever. Reception is
 * harvested in the shell thread (see shellport_rx_harvest), which copies every
 * new byte into the shell ring / OTA sink. Because the DMA is never stopped
 * mid-frame, there is no re-arm race and no "DMA silently stopped at a buffer
 * boundary" stall -- the two bug classes the old NORMAL ping-pong design had on
 * PY32 (the HAL itself notes clearing ORE is "mandatory for the second
 * transfer", i.e. re-arming NORMAL DMA is a known fragile point). */
#define SHELLPORT_RX_RING_SZ     1024U  /* power of two; must exceed the bytes a
                                          * full flash-write window can deliver at
                                          * the link rate (30 ms @115200 ~= 345 B)
                                          * so the CIRCULAR DMA never overwrites
                                          * un-harvested bytes mid-upgrade. */

/* ------------------------------------------------------------------ */
static UART_HandleTypeDef shell_uart;
static DMA_HandleTypeDef  hdma_shell_rx;

/* RX ring written by the DMA. rx_dma_tail is the next index to consume; it is
 * owned exclusively by the shell thread (shellport_rx_harvest), never by an
 * ISR, so there is no re-entrancy race on it. */
static uint8_t  rx_ring[SHELLPORT_RX_RING_SZ];
static volatile uint16_t rx_dma_tail;


/* Reference the shell process defined in quilla-shell.c. */
PROCESS_NAME(shell_process);

#ifdef QBOOT_BUILD
/* QIAP-only: when set, received bytes are handed to this sink (the OTA parser)
 * instead of the shell line buffer. NULL restores normal shell input. */
static void (*iap_raw_sink)(uint8_t) = NULL;
#endif

/* ------------------------------------------------------------------ */
/* MSP: clocks, GPIO AF, DMA channel, NVIC. This is a STRONG override of the
 * weak default provided by the HAL. It only configures SHELLPORT_USART
 * (USART2 on the reference board). */
void
HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
  GPIO_InitTypeDef gpio = {0};

  if(huart->Instance != SHELLPORT_USART) {
    return;
  }

  SHELLPORT_USART_CLK();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_RCC_DMA_CLK_ENABLE();

  /* TX (PA2) */
  gpio.Pin       = SHELLPORT_TX_PIN;
  gpio.Mode      = GPIO_MODE_AF_PP;
  gpio.Pull      = GPIO_PULLUP;
  gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = SHELLPORT_TX_AF;
  HAL_GPIO_Init(SHELLPORT_GPIO_PORT, &gpio);

  /* RX (PA3). Pull-up is mandatory here: the ring DMA runs forever and never
   * stops, so a floating RX line would sample noise glitches as fake start
   * bits, generating a continuous byte stream. That stream pins the shell
   * drain loop (which runs in a cooperative process with no yield) and starves
   * the idle-loop watchdog feed -> a deterministic IWDG reset the moment any
   * input is harvested. A clean idle line (pulled high) produces no bytes. */
  gpio.Pin       = SHELLPORT_RX_PIN;
  gpio.Mode      = GPIO_MODE_AF_OD;
  gpio.Pull      = GPIO_PULLUP;
  gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = SHELLPORT_RX_AF;
  HAL_GPIO_Init(SHELLPORT_GPIO_PORT, &gpio);

  /* DMA RX channel */
  hdma_shell_rx.Instance                 = SHELLPORT_DMA_RX_CH;
  hdma_shell_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
  hdma_shell_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_shell_rx.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_shell_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
  hdma_shell_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
  hdma_shell_rx.Init.Mode                = DMA_CIRCULAR;
  hdma_shell_rx.Init.Priority            = DMA_PRIORITY_HIGH;
  if(HAL_DMA_Init(&hdma_shell_rx) != HAL_OK) {
    return;
  }
  __HAL_LINKDMA(huart, hdmarx, hdma_shell_rx);
  HAL_DMA_ChannelMap(&hdma_shell_rx, SHELLPORT_DMA_RX_MAP);

  HAL_NVIC_SetPriority(SHELLPORT_USART_IRQn, 8, 0);
  HAL_NVIC_EnableIRQ(SHELLPORT_USART_IRQn);
  HAL_NVIC_SetPriority(SHELLPORT_DMA_IRQn, 9, 0);
  HAL_NVIC_EnableIRQ(SHELLPORT_DMA_IRQn);
}

/* ------------------------------------------------------------------ */
/* Push received bytes toward their destination: the shell line buffer, or the
 * QIAP OTA sink when one is armed (QBOOT upgrade phase). */
static void
shellport_rx_feed(const uint8_t *d, uint16_t n)
{
  uint16_t i;
  if(n == 0U) {
    return;
  }
  for(i = 0U; i < n; i++) {
#ifdef QBOOT_BUILD
    if(iap_raw_sink != NULL) {
      iap_raw_sink(d[i]);
      continue;
    }
#endif
    shell_input(&d[i], 1U);
  }
}

/* Harvest every byte the DMA has written since the last harvest. The DMA head is
 * derived from the live CNDTR counter, so this is correct no matter how many
 * HTF/IDLE/TC events fired in between -- we never assume a frame starts at a
 * ring half-boundary (which is exactly why both HTF and FTF are honoured: with
 * only one of them a command could start from the stale second half). Runs only
 * in the shell thread (never an ISR): the ISRs just poll the shell process, so
 * rx_dma_tail is touched by a single context -- no re-entrancy race. */
void
shellport_rx_harvest(void)
{
  /* Normalize head into [0, SZ): CNDTR == 0 means the DMA just wrapped, which
   * would otherwise yield head == SZ and an out-of-range tail / read. */
  uint16_t head = ((SHELLPORT_RX_RING_SZ -
                    (uint16_t)__HAL_DMA_GET_COUNTER(&hdma_shell_rx))
                   & (SHELLPORT_RX_RING_SZ - 1U));
  uint16_t tail = rx_dma_tail & (SHELLPORT_RX_RING_SZ - 1U);
  uint16_t avail;

  /* Bytes written since the last harvest, ring-wrap aware. */
  if(head >= tail) {
    avail = head - tail;
  } else {
    avail = SHELLPORT_RX_RING_SZ - tail + head;
  }
  if(avail == 0U) {
    return;   /* nothing new */
  }

  /* Only drop when a whole lap (or more) was lost -- i.e. the DMA genuinely
   * overran the consumer by >= one full ring, which is unrecoverable anyway.
   * We must NOT drop on a smaller backlog: a legitimate upgrade burst (the host
   * paces one frame in flight, but the harvest/pop cadence can momentarily lag)
   * would otherwise have real frame bytes thrown away, desyncing the QIAP
   * parser. With the 1024-byte ring and the upgrade loop draining its parser
   * ring every pass, a sub-lap backlog never occurs in practice. */
  if(avail >= SHELLPORT_RX_RING_SZ) {
    tail = head;
    avail = 0U;
  }

  /* Deliver in up to two contiguous runs to respect the ring wrap. */
  if(head >= tail) {
    shellport_rx_feed(&rx_ring[tail], avail);
  } else {
    uint16_t first = SHELLPORT_RX_RING_SZ - tail;
    shellport_rx_feed(&rx_ring[tail], first);
    shellport_rx_feed(&rx_ring[0], (uint16_t)(avail - first));
  }
  rx_dma_tail = (tail + avail) & (SHELLPORT_RX_RING_SZ - 1U);
}

/* Restart circular reception after a DMA transfer error. Resets the consumer
 * tail to the current DMA head so we never replay bytes across the fault. */
static void
shellport_rx_restart(void)
{
  HAL_UART_DMAStop(&shell_uart);
  (void)HAL_UART_Receive_DMA(&shell_uart, rx_ring, SHELLPORT_RX_RING_SZ);
  hdma_shell_rx.Instance->CCR |= (DMA_CCR_CIRC | DMA_CCR_HTIE);
  rx_dma_tail = ((SHELLPORT_RX_RING_SZ -
                  (uint16_t)__HAL_DMA_GET_COUNTER(&hdma_shell_rx))
                 & (SHELLPORT_RX_RING_SZ - 1U));
}

/* ------------------------------------------------------------------ */
/* Vector names differ between the two wirings (USART2 vs USART1 and their
 * DMA channel groups), so the entry points are selected by the same
 * compile-time switch as the pin/peripheral block above. */
void
SHELLPORT_USART_IRQ(void)
{
  /* Clear every latched UART flag (IDLE/ORE/PE/FE/NE) by reading SR then DR.
   * HAL_UART_Receive_DMA enabled the error interrupt (EIE), so any of these can
   * raise this vector; if one is left latched the vector re-enters forever. In
   * the old NORMAL-DMA design the per-frame re-arm happened to clear ORE, but in
   * circular mode nothing re-arms, so we must clear it here unconditionally. */
  uint32_t was_idle = (__HAL_UART_GET_FLAG(&shell_uart, UART_FLAG_IDLE) != RESET)
                      ? 1U : 0U;
  __HAL_UART_CLEAR_IDLEFLAG(&shell_uart);
  if(was_idle != 0U) {
    /* IDLE is the frame delimiter: wake the shell thread to harvest. Do not
     * touch the DMA global flag here -- the DMA runs continuously in circular
     * mode and owns its own completion interrupts (HTF/FTF below). */
    process_poll(&shell_process);
  }
}

void
SHELLPORT_DMA_IRQ(void)
{
  /* Shared DMA vector: channel 6 (USART2_RX) on the reference board.
   * HTF (half full) and FTF/TF (full) both mean "a slice of the ring is now
   * safe to read" -- shellport_rx_harvest() reads up to the live DMA head
   * regardless, so the command start never lands on a stale half-buffer. We act
   * only on our own channel and never clear sibling channels, so another DMA
   * user on the same vector keeps working through its own handler. */
  uint32_t isr = DMA1->ISR;
  if((isr & (SHELLPORT_DMA_HT_FLAG | SHELLPORT_DMA_TC_FLAG)) != 0U) {
    DMA1->IFCR = SHELLPORT_DMA_GL_FLAG;  /* clears HT/TC/TE/GL for our channel */
    process_poll(&shell_process);
  } else if((isr & SHELLPORT_DMA_GL_FLAG) != 0U) {
    /* A non-transfer flag for our channel (e.g. transfer error) latched:
     * clear it and restart circular RX instead of spinning. */
    DMA1->IFCR = SHELLPORT_DMA_GL_FLAG;
    shellport_rx_restart();
  }
}

/* ------------------------------------------------------------------ */
/* LPM gate: the console refuses Stop.
 *
 * Stop gates every clock in the VCORE domain, the console USART and its DMA included,
 * so a byte arriving while the core is stopped is not received at all. It
 * does not wake the core either: this port has no wake-up source owned by
 * the console, and the device header exposes neither a wake-up-pin enable
 * register in PWR nor a wake-up-from-Stop selection in USART_CR3.
 *
 * PM1 is therefore refused for as long as the console runs, which is the
 * honest answer for a unit with a host attached - a host can speak at any
 * time, and there is nothing to predict that from. An image that is
 * deployed with no host on the wire can set SHELLPORT_CONF_PERMIT_PM1 to 1
 * and take Stop back, accepting that a byte sent across a Stop is lost.
 */
#if LPM_CONF_ENABLE && !defined(QBOOT_BUILD)
#ifndef SHELLPORT_CONF_PERMIT_PM1
#define SHELLPORT_CONF_PERMIT_PM1        0
#endif

static bool
shellport_permit_pm1(void)
{
#if SHELLPORT_CONF_PERMIT_PM1
  return true;
#else
  return false;
#endif
}
#endif /* LPM_CONF_ENABLE && !defined(QBOOT_BUILD) */

/* ------------------------------------------------------------------ */
void
shellport_write(const char *s)
{
  if(s == NULL) {
    return;
  }
  /* NOTE: a blocking transmit with HAL_MAX_DELAY is unsafe here. This function
   * runs from the main loop (shell commands execute in the shell process, which
   * the idle loop schedules via process_run()). If TXE ever stays clear for the
   * length of one byte, HAL_UART_Transmit would spin forever, freezing the only
   * path that feeds the watchdog -> an IWDG reset ~512 ms later. Bound every
   * call so a stalled UART can never wedge the whole firmware. 200 ms is 4x the
   * time a full help screen needs at 115200 baud, so a healthy link never hits
   * it; a broken one returns and the line is dropped instead of hanging the box. */
  (void)HAL_UART_Transmit(&shell_uart, (uint8_t *)s,
                          (uint16_t)strlen(s), 200U);
}

void
shellport_emergency_write(const char *s)
{
  uint16_t n = 0U;
  if(s == NULL) {
    return;
  }
  while(s[n] != '\0') {
    n++;
  }
  /* Polling transmit; returns HAL_BUSY if a shell write is mid-flight, which
   * silently drops the dying-gasp line. Acceptable: the probe fires in the
   * idle window, not while output is streaming. */
  (void)HAL_UART_Transmit(&shell_uart, (uint8_t *)s, n, 100U);
}


/* ------------------------------------------------------------------ */
#ifdef QBOOT_BUILD
void
shellport_set_raw_sink(void (*sink)(uint8_t b))
{
  iap_raw_sink = sink;
}

void
shellport_write_raw(const uint8_t *data, uint16_t len)
{
  if(data == NULL || len == 0U) {
    return;
  }
  (void)HAL_UART_Transmit(&shell_uart, (uint8_t *)data, len, 200U);
}
#endif

/* ------------------------------------------------------------------ */
int
shellport_init(void)
{
  if(shell_init() != 0) {
    return -1;
  }
  shell_set_writer(shellport_write);

  shell_uart.Instance                     = SHELLPORT_USART;
  shell_uart.Init.BaudRate                = SHELLPORT_BAUDRATE;
  shell_uart.Init.WordLength              = UART_WORDLENGTH_8B;
  shell_uart.Init.StopBits                = UART_STOPBITS_1;
  shell_uart.Init.Parity                  = UART_PARITY_NONE;
  shell_uart.Init.HwFlowCtl               = UART_HWCONTROL_NONE;
  shell_uart.Init.Mode                    = UART_MODE_TX_RX;
  shell_uart.Init.OverSampling            = UART_OVERSAMPLING_16;
  shell_uart.AdvancedInit.AdvFeatureInit  = UART_ADVFEATURE_NO_INIT;

  if(HAL_UART_Init(&shell_uart) != HAL_OK) {
    return -1;
  }

  /* Start the shell process first, then begin DMA reception. */
  process_start(&shell_process, NULL);

  rx_dma_tail = 0U;
  if(HAL_UART_Receive_DMA(&shell_uart, rx_ring, SHELLPORT_RX_RING_SZ) != HAL_OK) {
    return -1;
  }
  /* Force CIRCULAR + Half-Transfer interrupt on top of whatever HAL configured
   * (belt-and-suspenders: Init.Mode is already DMA_CIRCULAR in MspInit). HTF is
   * what lets us drain the ring during a sustained burst so it never overflows;
   * FTF (TC) is enabled by HAL because a half-complete callback is registered. */
  hdma_shell_rx.Instance->CCR |= (DMA_CCR_CIRC | DMA_CCR_HTIE);
  __HAL_UART_ENABLE_IT(&shell_uart, UART_IT_IDLE);

#if LPM_CONF_ENABLE && !defined(QBOOT_BUILD)
  lpm_register_peripheral(shellport_permit_pm1);
#endif

  /* No banner here on purpose. The shell does not announce itself, and it does
   * not print the version: main() owns the boot stamp ("[QAPP] <name> V<ver>"
   * / "[BOOT] Quilla V<ver>"), which is what identifies a running image on the
   * wire. A single newline just clears the line after any bootloader noise. */
  shell_puts("\r\n");
  return 0;
}
/** @} */

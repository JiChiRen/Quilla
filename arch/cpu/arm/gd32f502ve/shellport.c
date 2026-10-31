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
 *    notice, this list of the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
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
 * \addtogroup shellport
 * @{
 */

#include "contiki.h"

#include "../../../../os/services/shell/quilla-shell.h"
#include "shellport.h"

#include "gd32f50x.h"

/* ------------------------------------------------------------------ */
/* Pin / peripheral assignment: USART0 on PB6 (TX) / PB7 (RX), AF0.
 *
 * This is the reference board's own "TRANS_COM0" wiring, which is why the DMA
 * set-up below mirrors that project: same USART, same pins, same AF number.
 *
 * USART0 lives on APB2, which runs at the full 160 MHz system clock; that is
 * what usart_baudrate_set() divides to reach the bit rate. */
#define SHELLPORT_USART          USART0
#define SHELLPORT_USART_CLK      RCU_USART0
#define SHELLPORT_USART_IRQn     USART0_IRQn
#define SHELLPORT_GPIO_PORT      GPIOB
#define SHELLPORT_GPIO_CLK       RCU_GPIOB
#define SHELLPORT_TX_PIN         GPIO_PIN_6
#define SHELLPORT_RX_PIN         GPIO_PIN_7
#define SHELLPORT_GPIO_AF        GPIO_AF_0

/* Address of USART_DATA (offset 0x04 in every USARTx block), i.e. the source
 * the DMA reads. The vendor library only exposes it as a dereference macro. */
#define SHELLPORT_USART_DATA_ADDR ((uint32_t)(USART0 + 0x04U))

/* DMA RX: DMA0 channel 4, routed through DMAMUX request 20
 * (DMA_REQUEST_USART0_RX). GD32F5 has no fixed channel-peripheral map: each
 * channel selects its source through a DMAMUX id, so any free channel would
 * do. Channel 4 (the reference board's choice for USART0 RX) keeps both
 * projects comparable. */
#define SHELLPORT_DMA            DMA0
#define SHELLPORT_DMA_CLK        RCU_DMA0
#define SHELLPORT_DMAMUX_CLK     RCU_DMAMUX
#define SHELLPORT_DMA_RX_CH      DMA_CH4
#define SHELLPORT_DMA_RX_REQ     DMA_REQUEST_USART0_RX
#define SHELLPORT_DMA_RX_MUXCH   DMAMUX_MUXCH4
#define SHELLPORT_DMA_RX_IRQn    DMA0_Channel4_IRQn

/* Console priority: below the rtimer (which owns the time base), above
 * SysTick. The two console vectors are kept at the same priority so that, if
 * they ever touch shared state, an interrupt never preempts one of its own
 * kind. With the circular-DMA design below the ISRs only poll the shell
 * process, so this is mostly for tidy nesting. */
#define SHELLPORT_IRQ_PRIO       6U

/* ------------------------------------------------------------------ */
/* Reference the shell process defined in quilla-shell.c. */
PROCESS_NAME(shell_process);

/* CIRCULAR RX ring filled by a CIRCULAR-mode DMA0 channel 4 (request 20,
 * DMA_REQUEST_USART0_RX). The DMA runs forever and never re-arms: it just
 * wraps around this ring. Reception is harvested in the shell process via
 * shellport_rx_harvest(), which reads the live transfer counter and copies
 * every new byte into the shell ring / OTA sink. Because the DMA is never
 * stopped mid-frame there is no re-arm race and no "DMA silently stopped at a
 * buffer boundary" stall -- the two bug classes the old NORMAL ping-pong
 * design had on first GD32 silicon. Size is a power of two and must exceed the
 * bytes a sustained flash-write burst can deliver at the link rate so the
 * CIRCULAR DMA never overwrites un-harvested bytes mid-upgrade. */
#define SHELLPORT_RX_RING_SZ  512U

static uint8_t rx_ring[SHELLPORT_RX_RING_SZ];
static volatile uint16_t rx_dma_tail;

#ifdef QBOOT_BUILD
/* QIAP-only: when set, received bytes are handed to this sink (the OTA parser)
 * instead of the shell line buffer. NULL restores normal shell input. */
static void (*iap_raw_sink)(uint8_t) = NULL;
#endif

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

/* Harvest every byte the DMA has written since the last harvest. The DMA head
 * is derived from the live transfer counter, so this is correct no matter how
 * many HTF / IDLE / FTF events fired in between -- we never assume a frame
 * starts at a ring half-boundary. Runs only in the shell thread (never an
 * ISR): the ISRs just poll the shell process, so rx_dma_tail is touched by a
 * single context -- no re-entrancy race. */
void
shellport_rx_harvest(void)
{
  /* Normalize head into [0, SZ): a counter of 0 means the DMA just wrapped,
   * which would otherwise yield head == SZ and an out-of-range tail / read. */
  uint16_t head = ((SHELLPORT_RX_RING_SZ -
                    (uint16_t)dma_transfer_number_get(SHELLPORT_DMA,
                                                      SHELLPORT_DMA_RX_CH))
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
   * A sub-lap backlog never occurs in practice: the shell drain / QIAP loop
   * runs every pass and the ring is sized for the worst-case burst. */
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
  rx_dma_tail = (uint16_t)((tail + avail) & (SHELLPORT_RX_RING_SZ - 1U));
}

void
USART0_IRQHandler(void)
{
  /* IDLE is the frame delimiter. Clear it (read STAT0, then read DATA per the
   * RM) and wake the shell process so shellport_rx_harvest() picks up the
   * bytes. We do NOT touch the DMA here: it runs continuously in circular
   * mode and owns its own completion interrupts. */
  if(usart_interrupt_flag_get(SHELLPORT_USART, USART_INT_FLAG_IDLE) != RESET) {
    (void)USART_DATA(SHELLPORT_USART);   /* dummy read clears IDLEF/RBNE/ORERR */
    process_poll(&shell_process);
  }
}

void
DMA0_Channel4_IRQHandler(void)
{
  /* HTF (half full) and FTF (full / wrap) both mean "a slice of the ring is
   * now safe to read"; the harvest reads up to the live DMA head regardless.
   * ERR (e.g. a transfer error) just wakes the harvest too. Clearing the lot
   * together is harmless. */
  if((dma_interrupt_flag_get(SHELLPORT_DMA, SHELLPORT_DMA_RX_CH, DMA_INT_FLAG_HTF)
      != RESET) ||
     (dma_interrupt_flag_get(SHELLPORT_DMA, SHELLPORT_DMA_RX_CH, DMA_INT_FLAG_FTF)
      != RESET) ||
     (dma_interrupt_flag_get(SHELLPORT_DMA, SHELLPORT_DMA_RX_CH, DMA_INT_FLAG_ERR)
      != RESET)) {
    dma_interrupt_flag_clear(SHELLPORT_DMA, SHELLPORT_DMA_RX_CH,
                             DMA_INT_FLAG_HTF | DMA_INT_FLAG_FTF |
                             DMA_INT_FLAG_ERR);
    process_poll(&shell_process);
  }
}

/* ------------------------------------------------------------------ */
/* shellport_rx_harvest() is the real RX worker; see above. The console write
 * (shellport_write) is the only thing the shell core needs from interrupt
 * context, and transmission is polling (see below). */
void
shellport_write(const char *s)
{
  const char *p = s;

  if(p == NULL) {
    return;
  }
  while(*p != '\0') {
    usart_data_transmit(SHELLPORT_USART, (uint32_t)*p);
    while(RESET == usart_flag_get(SHELLPORT_USART, USART_FLAG_TBE)) {
      /* spin until the byte has been handed to the shift register */
    }
    p++;
  }
}

/* Why TX is still polling when RX is DMA'd.
 *
 * The only caller that cares is the QIAP: the very last thing the QBOOT does
 * before it branches to the APP is emit the final frame, then hand over. With
 * a DMA TX the last transfer is still in flight at that point - up to a whole
 * frame of it - and anything that stops the USART or the channel before it
 * completes silently truncates the reply the host is waiting for. Polling
 * leaves at most one byte in the DATA register, which the RM lets us flush
 * explicitly. Until QIAP grows an explicit "wait for TX idle" step with a real
 * bound on it, polling is the honest answer; the reference design reaches the
 * same conclusion and disables TX DMA too. */
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
  uint16_t i;

  if(data == NULL) {
    return;
  }
  for(i = 0U; i < len; i++) {
    usart_data_transmit(SHELLPORT_USART, (uint32_t)data[i]);
    while(RESET == usart_flag_get(SHELLPORT_USART, USART_FLAG_TBE)) {
      /* spin */
    }
  }
}
#endif

/* ------------------------------------------------------------------ */
int
shellport_init(void)
{
  dma_parameter_struct dma_cfg;

  if(shell_init() != 0) {
    return -1;
  }
  shell_set_writer(shellport_write);

  rcu_periph_clock_enable(SHELLPORT_GPIO_CLK);
  rcu_periph_clock_enable(SHELLPORT_USART_CLK);
  rcu_periph_clock_enable(SHELLPORT_DMA_CLK);
  rcu_periph_clock_enable(SHELLPORT_DMAMUX_CLK);

  gpio_af_set(SHELLPORT_GPIO_PORT, SHELLPORT_GPIO_AF,
              SHELLPORT_TX_PIN | SHELLPORT_RX_PIN);
  gpio_mode_set(SHELLPORT_GPIO_PORT, GPIO_MODE_AF, GPIO_PUPD_PULLUP,
                SHELLPORT_TX_PIN | SHELLPORT_RX_PIN);
  gpio_output_options_set(SHELLPORT_GPIO_PORT, GPIO_OTYPE_PP,
                          GPIO_OSPEED_LEVEL3,
                          SHELLPORT_TX_PIN | SHELLPORT_RX_PIN);

  usart_deinit(SHELLPORT_USART);
  usart_baudrate_set(SHELLPORT_USART, SHELLPORT_BAUDRATE);
  usart_word_length_set(SHELLPORT_USART, USART_WL_8BIT);
  usart_stop_bit_set(SHELLPORT_USART, USART_STB_1BIT);
  usart_parity_config(SHELLPORT_USART, USART_PM_NONE);
  usart_receive_config(SHELLPORT_USART, USART_RECEIVE_ENABLE);
  usart_transmit_config(SHELLPORT_USART, USART_TRANSMIT_ENABLE);
  usart_enable(SHELLPORT_USART);

  /* DMA0 CH4: USART0 RX, peripheral-to-memory, 8 bit, CIRCULAR. The ring is
   * filled forever; shellport_rx_harvest() (called from the shell thread)
   * reads the live transfer counter and copies new bytes out. DMAMUX request
   * 20 (USART0_RX) is selected inside dma_init; the DMAMUX synchronization
   * mode must be switched off explicitly or the channel waits for a sync event
   * that never comes. */
  dma_deinit(SHELLPORT_DMA, SHELLPORT_DMA_RX_CH);
  dma_struct_para_init(&dma_cfg);
  dma_cfg.request      = SHELLPORT_DMA_RX_REQ;
  dma_cfg.periph_addr  = SHELLPORT_USART_DATA_ADDR;
  dma_cfg.periph_width = DMA_PERIPHERAL_WIDTH_8BIT;
  dma_cfg.periph_inc   = DMA_PERIPH_INCREASE_DISABLE;
  dma_cfg.memory_addr  = (uint32_t)rx_ring;
  dma_cfg.memory_width = DMA_MEMORY_WIDTH_8BIT;
  dma_cfg.memory_inc   = DMA_MEMORY_INCREASE_ENABLE;
  dma_cfg.number       = SHELLPORT_RX_RING_SZ;
  dma_cfg.priority     = DMA_PRIORITY_ULTRA_HIGH;
  dma_cfg.direction    = DMA_PERIPHERAL_TO_MEMORY;
  dma_cfg.circular_mode = DMA_CIRCULAR_MODE_ENABLE;
  dma_init(SHELLPORT_DMA, SHELLPORT_DMA_RX_CH, &dma_cfg);
  dma_memory_to_memory_disable(SHELLPORT_DMA, SHELLPORT_DMA_RX_CH);
  dmamux_synchronization_disable(SHELLPORT_DMA_RX_MUXCH);

  /* Start the shell process first, then arm reception. */
  process_start(&shell_process, NULL);
  rx_dma_tail = 0U;

  /* Drop anything latched before the channel was armed. DENR is off at this
   * point, so the data register is ours to read. */
  (void)USART_DATA(SHELLPORT_USART);
  usart_interrupt_flag_clear(SHELLPORT_USART, USART_INT_FLAG_IDLE);
  usart_flag_clear(SHELLPORT_USART, USART_FLAG_RBNE);

  dma_interrupt_flag_clear(SHELLPORT_DMA, SHELLPORT_DMA_RX_CH,
                           DMA_INT_FLAG_HTF | DMA_INT_FLAG_FTF | DMA_INT_FLAG_ERR);
  dma_interrupt_enable(SHELLPORT_DMA, SHELLPORT_DMA_RX_CH,
                       DMA_INT_HTF | DMA_INT_FTF | DMA_INT_ERR);

  NVIC_SetPriority(SHELLPORT_USART_IRQn, SHELLPORT_IRQ_PRIO);
  NVIC_SetPriority(SHELLPORT_DMA_RX_IRQn, SHELLPORT_IRQ_PRIO);
  NVIC_ClearPendingIRQ(SHELLPORT_USART_IRQn);
  NVIC_ClearPendingIRQ(SHELLPORT_DMA_RX_IRQn);
  NVIC_EnableIRQ(SHELLPORT_DMA_RX_IRQn);

  /* Arm the circular DMA, then enable USART RX DMA requests, then unmask
   * IDLE. Order matters: the DMA must be live before USART starts feeding it. */
  dma_channel_enable(SHELLPORT_DMA, SHELLPORT_DMA_RX_CH);
  usart_dma_receive_config(SHELLPORT_USART, USART_DENR_ENABLE);

  usart_interrupt_enable(SHELLPORT_USART, USART_INT_IDLE);
  NVIC_EnableIRQ(SHELLPORT_USART_IRQn);

  /* The shell itself prints no version and does not announce "ready" -- the
   * application banner ("[QAPP] <name> V..." / "[BOOT] Quilla V...") is the
   * only place the build version appears on the wire. Keep this to a bare
   * newline so the prompt follows that banner cleanly. */
  shell_puts("\r\n");
  return 0;
}
/** @} */

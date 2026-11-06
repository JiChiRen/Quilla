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

#include "../../../../os/services/shell/quilla-shell.h"
#include "shellport.h"

#include "ch32v30x.h"

/* ------------------------------------------------------------------ */
/* Pin / peripheral assignment: USART2 on PA2 (TX) / PA3 (RX).
 *
 * USART2 lives on APB1 (48 MHz with the 96 MHz system clock, APB1 = HCLK/2);
 * USART_BaudRate divides that to reach the bit rate. CH32V30x USART pins have
 * a fixed alternate function, so there is no gpio_af_set() step: TX is
 * AF push-pull, RX is floating input (the WCH EVT convention). */
#define SHELLPORT_USART          USART2
#define SHELLPORT_USART_CLK      RCC_APB1Periph_USART2
#define SHELLPORT_USART_IRQn     USART2_IRQn
#define SHELLPORT_GPIO_PORT      GPIOA
#define SHELLPORT_GPIO_CLK       RCC_APB2Periph_GPIOA
#define SHELLPORT_TX_PIN         GPIO_Pin_2
#define SHELLPORT_RX_PIN         GPIO_Pin_3

/* Address of USART2 DATAR (offset 0x04 in every USARTx block), i.e. the
 * source the DMA reads. The vendor library only exposes it as a struct
 * member, so take its address here. */
#define SHELLPORT_USART_DATA_ADDR ((uint32_t)(&(SHELLPORT_USART->DATAR)))

/* DMA RX: DMA1 channel 6, peripheral-to-memory, 8 bit, CIRCULAR. Any free
 * channel would do; channel 6 is chosen to keep the pin/channel pairing
 * documented in one place. CH32V30x DMA has no DMAMUX: a channel is bound to
 * its peripheral by the peripheral's own DMA request, selected with
 * USART_DMACmd(SHELLPORT_USART, USART_DMAReq_Rx, ...). */
#define SHELLPORT_DMA            DMA1
#define SHELLPORT_DMA_CLK        RCC_AHBPeriph_DMA1
#define SHELLPORT_DMA_RX_CH      DMA1_Channel6
#define SHELLPORT_DMA_RX_IRQn    DMA1_Channel6_IRQn
#define SHELLPORT_DMA_RX_TC_IT   DMA1_IT_TC6

/* Vector entry points. The startup table already lists every USART/DMA
 * vector; keeping the handler names here means a port change only touches
 * this macro block. There is no build-time port switch. */
#define SHELLPORT_USART_IRQ      USART2_IRQHandler
#define SHELLPORT_DMA_IRQ        DMA1_Channel6_IRQHandler

/* Console priority: below the rtimer (which owns the time base), above
 * SysTick. The two console vectors are kept at the same priority so they can
 * never preempt one of their own kind. */
#define SHELLPORT_IRQ_PRIO       6U

/* ------------------------------------------------------------------ */
/* Reference the shell process defined in quilla-shell.c. */
PROCESS_NAME(shell_process);

/* CIRCULAR RX ring filled by a CIRCULAR-mode DMA1 channel 6 (USART2 RX).
 * The DMA runs forever and never re-arms: it just wraps around this ring.
 * Reception is harvested in the shell process via shellport_rx_harvest(),
 * which reads the live transfer counter and copies every new byte into the
 * shell ring / OTA sink. Size is a power of two and must exceed the bytes a
 * sustained flash-write burst can deliver at the link rate. */
#define SHELLPORT_RX_RING_SZ  512U

static uint8_t rx_ring[SHELLPORT_RX_RING_SZ];
static volatile uint16_t rx_dma_tail;

#ifdef QBOOT_BUILD
/* QIAP-only: when set, received bytes are handed to this sink (the OTA parser)
 * instead of the shell line buffer. NULL restores normal shell input. */
static void (*iap_raw_sink)(uint8_t) = NULL;
#endif

/* ------------------------------------------------------------------ */
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

void
shellport_rx_harvest(void)
{
  /* Normalize head into [0, SZ): a counter of 0 means the DMA just wrapped. */
  uint16_t head = ((SHELLPORT_RX_RING_SZ -
                    (uint16_t)DMA_GetCurrDataCounter(SHELLPORT_DMA_RX_CH))
                   & (SHELLPORT_RX_RING_SZ - 1U));
  uint16_t tail = rx_dma_tail & (SHELLPORT_RX_RING_SZ - 1U);
  uint16_t avail;

  if(head >= tail) {
    avail = head - tail;
  } else {
    avail = SHELLPORT_RX_RING_SZ - tail + head;
  }
  if(avail == 0U) {
    return;
  }
  if(avail >= SHELLPORT_RX_RING_SZ) {
    tail = head;
    avail = 0U;
  }

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
SHELLPORT_USART_IRQ(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void
SHELLPORT_USART_IRQ(void)
{
  /* IDLE is the frame delimiter. Clear it (read DATAR per the RM) and wake the
   * shell process so shellport_rx_harvest() picks up the bytes. We do NOT
   * touch the DMA here: it runs continuously in circular mode. */
  if(USART_GetITStatus(SHELLPORT_USART, USART_IT_IDLE) != RESET) {
    (void)USART_ReceiveData(SHELLPORT_USART);   /* dummy read clears IDLE */
    process_poll(&shell_process);
  }
}

void
SHELLPORT_DMA_IRQ(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void
SHELLPORT_DMA_IRQ(void)
{
  /* Transfer-complete = the ring just wrapped. Only DMA_IT_TC is enabled on
   * this channel, so reaching here means a full lap: wake the harvest as a
   * backstop so a continuous burst cannot overrun un-harvested bytes. */
  DMA_ClearITPendingBit(SHELLPORT_DMA_RX_TC_IT);
  process_poll(&shell_process);
}

/* ------------------------------------------------------------------ */
void
shellport_write(const char *s)
{
  const char *p = s;

  if(p == NULL) {
    return;
  }
  while(*p != '\0') {
    USART_SendData(SHELLPORT_USART, (uint32_t)(uint8_t)*p);
    while(RESET == USART_GetFlagStatus(SHELLPORT_USART, USART_FLAG_TXE)) {
      /* spin until the byte has been handed to the shift register */
    }
    p++;
  }
  /* Wait for the last byte to leave before returning: the QBOOT branches to
   * the APP right after the final QIAP frame and the host is waiting for it. */
  while(RESET == USART_GetFlagStatus(SHELLPORT_USART, USART_FLAG_TC)) {
    /* spin */
  }
}

/* TX stays polling when RX is DMA'd: with a DMA TX the last transfer would
 * still be in flight when the QBOOT hands over, silently truncating the reply
 * the host waits for. Polling leaves at most one byte in the data register. */
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
    USART_SendData(SHELLPORT_USART, (uint32_t)data[i]);
    while(RESET == USART_GetFlagStatus(SHELLPORT_USART, USART_FLAG_TXE)) {
      /* spin */
    }
  }
  while(RESET == USART_GetFlagStatus(SHELLPORT_USART, USART_FLAG_TC)) {
    /* spin */
  }
}
#endif

/* ------------------------------------------------------------------ */
int
shellport_init(void)
{
  GPIO_InitTypeDef        gpio_cfg;
  USART_InitTypeDef       usart_cfg;
  DMA_InitTypeDef         dma_cfg;

  if(shell_init() != 0) {
    return -1;
  }
  shell_set_writer(shellport_write);

  RCC_APB2PeriphClockCmd(SHELLPORT_GPIO_CLK, ENABLE);
  RCC_APB1PeriphClockCmd(SHELLPORT_USART_CLK, ENABLE);
  RCC_AHBPeriphClockCmd(SHELLPORT_DMA_CLK, ENABLE);

  /* USART2 TX-->PA2 (AF push-pull), RX-->PA3 (floating input). */
  gpio_cfg.GPIO_Pin   = SHELLPORT_TX_PIN;
  gpio_cfg.GPIO_Speed = GPIO_Speed_50MHz;
  gpio_cfg.GPIO_Mode  = GPIO_Mode_AF_PP;
  GPIO_Init(SHELLPORT_GPIO_PORT, &gpio_cfg);
  gpio_cfg.GPIO_Pin  = SHELLPORT_RX_PIN;
  gpio_cfg.GPIO_Mode = GPIO_Mode_IN_FLOATING;
  GPIO_Init(SHELLPORT_GPIO_PORT, &gpio_cfg);

  USART_StructInit(&usart_cfg);
  usart_cfg.USART_BaudRate            = SHELLPORT_BAUDRATE;
  usart_cfg.USART_WordLength          = USART_WordLength_8b;
  usart_cfg.USART_StopBits            = USART_StopBits_1;
  usart_cfg.USART_Parity              = USART_Parity_No;
  usart_cfg.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
  usart_cfg.USART_Mode                = USART_Mode_Tx | USART_Mode_Rx;
  USART_Init(SHELLPORT_USART, &usart_cfg);
  USART_ITConfig(SHELLPORT_USART, USART_IT_IDLE, ENABLE);
  USART_Cmd(SHELLPORT_USART, ENABLE);

  /* DMA1 channel 6: USART2 RX, peripheral-to-memory, 8 bit, CIRCULAR. The
   * ring is filled forever; shellport_rx_harvest() reads the live transfer
   * counter and copies new bytes out. */
  DMA_DeInit(SHELLPORT_DMA_RX_CH);
  dma_cfg.DMA_PeripheralBaseAddr = SHELLPORT_USART_DATA_ADDR;
  dma_cfg.DMA_MemoryBaseAddr     = (uint32_t)rx_ring;
  dma_cfg.DMA_DIR                = DMA_DIR_PeripheralSRC;
  dma_cfg.DMA_BufferSize         = SHELLPORT_RX_RING_SZ;
  dma_cfg.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
  dma_cfg.DMA_MemoryInc          = DMA_MemoryInc_Enable;
  dma_cfg.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
  dma_cfg.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
  dma_cfg.DMA_Mode               = DMA_Mode_Circular;
  dma_cfg.DMA_Priority           = DMA_Priority_VeryHigh;
  dma_cfg.DMA_M2M                = DMA_M2M_Disable;
  DMA_Init(SHELLPORT_DMA_RX_CH, &dma_cfg);
  DMA_ITConfig(SHELLPORT_DMA_RX_CH, DMA_IT_TC, ENABLE);

  /* Start the shell process first, then arm reception. */
  process_start(&shell_process, NULL);
  rx_dma_tail = 0U;

  /* Drop anything latched before the channel was armed. */
  (void)USART_ReceiveData(SHELLPORT_USART);
  USART_ClearITPendingBit(SHELLPORT_USART, USART_IT_IDLE);
  DMA_ClearITPendingBit(SHELLPORT_DMA_RX_TC_IT);

  NVIC_SetPriority(SHELLPORT_USART_IRQn, SHELLPORT_IRQ_PRIO);
  NVIC_SetPriority(SHELLPORT_DMA_RX_IRQn, SHELLPORT_IRQ_PRIO);
  NVIC_ClearPendingIRQ(SHELLPORT_USART_IRQn);
  NVIC_ClearPendingIRQ(SHELLPORT_DMA_RX_IRQn);
  NVIC_EnableIRQ(SHELLPORT_DMA_RX_IRQn);

  /* Arm the circular DMA, then enable USART RX DMA requests, then unmask
   * IDLE. Order matters: the DMA must be live before USART starts feeding it. */
  DMA_Cmd(SHELLPORT_DMA_RX_CH, ENABLE);
  USART_DMACmd(SHELLPORT_USART, USART_DMAReq_Rx, ENABLE);

  USART_ITConfig(SHELLPORT_USART, USART_IT_IDLE, ENABLE);
  NVIC_EnableIRQ(SHELLPORT_USART_IRQn);

  /* The shell prints no version / "ready": the application banner is the only
   * place the build version appears on the wire. Keep this to a bare newline
   * so the prompt follows that banner cleanly. */
  shell_puts("\r\n");
  return 0;
}
/** @} */

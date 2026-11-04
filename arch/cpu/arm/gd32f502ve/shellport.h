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
 * \addtogroup shell
 * @{
 *
 * \defgroup shellport Quilla shell hardware port (GD32F502VE USART0 PB6/PB7)
 *
 * Binds the hardware-independent Quilla shell to USART0 on PB6 (TX) / PB7 (RX),
 * alternate function 0, 115200 8-N-1. This is the pinout of the reference
 * board's "TRANS_COM0" port, so the same wiring can be reused verbatim.
 *
 * Reception is DMA driven: DMA0 channel 4 (routed through DMAMUX request
 * DMA_REQUEST_USART0_RX) runs in CIRCULAR mode filling a single 512-byte ring
 * forever -- it never re-arms. The USART IDLE-line interrupt (frame delimiter)
 * and the DMA half-full / full-transfer interrupts merely poll the shell
 * process; the real work happens in shellport_rx_harvest(), which reads the
 * live DMA transfer counter from the shell thread and copies every new byte
 * into the shell ring / OTA sink. This is the same model the reference PY32
 * port uses, and it has no re-arm race and no "DMA silently stopped at a
 * buffer boundary" stall -- the two bug classes the old NORMAL ping-pong
 * design had on first GD32 silicon. The two console vectors run at the same
 * NVIC priority so they can never preempt each other.
 *
 * Transmission deliberately stays polling: see the note in shellport.c. It is
 * the only way to guarantee that the last byte has left the USART before the
 * QBOOT branches to the APP, which is what keeps the final QIAP frame intact.
 *
 * This file is the ONLY place that touches USART/GPIO/DMA/NVIC.
 *
 * @{
 */

#ifndef QUILLA_SHELLPORT_H_
#define QUILLA_SHELLPORT_H_

#include <stdint.h>

/* Default console baud rate (115200 8-N-1). Override from a board header. */
#ifndef SHELLPORT_BAUDRATE
#define SHELLPORT_BAUDRATE 115200U
#endif

/* Initialize the USART0 port and start the shell process.
 * Returns 0 on success, negative on failure. */
int shellport_init(void);

/* Output writer handed to the shell core (blocking USART transmit). */
void shellport_write(const char *s);

/* Required by the port-agnostic shell core (quilla-shell.c calls it from the
 * shell process) and by the QBOOT upgrade loop. It is the real RX worker on
 * this port: it reads the live DMA transfer counter, derives the ring head,
 * and copies every byte written since the last call into the shell ring / OTA
 * sink. The IDLE / DMA interrupts only poll the shell process; they never
 * touch the data path, so this is the single place bytes enter the shell. */
void shellport_rx_harvest(void);

#ifdef QBOOT_BUILD
/* QIAP-only extensions (compiled out for the APP image): */

/* Route received bytes to a raw sink (e.g. the OTA parser) instead of the
 * shell line buffer. Pass NULL to restore normal shell input. */
void shellport_set_raw_sink(void (*sink)(uint8_t b));

/* Blocking transmit of raw (possibly binary) bytes, bypassing the shell writer. */
void shellport_write_raw(const uint8_t *data, uint16_t len);
#endif

#endif /* QUILLA_SHELLPORT_H_ */
/** @} */
/** @} */

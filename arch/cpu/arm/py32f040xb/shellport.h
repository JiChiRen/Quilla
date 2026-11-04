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
 * \defgroup shellport Quilla shell hardware port (PY32F040xB USART2 + DMA)
 *
 * Binds the hardware-independent Quilla shell to the PY32F040xB USART2 on
 * PA2/PA3. Reception uses a single CIRCULAR-mode DMA feeding a 1024-byte ring
 * that runs forever (never re-armed). New bytes are harvested in the shell
 * thread and copied into the shell ring / OTA sink; the ISRs only poll the
 * shell process. This file is the ONLY place that touches UART/DMA/GPIO.
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

/* Initialize the USART2 + DMA port and start the shell process.
 * Returns 0 on success, negative on failure. */
int shellport_init(void);

/* Pull every byte the CIRCULAR RX DMA has written since the last call into the
 * shell ring / OTA sink. Runs only in a single thread context (the shell process
 * during interactive use, or qboot_qiap_run() while the upgrade loop owns the
 * CPU); never from an ISR. Call it from any owner of the serial RX path so the
 * DMA ring is drained even when the shell process is not scheduled. */
void shellport_rx_harvest(void);

/* Output writer handed to the shell core (blocking USART transmit). */
void shellport_write(const char *s);

/* Last-resort polling transmit, callable from an ISR (busy-waits the wire,
 * 100 ms cap). Bypasses the shell layer entirely; used by the wedge probe to
 * emit a dying-gasp line before a controlled reset. */
void shellport_emergency_write(const char *s);

#ifdef QBOOT_BUILD
/* QIAP-only extensions (compiled out for the APP image): */

/* Route received bytes to a raw sink (e.g. the OTA parser) instead of the
 * shell line buffer. Pass NULL to restore normal shell input. Intended for the
 * QIAP's serial upgrade phase where the link carries a binary protocol, not
 * shell lines. */
void shellport_set_raw_sink(void (*sink)(uint8_t b));

/* Blocking transmit of raw (possibly binary) bytes, bypassing the shell writer. */
void shellport_write_raw(const uint8_t *data, uint16_t len);
#endif

#endif /* QUILLA_SHELLPORT_H_ */
/** @} */
/** @} */

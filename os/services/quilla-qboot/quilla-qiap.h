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
 * \addtogroup quilla-qboot
 * @{
 *
 * \defgroup quilla-qiap Serial QIAP upgrade protocol state machine
 *
 * A byte-fed (streaming) parser implementing the Quilla QIAP upgrade protocol
 * described in doc/qboot-app-architecture.md. It is compiled into the QBOOT image
 * only. The QBOOT main loop pulls bytes off the serial link and calls
 * quilla_qiap_feed(); the state machine validates addresses against the APP
 * region, writes the APP Flash through quilla-qboot-hal.h, and verifies the
 * per-frame and total CRCs.
 *
 * The machine is transport-agnostic: it knows nothing about the UART. Replies
 * go out through quilla_qboot_hal_transmit().
 *
 * @{
 */

#ifndef QUILLA_QIAP_H_
#define QUILLA_QIAP_H_

#include <stdint.h>

#include "quilla-qboot-hal.h"

/* ---- protocol constants (mirror doc/qboot-app-architecture.md) ------------- */
#define QUILLA_QIAP_SOF        0xA5U

#define QUILLA_QIAP_CMD_ENTER  0x01U   /* H->M: app_start_addr(4) + unlock_key(8)
                                        *       [+ image_secver(4); required once
                                        *        a verification key is provisioned] */
#define QUILLA_QIAP_CMD_DATA   0x02U   /* H->M: start_addr(4) + end_addr(4) + data */
#define QUILLA_QIAP_CMD_END    0x03U   /* H->M: total_len(4) + total_crc16(2)     */
#define QUILLA_QIAP_CMD_SIGN   0x04U   /* H->M: signature(64)                     */

#define QUILLA_QIAP_ACK_ENTER  0x81U   /* M->H: status(1)                        */
#define QUILLA_QIAP_ACK_DATA   0x82U   /* M->H: start_addr(4)                    */
#define QUILLA_QIAP_ACK_END    0x83U   /* M->H: status(1)                        */
#define QUILLA_QIAP_ACK_SIGN   0x84U   /* M->H: status(1)                        */
#define QUILLA_QIAP_NAK        0xC1U   /* M->H: reason(1) (bit0..2 = phase)      */

/* SIGN payload: Ed25519 signature(64). */
#define QUILLA_QIAP_SIGN_LEN   64U

/* NAK reason codes. */
#define QUILLA_QIAP_RC_ADDR    0x01U   /* address out of the APP region          */
#define QUILLA_QIAP_RC_KEY     0x02U   /* unlock key mismatch                    */
#define QUILLA_QIAP_RC_CRC     0x03U   /* frame/field CRC error                  */
#define QUILLA_QIAP_RC_ERASE   0x04U   /* Flash page erase failed                */
#define QUILLA_QIAP_RC_PROG    0x05U   /* Flash program/readback failed          */
#define QUILLA_QIAP_RC_LEN     0x06U   /* total length/CRC mismatch              */
#define QUILLA_QIAP_RC_SZ      0x07U   /* length field vs data mismatch          */
#define QUILLA_QIAP_RC_SIGN    0x08U   /* signature did not verify / not signed  */
#define QUILLA_QIAP_RC_VER     0x09U   /* image has no usable security-version region */
#define QUILLA_QIAP_RC_ROLLBACK 0x0AU  /* firmware security version below device floor (downgrade) */
#define QUILLA_QIAP_RC_NO_SECVER 0x0BU /* ENTER carries no image security version
                                        * (host tool predates the CR-0055 ENTER
                                        * extension; rejected before any write) */

/* Max payload per frame: one Flash page of data plus the 8-byte (start,end)
 * header. The host chunks larger writes. */
#define QUILLA_QIAP_MAX_PAYLOAD  (QUILLA_QBOOT_PAGE_SIZE + 8U)
#define QUILLA_QIAP_UNLOCK_LEN   8U

/* Initialise the QIAP state machine. */
void quilla_qiap_init(void);

/* Feed one received byte from the serial port. */
void quilla_qiap_feed(uint8_t b);

/* Returns 1 after a successful END frame (flag restored to APP1). The QBOOT
 * main loop should then reset the MCU. */
int quilla_qiap_is_done(void);

#endif /* QUILLA_QIAP_H_ */
/** @} */
/** @} */

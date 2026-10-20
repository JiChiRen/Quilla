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
 */

/**
 * \addtogroup quilla-qboot
 * @{
 *
 * \defgroup quilla-qboot-flag QBOOT/APP interaction flag (field of the META record)
 *
 * A single uint32 flag that survives reset and power-cycle. It is no longer held
 * in a dedicated Flash page: it is one field of the wear-levelled META record
 * (quilla-qboot-meta.c). The QBOOT reads it at boot to decide jump-vs-QIAP; the
 * APP writes it to QIAP before resetting into the QBOOT.
 *
 * The module is hardware independent: quilla_qboot_flag_* simply delegate to the
 * META store (quilla-qboot-meta.h).
 *
 * @{
 */

#ifndef QUILLA_QBOOT_FLAG_H_
#define QUILLA_QBOOT_FLAG_H_

#include <stdint.h>

/* Flag values. */
#define QUILLA_QBOOT_FLAG_APP1   0x00000001U   /* default: run the APP            */
#define QUILLA_QBOOT_FLAG_QIAP    0x00000002U   /* serial upgrade in progress      */
#define QUILLA_QBOOT_FLAG_QBOOT 0x00000003U   /* stay in QBOOT shell (no jump,  */
                                                /*  no upgrade); set by 'swqboot' */

/* Initialise the flag module. Safe to call once at start-up of either image;
 * the caller must have the system clock running (Flash needs HCLK). */
void quilla_qboot_flag_init(void);

/* Read the current flag. Returns QUILLA_QBOOT_FLAG_APP1 when the magic is
 * absent (unprogrammed/erased page) or the value is anything other than QIAP, so
 * a corrupted or empty flag always falls back to "run the APP". */
uint32_t quilla_qboot_flag_get(void);

/* Write the flag (e.g. QUILLA_QBOOT_FLAG_QIAP / QUILLA_QBOOT_FLAG_APP1).
 * Erases the flag page and reprograms it. Returns 0 on success, <0 on error. */
int quilla_qboot_flag_set(uint32_t flag);

/* Erase the flag page back to the unprogrammed state (magic absent -> APP1). */
int quilla_qboot_flag_erase(void);

#endif /* QUILLA_QBOOT_FLAG_H_ */
/** @} */
/** @} */

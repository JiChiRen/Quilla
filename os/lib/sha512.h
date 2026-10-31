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
 * \addtogroup lib
 * @{
 *
 * \defgroup sha512 SHA-512 (FIPS 180-4)
 *
 * A self-contained, dependency-free SHA-512 implementation required by the
 * Ed25519 signature verification path. It operates on 64-bit words; on a
 * Cortex-M0+ these compile to multi-instruction sequences via the compiler's
 * soft 64-bit support, which is correct (only larger than SHA-256). The message
 * length is tracked with a 64-bit byte counter, enough for any firmware image.
 *
 * @{
 */

#ifndef SHA512_H_
#define SHA512_H_

#include <stdint.h>

#define SHA512_DIGEST_SIZE  64U
#define SHA512_BLOCK_SIZE   128U

typedef struct {
  uint64_t state[8];                       /* H0..H7                       */
  uint64_t total_len;                      /* bytes hashed, mod 2^64        */
  uint8_t  buf[SHA512_BLOCK_SIZE];
  uint8_t  buf_len;                        /* bytes waiting in buf          */
} sha512_ctx_t;

void sha512_init(sha512_ctx_t *ctx);
void sha512_update(sha512_ctx_t *ctx, const uint8_t *data, uint32_t len);
void sha512_final(sha512_ctx_t *ctx, uint8_t digest[SHA512_DIGEST_SIZE]);
void sha512_hash(const uint8_t *data, uint32_t len,
                 uint8_t digest[SHA512_DIGEST_SIZE]);

#endif /* SHA512_H_ */
/** @} */
/** @} */

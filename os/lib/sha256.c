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
 * \addtogroup sha256
 * @{
 */

#include "sha256.h"

#include <string.h>

/* Initial hash values H0..H7 (FIPS 180-4 §5.3.3). */
static const uint32_t H0[8] = {
  0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
  0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
};

/* Round constants K0..K63 (FIPS 180-4 §4.2.2). */
static const uint32_t K[64] = {
  0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
  0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
  0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
  0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
  0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
  0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
  0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
  0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
  0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
  0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
  0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

/* Rotate-right / rotate-left on a 32-bit word (well-defined for n in [0,31]. */
static uint32_t
rotr32(uint32_t x, uint32_t n)
{
  return (x >> n) | (x << (32U - n));
}

/* SHA-256 compression function on a single 64-byte block. */
static void
sha256_block(sha256_ctx_t *ctx, const uint8_t *block)
{
  uint32_t w[64];
  uint32_t a, b, c, d, e, f, g, h;
  uint32_t t2;
  uint32_t i;

  /* Prepare the message schedule W0..W15 (big-endian words). */
  for(i = 0U; i < 16U; i++) {
    w[i] = ((uint32_t)block[i * 4U] << 24) |
           ((uint32_t)block[i * 4U + 1U] << 16) |
           ((uint32_t)block[i * 4U + 2U] << 8) |
           ((uint32_t)block[i * 4U + 3U]);
  }
  /* Extend to W16..W63. */
  for(i = 16U; i < 64U; i++) {
    uint32_t s0, s1;
    s0 = rotr32(w[i - 15U], 7U) ^ rotr32(w[i - 15U], 18U) ^ (w[i - 15U] >> 3);
    s1 = rotr32(w[i - 2U], 17U) ^ rotr32(w[i - 2U], 19U) ^ (w[i - 2U] >> 10);
    w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
  }

  a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
  e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

  for(i = 0U; i < 64U; i++) {
    uint32_t S1, ch, temp, S0, maj;
    S1 = rotr32(e, 6U) ^ rotr32(e, 11U) ^ rotr32(e, 25U);
    ch = (e & f) ^ ((~e) & g);
    temp = h + S1 + ch + K[i] + w[i];
    S0 = rotr32(a, 2U) ^ rotr32(a, 13U) ^ rotr32(a, 22U);
    maj = (a & b) ^ (a & c) ^ (b & c);
    t2 = S0 + maj;
    h = g; g = f; f = e; e = d + temp; d = c; c = b; b = a; a = temp + t2;
  }

  ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
  ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void
sha256_init(sha256_ctx_t *ctx)
{
  uint32_t i;
  for(i = 0U; i < 8U; i++) {
    ctx->state[i] = H0[i];
  }
  ctx->total_len = 0U;
  ctx->buf_len = 0U;
}

void
sha256_update(sha256_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
  while(len > 0U) {
    uint8_t take = (uint8_t)(SHA256_BLOCK_SIZE - ctx->buf_len);
    if(take > len) {
      take = (uint8_t)len;
    }
    (void)memcpy(&ctx->buf[ctx->buf_len], data, take);
    ctx->buf_len += take;
    data += take;
    len -= take;
    ctx->total_len += take;

    if(ctx->buf_len == SHA256_BLOCK_SIZE) {
      sha256_block(ctx, ctx->buf);
      ctx->buf_len = 0U;
    }
  }
}

void
sha256_final(sha256_ctx_t *ctx, uint8_t digest[SHA256_DIGEST_SIZE])
{
  uint32_t bit_len = ctx->total_len << 3;   /* firmware << 3 is well within 32-bit */
  uint8_t i;

  /* The padding always adds at least the 0x80 marker byte plus the 8-byte
   * length, so if there is no room we flush a zero-padded block first. */
  if(ctx->buf_len < 56U) {
    ctx->buf[ctx->buf_len] = 0x80U;
    for(i = (uint8_t)(ctx->buf_len + 1U); i < 56U; i++) {
      ctx->buf[i] = 0x00U;
    }
  } else {
    ctx->buf[ctx->buf_len] = 0x80U;
    for(i = (uint8_t)(ctx->buf_len + 1U); i < SHA256_BLOCK_SIZE; i++) {
      ctx->buf[i] = 0x00U;
    }
    sha256_block(ctx, ctx->buf);
    for(i = 0U; i < 56U; i++) {
      ctx->buf[i] = 0x00U;
    }
  }

  /* Append the 64-bit bit length, big-endian (high word first; only the low
   * 32-bit value is ever non-zero for firmware, but write both for correctness). */
  ctx->buf[56U] = 0x00U;
  ctx->buf[57U] = 0x00U;
  ctx->buf[58U] = 0x00U;
  ctx->buf[59U] = 0x00U;
  ctx->buf[60U] = (uint8_t)(bit_len >> 24);
  ctx->buf[61U] = (uint8_t)(bit_len >> 16);
  ctx->buf[62U] = (uint8_t)(bit_len >> 8);
  ctx->buf[63U] = (uint8_t)(bit_len);
  sha256_block(ctx, ctx->buf);

  for(i = 0U; i < SHA256_DIGEST_SIZE; i += 4U) {
    uint32_t v = ctx->state[i / 4U];
    digest[i] = (uint8_t)(v >> 24);
    digest[i + 1U] = (uint8_t)(v >> 16);
    digest[i + 2U] = (uint8_t)(v >> 8);
    digest[i + 3U] = (uint8_t)(v);
  }
}

void
sha256_hash(const uint8_t *data, uint32_t len,
            uint8_t digest[SHA256_DIGEST_SIZE])
{
  sha256_ctx_t ctx;
  sha256_init(&ctx);
  sha256_update(&ctx, data, len);
  sha256_final(&ctx, digest);
}

/** @} */

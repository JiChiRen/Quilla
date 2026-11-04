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
 * \addtogroup sha512
 * @{
 */

#include "sha512.h"

#include <string.h>

/* Initial hash values H0..H7 (FIPS 180-4 §5.3.5). */
static const uint64_t H0[8] = {
  0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
  0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
  0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
  0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

/* Round constants K0..K79 (FIPS 180-4 §4.2.3, generated with exact integer
 * arithmetic so no hand-copied value can be wrong). */
static const uint64_t K[80] = {
  0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
  0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
  0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
  0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
  0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
  0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
  0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
  0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
  0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
  0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
  0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
  0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
  0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
  0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
  0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
  0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
  0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
  0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
  0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
  0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

/* Rotate-right on a 64-bit word. Never called with n==0, so the (64-n)
 * shift is always in [1,63]. */
static uint64_t
rotr64(uint64_t x, uint64_t n)
{
  return (x >> n) | (x << (64U - n));
}

static void
sha512_block(sha512_ctx_t *ctx, const uint8_t *block)
{
  uint64_t w[80];
  uint64_t a, b, c, d, e, f, g, h;
  uint64_t t1, t2;
  int i;

  for(i = 0; i < 16; i++) {
    w[i] = ((uint64_t)block[i * 8] << 56) |
           ((uint64_t)block[i * 8 + 1] << 48) |
           ((uint64_t)block[i * 8 + 2] << 40) |
           ((uint64_t)block[i * 8 + 3] << 32) |
           ((uint64_t)block[i * 8 + 4] << 24) |
           ((uint64_t)block[i * 8 + 5] << 16) |
           ((uint64_t)block[i * 8 + 6] << 8) |
           (uint64_t)block[i * 8 + 7];
  }
  for(i = 16; i < 80; i++) {
    uint64_t s0, s1;
    s0 = rotr64(w[i - 15], 1U) ^ rotr64(w[i - 15], 8U) ^ (w[i - 15] >> 7);
    s1 = rotr64(w[i - 2], 19U) ^ rotr64(w[i - 2], 61U) ^ (w[i - 2] >> 6);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
  e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

  for(i = 0; i < 80; i++) {
    uint64_t S1, ch, S0, maj;
    S1 = rotr64(e, 14U) ^ rotr64(e, 18U) ^ rotr64(e, 41U);
    ch = (e & f) ^ ((~e) & g);
    t1 = h + S1 + ch + K[i] + w[i];
    S0 = rotr64(a, 28U) ^ rotr64(a, 34U) ^ rotr64(a, 39U);
    maj = (a & b) ^ (a & c) ^ (b & c);
    t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }

  ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
  ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

void
sha512_init(sha512_ctx_t *ctx)
{
  int i;
  for(i = 0; i < 8; i++) {
    ctx->state[i] = H0[i];
  }
  ctx->total_len = 0U;
  ctx->buf_len = 0U;
}

void
sha512_update(sha512_ctx_t *ctx, const uint8_t *data, uint32_t len)
{
  while(len > 0U) {
    uint8_t take = (uint8_t)(SHA512_BLOCK_SIZE - ctx->buf_len);
    if(take > len) {
      take = (uint8_t)len;
    }
    (void)memcpy(&ctx->buf[ctx->buf_len], data, take);
    ctx->buf_len += take;
    data += take;
    len -= take;
    ctx->total_len += take;

    if(ctx->buf_len == SHA512_BLOCK_SIZE) {
      sha512_block(ctx, ctx->buf);
      ctx->buf_len = 0U;
    }
  }
}

void
sha512_final(sha512_ctx_t *ctx, uint8_t digest[SHA512_DIGEST_SIZE])
{
  uint64_t bit_len = ctx->total_len << 3U;   /* firmware << 3 is well within 64-bit */
  uint8_t i;

  if(ctx->buf_len < 112U) {
    ctx->buf[ctx->buf_len] = 0x80U;
    for(i = (uint8_t)(ctx->buf_len + 1U); i < 112U; i++) {
      ctx->buf[i] = 0x00U;
    }
  } else {
    ctx->buf[ctx->buf_len] = 0x80U;
    for(i = (uint8_t)(ctx->buf_len + 1U); i < SHA512_BLOCK_SIZE; i++) {
      ctx->buf[i] = 0x00U;
    }
    sha512_block(ctx, ctx->buf);
    for(i = 0U; i < 112U; i++) {
      ctx->buf[i] = 0x00U;
    }
  }

  /* 128-bit big-endian bit length; the high 64 bits are 0 for any firmware. */
  ctx->buf[112U] = 0x00U; ctx->buf[113U] = 0x00U; ctx->buf[114U] = 0x00U; ctx->buf[115U] = 0x00U;
  ctx->buf[116U] = 0x00U; ctx->buf[117U] = 0x00U; ctx->buf[118U] = 0x00U; ctx->buf[119U] = 0x00U;
  ctx->buf[120U] = (uint8_t)(bit_len >> 56);
  ctx->buf[121U] = (uint8_t)(bit_len >> 48);
  ctx->buf[122U] = (uint8_t)(bit_len >> 40);
  ctx->buf[123U] = (uint8_t)(bit_len >> 32);
  ctx->buf[124U] = (uint8_t)(bit_len >> 24);
  ctx->buf[125U] = (uint8_t)(bit_len >> 16);
  ctx->buf[126U] = (uint8_t)(bit_len >> 8);
  ctx->buf[127U] = (uint8_t)(bit_len);
  sha512_block(ctx, ctx->buf);

  for(i = 0U; i < SHA512_DIGEST_SIZE; i += 8U) {
    uint64_t v = ctx->state[i / 8U];
    digest[i] = (uint8_t)(v >> 56);
    digest[i + 1U] = (uint8_t)(v >> 48);
    digest[i + 2U] = (uint8_t)(v >> 40);
    digest[i + 3U] = (uint8_t)(v >> 32);
    digest[i + 4U] = (uint8_t)(v >> 24);
    digest[i + 5U] = (uint8_t)(v >> 16);
    digest[i + 6U] = (uint8_t)(v >> 8);
    digest[i + 7U] = (uint8_t)(v);
  }
}

void
sha512_hash(const uint8_t *data, uint32_t len, uint8_t digest[SHA512_DIGEST_SIZE])
{
  sha512_ctx_t ctx;
  sha512_init(&ctx);
  sha512_update(&ctx, data, len);
  sha512_final(&ctx, digest);
}

/** @} */

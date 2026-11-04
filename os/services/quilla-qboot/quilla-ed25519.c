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
 * \addtogroup quilla-ed25519
 * @{
 *
 * Ed25519 verification (RFC 8032) built on a self-contained 256/512-bit
 * big-integer field arithmetic over GF(2^255-19).  No platform crypto, no
 * 64-bit assumption beyond what the C compiler provides (multi-precision is
 * done with uint32_t limbs and uint64_t accumulators, so it is portable to
 * Cortex-M0+ where 64-bit math is emulated by the compiler).
 *
 * The Edwards addition law used (a = -1, d a non-square) is complete, so the
 * same formula serves for addition and doubling with no exceptional cases.
 */

#include "quilla-ed25519.h"
#include "sha512.h"

#include <string.h>

#ifdef QUILLA_ED25519_SELFTEST
#include <stdio.h>
#endif

/* ------------------------------------------------------------------ */
/* Big integer: little-endian 16 x uint32 (512-bit capacity).          */
/* Field elements only use the low 8 limbs (value < 2^256).           */
/* ------------------------------------------------------------------ */

typedef uint32_t bn[16];

/* Edwards group element: projective (X:Y:Z), a = -1 (complete law). */
typedef struct {
  bn X, Y, Z;
} ge;

/* Forward declarations so ed_init() can use the helpers defined below. */
static void bn_zero(bn a);
static void bn_copy(bn d, const bn s);
static void bn_set_bytes32(bn a, const uint8_t *b);

/* Ed25519 domain constants, generated independently (see implementation
 * notes) so no hand-copied magic number can be wrong.  All stored
 * little-endian. */
static const uint8_t ED25519_P[32] = {
  0xedU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
  0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x7fU
};
static const uint8_t ED25519_D[32] = {
  0xa3U, 0x78U, 0x59U, 0x13U, 0xcaU, 0x4dU, 0xebU, 0x75U, 0xabU, 0xd8U, 0x41U, 0x41U, 0x4dU, 0x0aU, 0x70U, 0x00U,
  0x98U, 0xe8U, 0x79U, 0x77U, 0x79U, 0x40U, 0xc7U, 0x8cU, 0x73U, 0xfeU, 0x6fU, 0x2bU, 0xeeU, 0x6cU, 0x03U, 0x52U
};
static const uint8_t ED25519_L[32] = {
  0xedU, 0xd3U, 0xf5U, 0x5cU, 0x1aU, 0x63U, 0x12U, 0x58U, 0xd6U, 0x9cU, 0xf7U, 0xa2U, 0xdeU, 0xf9U, 0xdeU, 0x14U,
  0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x10U
};
static const uint8_t ED25519_BX[32] = {
  0x1aU, 0xd5U, 0x25U, 0x8fU, 0x60U, 0x2dU, 0x56U, 0xc9U, 0xb2U, 0xa7U, 0x25U, 0x95U, 0x60U, 0xc7U, 0x2cU, 0x69U,
  0x5cU, 0xdcU, 0xd6U, 0xfdU, 0x31U, 0xe2U, 0xa4U, 0xc0U, 0xfeU, 0x53U, 0x6eU, 0xcdU, 0xd3U, 0x36U, 0x69U, 0x21U
};
static const uint8_t ED25519_BY[32] = {
  0x58U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U,
  0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U, 0x66U
};
static const uint8_t ED25519_I[32] = {
  0xb0U, 0xa0U, 0x0eU, 0x4aU, 0x27U, 0x1bU, 0xeeU, 0xc4U, 0x78U, 0xe4U, 0x2fU, 0xadU, 0x06U, 0x18U, 0x43U, 0x2fU,
  0xa7U, 0xd7U, 0xfbU, 0x3dU, 0x99U, 0x00U, 0x4dU, 0x2bU, 0x0bU, 0xdfU, 0xc1U, 0x4fU, 0x80U, 0x24U, 0x83U, 0x2bU
};
static const uint8_t ED25519_PE2[32] = {
  0xebU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
  0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x7fU
};
static const uint8_t ED25519_P38[32] = {
  0xfeU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
  0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x0fU
};

/* Runtime-loaded copies. */
static bn P_, D_, I_, L_, ONE_;
static ge B_pt;
static int ed_inited = 0;

static void ed_init(void)
{
  int i;
  if(ed_inited) {
    return;
  }
  for(i = 0; i < 16; i++) {
    P_[i] = 0; D_[i] = 0; I_[i] = 0; L_[i] = 0; ONE_[i] = 0;
  }
  for(i = 0; i < 8; i++) {
    P_[i] = ((uint32_t)ED25519_P[4 * i]) |
            ((uint32_t)ED25519_P[4 * i + 1] << 8) |
            ((uint32_t)ED25519_P[4 * i + 2] << 16) |
            ((uint32_t)ED25519_P[4 * i + 3] << 24);
    D_[i] = ((uint32_t)ED25519_D[4 * i]) |
            ((uint32_t)ED25519_D[4 * i + 1] << 8) |
            ((uint32_t)ED25519_D[4 * i + 2] << 16) |
            ((uint32_t)ED25519_D[4 * i + 3] << 24);
    I_[i] = ((uint32_t)ED25519_I[4 * i]) |
            ((uint32_t)ED25519_I[4 * i + 1] << 8) |
            ((uint32_t)ED25519_I[4 * i + 2] << 16) |
            ((uint32_t)ED25519_I[4 * i + 3] << 24);
    L_[i] = ((uint32_t)ED25519_L[4 * i]) |
            ((uint32_t)ED25519_L[4 * i + 1] << 8) |
            ((uint32_t)ED25519_L[4 * i + 2] << 16) |
            ((uint32_t)ED25519_L[4 * i + 3] << 24);
  }
  ONE_[0] = 1;
  bn_set_bytes32(B_pt.X, ED25519_BX);
  bn_set_bytes32(B_pt.Y, ED25519_BY);
  bn_zero(B_pt.Z);
  B_pt.Z[0] = 1;
  ed_inited = 1;
}

/* ---- low level big-integer ops (mod 2^512, values kept < 2^256) ---- */

static void bn_zero(bn a)
{
  int i;
  for(i = 0; i < 16; i++) {
    a[i] = 0;
  }
}

static void bn_copy(bn d, const bn s)
{
  int i;
  for(i = 0; i < 16; i++) {
    d[i] = s[i];
  }
}

static int bn_is_zero(const bn a)
{
  int i;
  for(i = 0; i < 16; i++) {
    if(a[i]) {
      return 0;
    }
  }
  return 1;
}

static int bn_cmp(const bn a, const bn b)
{
  int i;
  for(i = 15; i >= 0; i--) {
    if(a[i] != b[i]) {
      return (a[i] > b[i]) ? 1 : -1;
    }
  }
  return 0;
}

/* a = a + b (mod 2^512; b and sum fit in 512 bits). */
static void bn_add(bn a, const bn b)
{
  uint64_t carry = 0;
  int i;
  for(i = 0; i < 16; i++) {
    uint64_t s = (uint64_t)a[i] + b[i] + carry;
    a[i] = (uint32_t)s;
    carry = s >> 32;
  }
}

/* a = a - b, assuming a >= b. */
static void bn_sub(bn a, const bn b)
{
  int64_t borrow = 0;
  int i;
  for(i = 0; i < 16; i++) {
    int64_t s = (int64_t)a[i] - (int64_t)b[i] - borrow;
    if(s < 0) {
      s += ((int64_t)1 << 32);
      borrow = 1;
    } else {
      borrow = 0;
    }
    a[i] = (uint32_t)s;
  }
}

/* a = a >> n (logical, 0 <= n < 512). */
static void bn_shr(bn a, unsigned n)
{
  unsigned word = n / 32, bits = n % 32, i;
  if(word >= 16) {
    bn_zero(a);
    return;
  }
  if(bits == 0) {
    for(i = 0; i < 16 - word; i++) {
      a[i] = a[i + word];
    }
  } else {
    for(i = 0; i < 16 - word - 1; i++) {
      a[i] = (a[i + word] >> bits) | (a[i + word + 1] << (32 - bits));
    }
    a[16 - word - 1] = a[15] >> bits;
  }
  for(i = 16 - word; i < 16; i++) {
    a[i] = 0;
  }
}

/* a = (a * c) mod 2^512. */
static void bn_mul_small(bn a, uint32_t c)
{
  uint64_t carry = 0;
  int i;
  for(i = 0; i < 16; i++) {
    uint64_t s = (uint64_t)a[i] * c + carry;
    a[i] = (uint32_t)s;
    carry = s >> 32;
  }
}

/* out = a * b, a and b have zero upper limbs (value < 2^256). */
static void bn_mul(const bn a, const bn b, bn out)
{
  bn t;
  int i, j;
  bn_zero(t);
  for(i = 0; i < 8; i++) {
    uint64_t ai = a[i];
    if(ai == 0) {
      continue;
    }
    for(j = 0; j < 8; j++) {
      uint64_t p = ai * b[j];
      int k = i + j;
      uint64_t s = (uint64_t)t[k] + p;
      t[k] = (uint32_t)s;
      uint64_t carry = s >> 32;
      k++;
      while(carry) {
        if(k >= 16) {
          break;
        }
        s = (uint64_t)t[k] + carry;
        t[k] = (uint32_t)s;
        carry = s >> 32;
        k++;
      }
    }
  }
  bn_copy(out, t);
}

/* out = a mod m, using binary long division.  a up to 2^512, m = L (< 2^256). */
static void bn_mod_m(const bn a, const bn m, bn out)
{
  bn r;
  int bit;
  bn_zero(r);
  for(bit = 511; bit >= 0; bit--) {
    int k;
    uint64_t carry = 0;
    /* Shift r left by one bit.  The carry out of limb k feeds limb k+1,
     * so the limbs must be visited in ascending order; descending order
     * would push the carry from the top limb downwards and corrupt the
     * remainder. */
    for(k = 0; k < 16; k++) {
      uint64_t s = ((uint64_t)r[k] << 1) | carry;
      r[k] = (uint32_t)s;
      carry = s >> 32;
    }
    if((a[bit / 32] >> (bit % 32)) & 1U) {
      r[0] |= 1U;
    }
    if(bn_cmp(r, m) >= 0) {
      bn_sub(r, m);
    }
  }
  bn_copy(out, r);
}

/* out = a mod (2^255 - 19) via the identity 2^255 = 19 (mod p). */
static void reduce_mod_p(bn x)
{
  bn lo, hi, t;
  int guard = 0;
  while(guard < 16) {
    bn_copy(lo, x);
    lo[7] &= 0x7FFFFFFFU;          /* keep bits 0..254, drop bit 255+ */
    {
      int i;
      for(i = 8; i < 16; i++) {
        lo[i] = 0;
      }
    }
    bn_copy(hi, x);
    bn_shr(hi, 255);
    bn_copy(t, hi);
    bn_mul_small(t, 19);           /* t = 19 * (x >> 255) */
    bn_add(lo, t);                 /* lo = (x mod 2^255) + 19*(x>>255) */
    bn_copy(x, lo);
    if(bn_cmp(x, P_) < 0) {
      break;
    }
    guard++;
  }
  while(bn_cmp(x, P_) >= 0) {
    bn_sub(x, P_);
  }
}

static void bn_set_bytes32(bn a, const uint8_t *b)
{
  int i;
  for(i = 0; i < 16; i++) {
    a[i] = 0;
  }
  for(i = 0; i < 8; i++) {
    a[i] = ((uint32_t)b[4 * i]) |
           ((uint32_t)b[4 * i + 1] << 8) |
           ((uint32_t)b[4 * i + 2] << 16) |
           ((uint32_t)b[4 * i + 3] << 24);
  }
}

static void bn_set_bytes64(bn a, const uint8_t *b)
{
  int i;
  for(i = 0; i < 16; i++) {
    a[i] = ((uint32_t)b[4 * i]) |
           ((uint32_t)b[4 * i + 1] << 8) |
           ((uint32_t)b[4 * i + 2] << 16) |
           ((uint32_t)b[4 * i + 3] << 24);
  }
}

static void bn_get_bytes32(const bn a, uint8_t *b)
{
  int i;
  for(i = 0; i < 8; i++) {
    b[4 * i] = (uint8_t)a[i];
    b[4 * i + 1] = (uint8_t)(a[i] >> 8);
    b[4 * i + 2] = (uint8_t)(a[i] >> 16);
    b[4 * i + 3] = (uint8_t)(a[i] >> 24);
  }
}

/* ------------------------------------------------------------------ */
/* Field arithmetic over GF(p), p = 2^255 - 19.                       */
/* ------------------------------------------------------------------ */

static void fe_add(bn r, const bn a, const bn b)
{
  bn t;
  bn_copy(t, a);
  bn_add(t, b);
  reduce_mod_p(t);
  bn_copy(r, t);
}

static void fe_sub(bn r, const bn a, const bn b)
{
  bn t;
  bn_copy(t, a);
  if(bn_cmp(a, b) >= 0) {
    bn_sub(t, b);
  } else {
    bn_add(t, P_);
    bn_sub(t, b);
  }
  bn_copy(r, t);
}

static void fe_mul(bn r, const bn a, const bn b)
{
  bn t;
  bn_mul(a, b, t);
  reduce_mod_p(t);
  bn_copy(r, t);
}

static void fe_sqr(bn r, const bn a)
{
  fe_mul(r, a, a);
}

static void fe_neg(bn r, const bn a)
{
  bn t;
  bn_copy(t, P_);
  bn_sub(t, a);
  reduce_mod_p(t);
  bn_copy(r, t);
}

static int fe_eq(const bn a, const bn b)
{
  return bn_cmp(a, b) == 0;
}

/* r = base^exp, exp is a 32-byte little-endian integer. */
static void fe_pow(bn r, const bn base, const uint8_t *exp32)
{
  bn res, b, tmp;
  int i;
  bn_zero(res);
  res[0] = 1;                     /* res = 1 */
  bn_copy(b, base);
  for(i = 255; i >= 0; i--) {
    fe_sqr(res, res);
    if((exp32[i / 8] >> (i % 8)) & 1U) {
      fe_mul(tmp, res, b);
      bn_copy(res, tmp);
    }
  }
  bn_copy(r, res);
}

/* a = a^(p-2)  (Fermat inversion). */
static void fe_inv(bn a)
{
  bn t;
  fe_pow(t, a, ED25519_PE2);
  bn_copy(a, t);
}

/* ------------------------------------------------------------------ */
/* Edwards group: projective (X:Y:Z), a = -1 (complete addition law). */
/* ------------------------------------------------------------------ */

static void ge_set_identity(ge *p)
{
  bn_zero(p->X);
  bn_zero(p->Y);
  bn_zero(p->Z);
  p->Y[0] = 1;
  p->Z[0] = 1;
}

static void ge_add(ge *r, const ge *p, const ge *q)
{
  bn A, Br, C, D, E, F, G, H, t1, t2, t3, t4;
  fe_mul(A, p->Z, q->Z);          /* A = Z1*Z2 mod p */
  fe_sqr(Br, A);                  /* B = A^2 mod p */
  fe_mul(C, p->X, q->X);          /* C = X1*X2 mod p */
  fe_mul(D, p->Y, q->Y);          /* D = Y1*Y2 mod p */
  fe_mul(E, C, D);                /* E = C*D */
  fe_mul(E, E, D_);               /* E = d*C*D */
  fe_sub(F, Br, E);               /* F = B - E */
  fe_add(G, Br, E);               /* G = B + E */
  fe_add(t1, p->X, p->Y);         /* (X1+Y1) */
  fe_add(t2, q->X, q->Y);         /* (X2+Y2) */
  fe_mul(H, t1, t2);              /* H = (X1+Y1)(X2+Y2) */
  fe_sub(t3, H, C);               /* t3 = H - C */
  fe_sub(t3, t3, D);              /* t3 = H - C - D */
  fe_mul(t1, A, F);               /* t1 = A*F */
  fe_mul(r->X, t1, t3);           /* X3 = A*F*(H-C-D) */
  fe_add(t4, D, C);               /* t4 = D + C  (a = -1) */
  fe_mul(t2, A, G);               /* t2 = A*G */
  fe_mul(r->Y, t2, t4);           /* Y3 = A*G*(D+C) */
  fe_mul(r->Z, F, G);             /* Z3 = F*G */
}

static void ge_dbl(ge *r, const ge *p)
{
  ge_add(r, p, p);
}

static void ge_scalarmult(ge *r, const uint8_t *k32, const ge *P)
{
  ge res;
  int i;
  ge_set_identity(&res);
  for(i = 255; i >= 0; i--) {
    ge_dbl(&res, &res);
    if((k32[i / 8] >> (i % 8)) & 1U) {
      ge_add(&res, &res, P);
    }
  }
  *r = res;
}

static int ge_eq(const ge *p, const ge *q)
{
  bn a, b;
  fe_mul(a, p->X, q->Z);
  fe_mul(b, q->X, p->Z);
  if(!fe_eq(a, b)) {
    return 0;
  }
  fe_mul(a, p->Y, q->Z);
  fe_mul(b, q->Y, p->Z);
  if(!fe_eq(a, b)) {
    return 0;
  }
  return 1;
}

/* Decode a 32-byte Edwards point (RFC 8032 §5.1.3). Returns 1 on success. */
static int ge_decode(ge *out, const uint8_t *buf)
{
  uint8_t b[32];
  uint8_t sign;
  bn y, u, v, x2, x;
  int i;
  for(i = 0; i < 32; i++) {
    b[i] = buf[i];
  }
  sign = (uint8_t)((b[31] >> 7) & 1U);
  b[31] &= 0x7FU;
  bn_set_bytes32(y, b);           /* y in [0, 2^255-1] < p */

  fe_sqr(u, y);                   /* u = y^2 */
  fe_mul(v, D_, u);               /* v = d*y^2 */
  fe_add(v, v, ONE_);             /* v = d*y^2 + 1 */
  fe_sub(u, u, ONE_);             /* u = y^2 - 1 */
  if(bn_is_zero(v)) {
    return 0;
  }
  fe_inv(v);                      /* vinv = v^(p-2) */
  fe_mul(x2, u, v);               /* x2 = (y^2-1)/(d*y^2+1) */
  fe_pow(x, x2, ED25519_P38);     /* x = x2^((p+3)/8) */
  fe_sqr(u, x);                   /* u = x^2 */
  if(!fe_eq(u, x2)) {
    fe_mul(x, x, I_);             /* x *= sqrt(-1) */
    fe_sqr(u, x);
    if(!fe_eq(u, x2)) {
      return 0;
    }
  }
  if(bn_is_zero(x) && sign) {
    return 0;
  }
  if((x[0] & 1U) != sign) {
    fe_neg(x, x);
  }
  bn_copy(out->X, x);
  bn_copy(out->Y, y);
  bn_zero(out->Z);
  out->Z[0] = 1;
  return 1;
}

/* ------------------------------------------------------------------ */
/* Top-level verification (RFC 8032 §5.2.7, basic equation).          */
/* ------------------------------------------------------------------ */


int quilla_ed25519_verify(const uint8_t *sig, size_t siglen,
                          const uint8_t *msg, size_t msglen,
                          const uint8_t *pk)
{
  ge A, R, lhs, rhs;
  bn S, h, hred, sred;
  uint8_t digest[64];
  uint8_t sred_bytes[32];
  uint8_t h_bytes[32];

  if(siglen != 64U) {
    return 0;
  }
  ed_init();


  if(!ge_decode(&A, pk)) {
    return 0;
  }
  if(!ge_decode(&R, sig)) {
    return 0;
  }

  /* S must be the canonical little-endian encoding of an integer < L. */
  bn_set_bytes32(S, sig + 32);
  bn_mod_m(S, L_, sred);
  bn_get_bytes32(sred, sred_bytes);
  if(memcmp(sred_bytes, sig + 32, 32) != 0) {
    return 0;
  }

  /* h = SHA-512(R || A || M) reduced mod L. */
  {
    sha512_ctx_t ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);          /* R */
    sha512_update(&ctx, pk, 32);           /* A */
    sha512_update(&ctx, msg, (uint32_t)msglen);
    sha512_final(&ctx, digest);
  }
  bn_set_bytes64(h, digest);
  bn_mod_m(h, L_, hred);
  bn_get_bytes32(hred, h_bytes);

  /* [S]B == [h]A + R  (basic equation; sufficient per RFC 8032). */
  ge_scalarmult(&lhs, sig + 32, &B_pt);
  ge_scalarmult(&rhs, h_bytes, &A);
  ge_add(&rhs, &rhs, &R);


  return ge_eq(&lhs, &rhs) ? 1 : 0;
}

/** @} */

/* ------------------------------------------------------------------ */
/* Self test: compile with -DQUILLA_ED25519_SELFTEST and run.          */
/* ------------------------------------------------------------------ */

#ifdef QUILLA_ED25519_SELFTEST

static int hex2bin(const char *s, uint8_t *out, size_t max)
{
  size_t n = strlen(s) / 2;
  size_t i;
  if(n > max) {
    n = max;
  }
  for(i = 0; i < n; i++) {
    unsigned v = 0;
    char c0 = s[2 * i], c1 = s[2 * i + 1];
    if(c0 >= '0' && c0 <= '9') {
      v |= (unsigned)(c0 - '0') << 4;
    } else if(c0 >= 'a' && c0 <= 'f') {
      v |= (unsigned)(c0 - 'a' + 10) << 4;
    } else if(c0 >= 'A' && c0 <= 'F') {
      v |= (unsigned)(c0 - 'A' + 10) << 4;
    }
    if(c1 >= '0' && c1 <= '9') {
      v |= (unsigned)(c1 - '0');
    } else if(c1 >= 'a' && c1 <= 'f') {
      v |= (unsigned)(c1 - 'a' + 10);
    } else if(c1 >= 'A' && c1 <= 'F') {
      v |= (unsigned)(c1 - 'A' + 10);
    }
    out[i] = (uint8_t)v;
  }
  return (int)n;
}

static const struct {
  const char *pk, *msg, *sig;
} kTestVec[] = {
  {
    "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
    "",
    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
  },
  {
    "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
    "72",
    "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"
  },
  {
    "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
    "af82",
    "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"
  },
  {
    "278117fc144c72340f67d0f2316e8386ceffbf2b2428c9c51fef7c597f1d426e",
    "08b8b2b733424243760fe426a4b54908632110a66c2f6591eabd3345e3e4eb98"
    "fa6e264bf09efe12ee50f8f54e9f77b1e355f6c50544e23fb1433ddf73be84d8"
    "79de7c0046dc4996d9e773f4bc9efe5738829adb26c81b37c93a1b270b20329d"
    "658675fc6ea534e0810a4432826bf58c941efb65d57a338bbd2e26640f89ffbc"
    "1a858efcb8550ee3a5e1998bd177e93a7363c344fe6b199ee5d02e82d522c4fe"
    "ba15452f80288a821a579116ec6dad2b3b310da903401aa62100ab5d1a36553e"
    "06203b33890cc9b832f79ef80560ccb9a39ce767967ed628c6ad573cb116dbef"
    "efd75499da96bd68a8a97b928a8bbc103b6621fcde2beca1231d206be6cd9ec7"
    "aff6f6c94fcd7204ed3455c68c83f4a41da4af2b74ef5c53f1d8ac70bdcb7ed1"
    "85ce81bd84359d44254d95629e9855a94a7c1958d1f8ada5d0532ed8a5aa3fb2"
    "d17ba70eb6248e594e1a2297acbbb39d502f1a8c6eb6f1ce22b3de1a1f40cc24"
    "554119a831a9aad6079cad88425de6bde1a9187ebb6092cf67bf2b13fd65f270"
    "88d78b7e883c8759d2c4f5c65adb7553878ad575f9fad878e80a0c9ba63bcbcc"
    "2732e69485bbc9c90bfbd62481d9089beccf80cfe2df16a2cf65bd92dd597b07"
    "07e0917af48bbb75fed413d238f5555a7a569d80c3414a8d0859dc65a46128ba"
    "b27af87a71314f318c782b23ebfe808b82b0ce26401d2e22f04d83d1255dc51a"
    "ddd3b75a2b1ae0784504df543af8969be3ea7082ff7fc9888c144da2af58429e"
    "c96031dbcad3dad9af0dcbaaaf268cb8fcffead94f3c7ca495e056a9b47acdb7"
    "51fb73e666c6c655ade8297297d07ad1ba5e43f1bca32301651339e22904cc8c"
    "42f58c30c04aafdb038dda0847dd988dcda6f3bfd15c4b4c4525004aa06eeff8"
    "ca61783aacec57fb3d1f92b0fe2fd1a85f6724517b65e614ad6808d6f6ee34df"
    "f7310fdc82aebfd904b01e1dc54b2927094b2db68d6f903b68401adebf5a7e08"
    "d78ff4ef5d63653a65040cf9bfd4aca7984a74d37145986780fc0b16ac451649"
    "de6188a7dbdf191f64b5fc5e2ab47b57f7f7276cd419c17a3ca8e1b939ae49e4"
    "88acba6b965610b5480109c8b17b80e1b7b750dfc7598d5d5011fd2dcc5600a3"
    "2ef5b52a1ecc820e308aa342721aac0943bf6686b64b2579376504ccc493d97e"
    "6aed3fb0f9cd71a43dd497f01f17c0e2cb3797aa2a2f256656168e6c496afc5f"
    "b93246f6b1116398a346f1a641f3b041e989f7914f90cc2c7fff357876e506b5"
    "0d334ba77c225bc307ba537152f3f1610e4eafe595f6d9d90d11faa933a15ef1"
    "369546868a7f3a45a96768d40fd9d03412c091c6315cf4fde7cb68606937380d"
    "b2eaaa707b4c4185c32eddcdd306705e4dc1ffc872eeee475a64dfac86aba41c"
    "0618983f8741c5ef68d3a101e8a3b8cac60c905c15fc910840b94c00a0b9d0",
    "0aab4c900501b3e24d7cdf4663326a3a87df5e4843b2cbdb67cbf6e460fec350aa5371b1508f9f4528ecea23c436d94b5e8fcd4f681e30a6ac00a9704a188a03"
  },
};


int main(void)
{
  int fails = 0;
  int i;

  ed_init();   /* load constants + base point first */

  /* ---- diagnostics ---- */
  {
    bn a, ai, prod;
    int j;
    for(j = 0; j < 8; j++) {
      a[j] = 0x33333333U + (uint32_t)j;
    }
    for(j = 8; j < 16; j++) {
      a[j] = 0;
    }
    reduce_mod_p(a);
    bn_copy(ai, a);
    fe_inv(ai);
    fe_mul(prod, a, ai);
    if(!(prod[0] == 1 && prod[1] == 0 && prod[2] == 0 && prod[3] == 0 &&
         prod[4] == 0 && prod[5] == 0 && prod[6] == 0 && prod[7] == 0)) {
      printf("DIAG: field inverse/ mul/ reduce FAILED\n");
      fails++;
    } else {
      printf("DIAG: field inverse ok\n");
    }

    /* decode base point from its y encoding and compare recovered x to Bx */
    {
      uint8_t by[32];
      ge bp;
      int k;
      for(k = 0; k < 32; k++) {
        by[k] = ED25519_BY[k];
      }
      if(!ge_decode(&bp, by)) {
        printf("DIAG: base point decode FAILED\n");
        fails++;
      } else if(bn_cmp(bp.X, B_pt.X) != 0) {
        printf("DIAG: base point x recovery MISMATCH\n");
        fails++;
      } else {
        printf("DIAG: base point decode ok (x matches)\n");
      }
    }
  }
  /* ---- end diagnostics ---- */


  /* RFC 8032 TEST 1024 uses a 1023-byte message; size the buffer to hold
   * it in full (hex2bin() silently truncates to the buffer length). */
  uint8_t pk[32], msg[1024], sig[64];
  size_t nvec = sizeof(kTestVec) / sizeof(kTestVec[0]);

  ed_init();   /* also loads B_pt */

  for(i = 0; i < (int)nvec; i++) {
    int ml = hex2bin(kTestVec[i].msg, msg, sizeof(msg));
    hex2bin(kTestVec[i].pk, pk, sizeof(pk));
    hex2bin(kTestVec[i].sig, sig, sizeof(sig));
    if(quilla_ed25519_verify(sig, 64, msg, (size_t)ml, pk) != 1) {
      fails++;
      printf("FAIL: RFC8032 TEST %d (valid signature rejected)\n", i + 1);
    } else {
      printf("PASS: RFC8032 TEST %d\n", i + 1);
    }
  }

  /* Tamper: flip a byte in the signature of TEST 1 -> must reject. */
  hex2bin(kTestVec[0].msg, msg, sizeof(msg));
  hex2bin(kTestVec[0].pk, pk, sizeof(pk));
  hex2bin(kTestVec[0].sig, sig, sizeof(sig));
  sig[0] ^= 0xFFU;
  if(quilla_ed25519_verify(sig, 64, msg, 0, pk) != 0) {
    fails++;
    printf("FAIL: tampered signature was accepted\n");
  } else {
    printf("PASS: tampered signature rejected\n");
  }

  /* Wrong public key -> must reject. */
  hex2bin(kTestVec[0].msg, msg, sizeof(msg));
  hex2bin(kTestVec[1].pk, pk, sizeof(pk));   /* different pk */
  hex2bin(kTestVec[0].sig, sig, sizeof(sig));
  if(quilla_ed25519_verify(sig, 64, msg, 0, pk) != 0) {
    fails++;
    printf("FAIL: wrong public key was accepted\n");
  } else {
    printf("PASS: wrong public key rejected\n");
  }

  /* Non-canonical S (S >= L) -> must be rejected (RFC 8032 section 5.2.7).
   * The encoding below is R || (S + L): it reduces to the same scalar as
   * TEST 1's S, so only the canonicality check can catch it. */
  hex2bin(kTestVec[0].msg, msg, sizeof(msg));
  hex2bin(kTestVec[0].pk, pk, sizeof(pk));
  {
    static const char nc[] =
      "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e0652249015"
      "4c8c7872aa064e049dbb3013fbf29380d25bf5f0595bbe24655141438e7a101b";
    hex2bin(nc, sig, sizeof(sig));
  }
  if(quilla_ed25519_verify(sig, 64, msg, 0, pk) != 0) {
    fails++;
    printf("FAIL: non-canonical S (S >= L) was accepted\n");
  } else {
    printf("PASS: non-canonical S rejected\n");
  }

  /* Tampered message -> must be rejected. */
  {
    int ml = hex2bin(kTestVec[1].msg, msg, sizeof(msg));
    hex2bin(kTestVec[1].pk, pk, sizeof(pk));
    hex2bin(kTestVec[1].sig, sig, sizeof(sig));
    msg[0] ^= 0xFFU;
    if(quilla_ed25519_verify(sig, 64, msg, (size_t)ml, pk) != 0) {
      fails++;
      printf("FAIL: tampered message was accepted\n");
    } else {
      printf("PASS: tampered message rejected\n");
    }
  }

  printf("\n%d failures\n", fails);
  return fails;
}

#endif /* QUILLA_ED25519_SELFTEST */

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
 * \defgroup quilla-qboot-pubkey Per-product secure-boot public key
 *
 * Provisioning the Ed25519 verification key without breaking the IDE.
 *
 * The key is a 32-byte initializer. Passing it through the Keil/MDK
 * "Define" field (uvprojx) is unsafe: that field is comma-separated, so a
 * `{0x..,0x..}` list would be split into many bogus macros. Instead, select
 * a product macro here and define only that single token (no commas) in the
 * build. quilla-qboot-secure.h includes this header and, when a product macro
 * is active, defines QUILLA_SECURE_BOOT_PUBKEY before its own all-0xFF
 * default.
 *
 * The GNU flow uses the same product macro (see Makefile.qboot), so both
 * toolchains share one source of truth for the key.
 *
 * To provision a real product key: generate a keypair with
 * tools/quilla_ed25519_sign.py keygen, paste the public bytes into a new
 * #if branch below, and add the matching QUILLA_QBOOT_PRODUCT_* macro to the
 * QBOOT build (Keil Define field or Makefile C_DEFS).
 *
 * @{
 */

#ifndef QUILLA_QBOOT_PUBKEY_H_
#define QUILLA_QBOOT_PUBKEY_H_

/* Keep this guard so a build that still passes -DQUILLA_SECURE_BOOT_PUBKEY
 * directly (legacy GNU form) does not trigger a macro redefinition. */
#if defined(QUILLA_QBOOT_DEFAULT_KEY) && !defined(QUILLA_SECURE_BOOT_PUBKEY)
/* REPOSITORY DEFAULT KEY.
 * This is a REAL Ed25519 public key that ships with the repository so a fresh
 * clone can build, boot and sign immediately. At the cryptographic level it is
 * exactly as valid as a customer-generated key -- there is no "test" versus
 * "real" distinction. The only difference is that its private counterpart lives
 * in tools/_keytest/private.hex, so the keypair is PUBLICLY KNOWN: anyone who
 * holds the repository can sign an image this device will accept as authentic.
 *
 * For any real deployment you MUST replace it with your own keypair
 * (tools/quilla_ed25519_sign.py keygen) and never ship the default key to end
 * users. The warning block below reminds you at compile time; it is a warning,
 * not an error, because the choice to ship the default key is the customer's. */
#define QUILLA_SECURE_BOOT_PUBKEY { \
  0xa4,0xfa,0x8c,0x3a,0xe5,0x1e,0x12,0x34, \
  0xc1,0xd3,0xc9,0x7b,0x4a,0x9d,0x16,0xb5, \
  0xa3,0xa2,0x7c,0x90,0xfc,0xe2,0x56,0x13, \
  0xdd,0x01,0xa2,0x45,0x96,0xd5,0x9f,0x6c }
/* Marker: the key compiled into THIS image is the repository default key. */
#define QUILLA_SB_USING_DEFAULT_KEY  1
#endif

/* ---- Compile-time reminder to replace the default key ---------------------
 * The default key is publicly known (its private key is in tools/_keytest/).
 * Shipping it to end users means anyone holding this repository can sign an
 * image your devices will accept -- that is the customer's choice to make, not
 * a build-time prohibition. We only WARN so the substitution is never silently
 * forgotten. A production-intended build (QUILLA_PRODUCTION_BUILD) gets a louder
 * warning; a build that supplies its own key (QUILLA_SECURE_BOOT_PUBKEY) gets
 * none. */
#if defined(QUILLA_SB_USING_DEFAULT_KEY)
#if defined(QUILLA_PRODUCTION_BUILD)
#warning "QUILLA_PRODUCTION_BUILD is set but the REPOSITORY DEFAULT public key is compiled in. This key is publicly known (private key: tools/_keytest/private.hex) -- any holder of this repository can sign images your devices will accept. Replace it with your own keypair (-DQUILLA_SECURE_BOOT_PUBKEY={...}) before production shipment."
#else
#warning "QUILLA_QBOOT_DEFAULT_KEY is compiled in: this is the REPOSITORY DEFAULT public key (publicly known, private key in tools/_keytest/private.hex). Replace it with your own keypair (-DQUILLA_SECURE_BOOT_PUBKEY={...}) before production shipment."
#endif
#endif

#endif /* QUILLA_QBOOT_PUBKEY_H_ */

/**
 * @}
 * @}
 */

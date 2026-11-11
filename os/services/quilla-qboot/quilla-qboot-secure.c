/*
 * Copyright (c) 2026, JiChiRen Info. Tech.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE
 * INSTITUTE OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ----------------------------------------------------------------------------
 * Secure-boot policy (signature + anti-rollback).
 *
 * The manifest, floor and seal are no longer stored in fixed Flash pages; they
 * live inside the wear-levelled META log (quilla-qboot-meta.c). This file reads
 * them from the cached latest record and writes them through quilla_qboot_meta_*
 * (which appends a record without erasing the live slot). The signature
 * verification math itself is unchanged.
 * ----------------------------------------------------------------------------
 */

/**
 * \addtogroup quilla-qboot-secure
 * @{
 *
 * Implementation notes
 *
 * The signed object is deliberately small: rather than signing the whole
 * firmware image (which would need a streaming Ed25519 verifier), the
 * signature covers
 *
 *     image_len (4, LE) || SHA-256(image) (32)
 *
 * a fixed 36-byte descriptor. Ed25519 is then a one-shot call, and the only
 * operation that has to stream over the image is SHA-256, which is cheap and
 * already incremental. The firmware security version is not a descriptor
 * field, but it lives INSIDE image_len (at QUILLA_QBOOT_APP_START +
 * QUILLA_APP_SECVER_OFF), so the digest -- and therefore the signature --
 * covers it: only a genuine (signed) release can change the version.
 * See doc/secure-boot.md ("Dual anti-rollback mechanism").
 *
 * Verification binds the image content through the digest we compute here,
 * not through the digest stored in the manifest: the descriptor handed to
 * Ed25519 is rebuilt from the freshly computed hash. Tampering with the Flash
 * image therefore changes the descriptor and invalidates the signature even
 * if the attacker also rewrites the manifest's hash field. The stored hash is
 * still compared, but only to distinguish "image corrupted" from "wrong
 * signature" in the result code.
 *
 * @{
 */

#include "quilla-qboot-secure.h"

#include "quilla-qboot-meta.h"
#include "quilla-qboot-otp.h"
#include "quilla-ed25519.h"
#include "sha256.h"

#include <string.h>

/* Boot-phase markers were DEBUG ONLY and must never ship (project notes):
 * they emitted "[qb] m-*" timing strings over UART during secure-boot
 * bring-up. Retained as a no-op so the phase structure stays visible in
 * source without any UART traffic. */
#define QB_MARK(s)  ((void)0)

/* ---- Default-key reminder ------------------------------------------------
 * The repository default key is publicly known: its private half ships in
 * tools/_keytest/private.hex, so anyone holding this repository can sign an
 * image this device will accept. Warn (do not error) so replacement is never
 * silently forgotten; shipping the default key remains the customer's call.
 *
 * This sits in the one TU that instantiates the key, not in
 * quilla-qboot-pubkey.h: a header would repeat the warning in every including
 * translation unit. Exactly one warning per QBOOT build, none once a product
 * key is provisioned (QUILLA_SB_USING_DEFAULT_KEY is then undefined).
 * ------------------------------------------------------------------------- */
#if defined(QUILLA_SB_USING_DEFAULT_KEY)
#if defined(QUILLA_PRODUCTION_BUILD)
#warning "QBOOT: PRODUCTION build uses the repository default public key - provision your own keypair before shipment"
#else
#warning "QBOOT: repository default public key in use - provision your own keypair before shipment"
#endif
#endif

/* The provisioned verification key. Blank (erased) keeps the gate off. */
static const uint8_t pubkey[QUILLA_SECURE_BOOT_KEY_SIZE] =
  QUILLA_SECURE_BOOT_PUBKEY;

/* Chunk size for streaming the image through SHA-256. */
#define SB_HASH_CHUNK  64U

int
quilla_qboot_secure_pubkey_active(void)
{
  uint32_t i;
  uint8_t all_ff = 1u, all_00 = 1u;

  for(i = 0; i < QUILLA_SECURE_BOOT_KEY_SIZE; i++) {
    if(pubkey[i] != 0xFFU) {
      all_ff = 0u;
    }
    if(pubkey[i] != 0x00U) {
      all_00 = 0u;
    }
  }
  return (all_ff || all_00) ? 0 : 1;
}

void
quilla_qboot_secure_hash_range(uint32_t addr, uint32_t len,
                              uint8_t out[QUILLA_SECURE_BOOT_HASH_SIZE])
{
  sha256_ctx_t ctx;
  uint8_t chunk[SB_HASH_CHUNK];
  uint32_t done = 0;

  sha256_init(&ctx);
  while(done < len) {
    uint32_t n = len - done;
    if(n > SB_HASH_CHUNK) {
      n = SB_HASH_CHUNK;
    }
    quilla_qboot_hal_flash_read(addr + done, chunk, n);
    sha256_update(&ctx, chunk, n);
    done += n;
  }
  sha256_final(&ctx, out);
}

int
quilla_qboot_secure_verify_descriptor(uint32_t image_len,
                                     const uint8_t hash[QUILLA_SECURE_BOOT_HASH_SIZE],
                                     const uint8_t sig[QUILLA_SECURE_BOOT_SIG_SIZE])
{
  uint8_t msg[QUILLA_SECURE_BOOT_SIGNED_LEN];

  if(!quilla_qboot_secure_pubkey_active()) {
    return 0;
  }

  /* Descriptor layout: image_len || digest, all little-endian. The version is
   * not a descriptor field, but it is covered by the digest (it lives inside
   * image_len), so authenticity and freshness bind to the same signed bytes.
   * See the Freshness gate above and doc/secure-boot.md. */
  msg[0] = (uint8_t)(image_len >> 0);
  msg[1] = (uint8_t)(image_len >> 8);
  msg[2] = (uint8_t)(image_len >> 16);
  msg[3] = (uint8_t)(image_len >> 24);
  (void)memcpy(msg + 4, hash, QUILLA_SECURE_BOOT_HASH_SIZE);

  int rc = quilla_ed25519_verify(sig, QUILLA_SECURE_BOOT_SIG_SIZE,
                                 msg, QUILLA_SECURE_BOOT_SIGNED_LEN, pubkey);
  return rc;
}

int
quilla_qboot_secure_verify_app(void)
{
  quilla_meta_state_t s;
  uint8_t hash[QUILLA_SECURE_BOOT_HASH_SIZE];
  uint32_t version, image_len;
  uint32_t max_len;

  quilla_qboot_meta_get(&s);

  if(s.has_manifest == 0U) {
    return QUILLA_SB_ERR_NO_MANIFEST;
  }

  version   = quilla_qboot_secure_fw_version();
  image_len = s.mani_image_len;

  /* The signed image must end before the META region, otherwise an image that
   * overran would overlap the boot metadata. */
  max_len = QUILLA_SECURE_BOOT_MAX_IMAGE_LEN;
  if(image_len == 0U || image_len > max_len) {
    return QUILLA_SB_ERR_LEN;
  }

  quilla_qboot_secure_hash_range(QUILLA_QBOOT_APP_START, image_len, hash);

  if(memcmp(hash, s.mani_hash, QUILLA_SECURE_BOOT_HASH_SIZE) != 0) {
    return QUILLA_SB_ERR_HASH;
  }

  /* INTEGRITY-ONLY (CHECK) mode: the digest matches the manifest, so the
   * firmware was not corrupted or truncated in transit. No signature is
   * checked, so this does NOT authenticate the image's origin. Anti-rollback is
   * deliberately NOT enforced here: without a signature the floor cannot be
   * trusted. The caller reports QUILLA_SB_OK_INTEGRITY_ONLY -- never "secure
   * boot". */
#if QUILLA_SB_BOOT_MODE == QUILLA_SB_BOOT_MODE_CHECK
  return QUILLA_SB_OK_INTEGRITY_ONLY;
#endif

  /* Authenticity modes (SECURE / SEALED) require a provisioned key. With no key
   * the gate is inactive and any mechanically valid image is accepted -- the
   * legacy pre-secure-boot behaviour. */
  if(!quilla_qboot_secure_pubkey_active()) {
    return QUILLA_SB_INACTIVE;
  }

#if QUILLA_SB_BOOT_MODE == QUILLA_SB_BOOT_MODE_SECURE
  /* SECURE: verify the signature on EVERY boot. No seal shortcut, no seal
   * write -- trust is re-established from scratch each power-up. */
  if(!quilla_qboot_secure_verify_descriptor(image_len, hash,
                                           s.mani_sig)) {
    return QUILLA_SB_ERR_SIG;
  }
  if(version < quilla_qboot_secure_min_version_get()) {
    return QUILLA_SB_ERR_ROLLBACK;
  }
  (void)quilla_qboot_secure_min_version_set(version);
  return QUILLA_SB_OK;
#else
  /* SEALED (default): prefer the sealed fast path. A valid seal whose digest
   * matches the image we just hashed means this exact image already passed
   * Ed25519 on a prior boot or at install, so skip the ~20 s verify now.
   * The seal is trusted only because (a) it is written solely by the
   * authenticated SIGN path and (b) the debug interface is locked (RDP). */
  if(s.has_seal != 0U &&
     s.seal_len == image_len &&
     memcmp(s.seal_hash, hash, QUILLA_SECURE_BOOT_HASH_SIZE) == 0) {
    if(version < quilla_qboot_secure_min_version_get()) {
      return QUILLA_SB_ERR_ROLLBACK;
    }
    return QUILLA_SB_OK_SEALED;     /* authentic via seal; Ed25519 skipped */
  }

  /* No valid seal (first boot on a freshly programmed unit, a direct SWD
   * reflash, or a lost seal): run the full authenticity check. This is the
   * only place Ed25519 runs at boot, and on success we persist the seal so
   * every later boot of this image is fast. */
  if(!quilla_qboot_secure_verify_descriptor(image_len, hash,
                                           s.mani_sig)) {
    return QUILLA_SB_ERR_SIG;
  }
  (void)quilla_qboot_secure_seal_set(hash, image_len);

  if(version < quilla_qboot_secure_min_version_get()) {
    return QUILLA_SB_ERR_ROLLBACK;
  }

  /* Ratchet the floor. Best effort: a failed ratchet must not brick a device
   * whose image is otherwise authentic, so the signature remains the gate. */
  (void)quilla_qboot_secure_min_version_set(version);
  return QUILLA_SB_OK;
#endif
}

uint32_t
quilla_qboot_secure_min_version_get(void)
{
  quilla_meta_state_t s;
  uint32_t floor;

  quilla_qboot_meta_get(&s);
  floor = s.floor;

#if QUILLA_QBOOT_OTP_COUNTER
  /* Hardware monotonic counter: the OTP value is a second, tam-resistant
   * floor. An attacker who erases/rewrites the rewritable META region can
   * replay an older but genuinely signed manifest+image pair; the OTP bytes
   * cannot be erased, so the burned count still rejects that replay. The
   * effective floor is the higher of the two carriers. */
  {
    uint32_t otp = quilla_qboot_otp_counter_get();
    if(otp > floor) {
      floor = otp;
    }
  }
#endif

  return floor;
}

int
quilla_qboot_secure_min_version_set(uint32_t version)
{
  int rc;

  /* quilla_qboot_meta_set_floor() itself skips the commit when version is at or
   * below the current floor, so a pointless record is never written. */
  rc = quilla_qboot_meta_set_floor(version);

#if QUILLA_QBOOT_OTP_COUNTER
  /* Ratchet the hardware monotonic counter to the same value. Best effort,
   * like the META ratchet: a failed burn must not brick a device whose image
   * is otherwise authentic, so the signature remains the gate. Capacity
   * exhaustion (return 1) hands future state over to the META flash floor;
   * see quilla-qboot-otp.c and doc/anti-rollback-counter.md. */
  (void)quilla_qboot_otp_floor_ratchet(version);
#endif

  return rc;
}

int
quilla_qboot_secure_seal_get(uint8_t hash_out[QUILLA_SECURE_BOOT_HASH_SIZE],
                             uint32_t *len_out)
{
  quilla_meta_state_t s;
  quilla_qboot_meta_get(&s);

  if(s.has_seal == 0U) {
    return 0;
  }
  if(hash_out != NULL) {
    (void)memcpy(hash_out, s.seal_hash, QUILLA_SECURE_BOOT_HASH_SIZE);
  }
  if(len_out != NULL) {
    *len_out = s.seal_len;
  }
  return 1;
}

int
quilla_qboot_secure_seal_set(const uint8_t hash[QUILLA_SECURE_BOOT_HASH_SIZE],
                             uint32_t len)
{
  return quilla_qboot_meta_set_seal(hash, len);
}

/* The APP places the security-version region at QUILLA_APP_SECVER_ADDR (see
 * quilla-version.h); QBOOT must read exactly the same address. Cross-check the
 * two derivations here, where both are in scope, so a partition change that
 * moves one but not the other fails the build instead of silently breaking the
 * anti-rollback gate. */
#if QUILLA_APP_SECVER_ADDR != (QUILLA_QBOOT_APP_START + QUILLA_APP_SECVER_OFF)
#error "QUILLA_APP_SECVER_ADDR != QUILLA_QBOOT_APP_START + QUILLA_APP_SECVER_OFF"
#endif

uint32_t
quilla_qboot_secure_fw_version(void)
{
  uint32_t addr = QUILLA_QBOOT_APP_START + QUILLA_APP_SECVER_OFF;
  uint32_t magic, version;

  quilla_qboot_hal_flash_read(addr, (uint8_t *)&magic, 4U);
  if(magic != QUILLA_APP_SECVER_MAGIC_U32) {
    return 0U;                         /* region missing / corrupted */
  }
  quilla_qboot_hal_flash_read(addr + 4U, (uint8_t *)&version, 4U);
  return version;
}

uint32_t
quilla_qboot_secure_app_version(void)
{
  return quilla_qboot_secure_fw_version();
}

/** @} */
/** @} */

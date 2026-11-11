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
 * \defgroup quilla-qboot-secure Secure boot policy (signature + anti-rollback)
 *
 * Policy layer that decides whether the QBOOT may hand control to the APP
 * image. Two independent gates:
 *
 * 1. Authenticity - the APP image is covered by an Ed25519 signature held in
 *    a fixed manifest at the top of the APP region. The key lives in the
 *    QBOOT image, so it is immutable for a given QBOOT build.
 * 2. Freshness    - the firmware image carries its own security version,
 *    baked at a fixed address (QUILLA_QBOOT_APP_START + QUILLA_APP_SECVER_OFF)
 *    and covered by the SHA-256 / signature, so it cannot be forged without
 *    the private key. QBOOT persists a monotonic "floor" in the META record;
 *    at boot it compares the firmware security version against the floor and
 *    rejects an older image. On a successful, authenticated upgrade the floor
 *    is raised to the firmware version. Both the host tool and the firmware
 *    image can influence the version only through a genuine (signed) release.
 *
 * An unprovisioned (all-0xFF) key area means "secure boot inactive": the
 * gate reports QUILLA_SB_INACTIVE and the caller keeps the legacy behaviour of
 * booting any mechanically valid image. This keeps existing field units and
 * the development flow working; provisioning the key is what turns the gate on.
 *
 * The module is hardware independent: Flash access goes through
 * quilla-qboot-hal.h only.
 *
 * @{
 */

#ifndef QUILLA_SECURE_BOOT_H_
#define QUILLA_SECURE_BOOT_H_

/* ---- QBOOT product configuration -----------------------------------------
 *
 * QUILLA_QBOOT_HW_COUNTER_LEVEL selects the hardware monotonic-counter
 * carrier. It is a PRODUCT decision and is defined here, in the QBOOT policy
 * header. Do NOT pass it with -D from Keil .uvprojx, EIDE or the Makefiles:
 * the build projects carry no QBOOT policy. soc.h declares physical
 * capability only.
 *
 * This block sits BEFORE the quilla-qboot-hal.h include so that hal.h can
 * resolve the level when it derives the internal carrier macros. Include
 * guards make both entry paths (include hal.h first, or secure.h first)
 * converge on exactly one definition.
 * ------------------------------------------------------------------------- */
#include "soc.h"

#define QUILLA_QBOOT_HW_COUNTER_NONE  0
#define QUILLA_QBOOT_HW_COUNTER_WPP   1
#define QUILLA_QBOOT_HW_COUNTER_OTP   2

/* On-target validation switch for true-OTP parts (GD32F502VE).
 * Set to 1 to select LEVEL=OTP while validating the true-OTP counter path.
 * True-OTP cells are irreversible; set back to 0 for production, where
 * true-OTP parts fall back to NONE. PY32/CH32 always derive WPP. */
#define QUILLA_QBOOT_CONF_VALIDATE_TRUE_OTP  1

#ifndef QUILLA_QBOOT_HW_COUNTER_LEVEL
#if defined(SOC_OTP_AVAILABLE) && (SOC_OTP_AVAILABLE != 0) && \
    (QUILLA_QBOOT_CONF_VALIDATE_TRUE_OTP != 0)
#define QUILLA_QBOOT_HW_COUNTER_LEVEL  QUILLA_QBOOT_HW_COUNTER_OTP
#elif defined(SOC_WPP_AVAILABLE) && (SOC_WPP_AVAILABLE != 0)
#define QUILLA_QBOOT_HW_COUNTER_LEVEL  QUILLA_QBOOT_HW_COUNTER_WPP
#else
#define QUILLA_QBOOT_HW_COUNTER_LEVEL  QUILLA_QBOOT_HW_COUNTER_NONE
#endif
#endif

/* ---- hardware carrier resolution -----------------------------------------
 *
 * Resolves QUILLA_QBOOT_HW_COUNTER_LEVEL (policy above) against the physical
 * capability declared by the port's soc.h, and derives the internal carrier
 * macros consumed by quilla-qboot-otp.c / quilla-qboot-secure.c / the APP_END
 * partition logic in quilla-qboot-hal.h. The two hardware carriers are
 * documented in doc/anti-rollback-counter.md section 2.4.
 * ------------------------------------------------------------------------- */

/* ---- hardware monotonic counter feature level ----------------------------
 *
 * QUILLA_QBOOT_HW_COUNTER_LEVEL and its three level constants are defined
 * above in this file. Do not define them in the build projects or in soc.h:
 * soc.h declares physical capability only.
 *
 * This section resolves the configured level against that capability and
 * derives the internal carrier macros consumed by quilla-qboot-otp.c /
 * quilla-qboot-secure.c. The two hardware carriers are documented in
 * doc/anti-rollback-counter.md section 2.4.
 * ------------------------------------------------------------------------ */

/* Physical carrier availability, declared by the port in soc.h. */
#ifdef SOC_OTP_AVAILABLE
#define QUILLA_QBOOT_OTP_AVAIL   SOC_OTP_AVAILABLE
#else
#define QUILLA_QBOOT_OTP_AVAIL   0
#endif

#ifdef SOC_WPP_AVAILABLE
#define QUILLA_QBOOT_WPP_AVAIL  SOC_WPP_AVAILABLE
#else
#define QUILLA_QBOOT_WPP_AVAIL  0
#endif

/* 1 when any counter carrier exists (true OTP or WRP-protected flash). Retained
 * so the per-MCU HAL (e.g. gd32f502ve/quilla-qboot-hal.c) can still gate its
 * real OTP programming routines on physical capability. */
#define QUILLA_QBOOT_SECPROG_AVAILABLE \
  (QUILLA_QBOOT_OTP_AVAIL || QUILLA_QBOOT_WPP_AVAIL)

/* Resolve the configured level against the declared hardware. The selected
 * carrier macros are what quilla-qboot-otp.c / -secure.c key on: a requested
 * carrier that the hardware does not provide resolves to neither (the build
 * warns about the mismatch in quilla-qboot-otp.c). */
#if QUILLA_QBOOT_HW_COUNTER_LEVEL == QUILLA_QBOOT_HW_COUNTER_OTP
#if QUILLA_QBOOT_OTP_AVAIL
#define QUILLA_QBOOT_OTP_HW   1   /* selected: true OTP */
#define QUILLA_QBOOT_WPP    0
#else
#define QUILLA_QBOOT_OTP_HW   0
#define QUILLA_QBOOT_WPP    0   /* OTP requested but unavailable */
#endif
#elif QUILLA_QBOOT_HW_COUNTER_LEVEL == QUILLA_QBOOT_HW_COUNTER_WPP
#if QUILLA_QBOOT_WPP_AVAIL
#define QUILLA_QBOOT_OTP_HW   0
#define QUILLA_QBOOT_WPP    1   /* selected: write-protected programmable (WPP) */
#else
#define QUILLA_QBOOT_OTP_HW   0
#define QUILLA_QBOOT_WPP    0   /* WPP requested but unavailable */
#endif
#else /* QUILLA_QBOOT_HW_COUNTER_NONE */
#define QUILLA_QBOOT_OTP_HW   0
#define QUILLA_QBOOT_WPP    0
#endif

/* Internal enable switch: compiled-in AND a carrier is actually selected.
 * Derived from QUILLA_QBOOT_HW_COUNTER_LEVEL and the declared hardware; never
 * set directly by a port. */
#if (QUILLA_QBOOT_OTP_HW + QUILLA_QBOOT_WPP)
#define QUILLA_QBOOT_OTP_COUNTER   1
#else
#define QUILLA_QBOOT_OTP_COUNTER   0
#endif

/* Base/size come from whichever carrier is actually selected. */
#if QUILLA_QBOOT_OTP_HW
#ifndef SOC_OTP_BASE
#error "QUILLA_QBOOT_HW_COUNTER_LEVEL=OTP requires SOC_OTP_BASE in arch/cpu/<arch>/<mcu>/soc.h"
#endif
#ifndef SOC_OTP_SIZE
#error "QUILLA_QBOOT_HW_COUNTER_LEVEL=OTP requires SOC_OTP_SIZE in arch/cpu/<arch>/<mcu>/soc.h"
#endif
#define QUILLA_QBOOT_OTP_BASE          SOC_OTP_BASE
#define QUILLA_QBOOT_OTP_SIZE          SOC_OTP_SIZE
#elif QUILLA_QBOOT_WPP
#ifndef SOC_OTP_WRPROT_BASE
#error "QUILLA_QBOOT_HW_COUNTER_LEVEL=WPP requires SOC_OTP_WRPROT_BASE in arch/cpu/<arch>/<mcu>/soc.h"
#endif
#ifndef SOC_OTP_WRPROT_SIZE
#error "QUILLA_QBOOT_HW_COUNTER_LEVEL=WPP requires SOC_OTP_WRPROT_SIZE in arch/cpu/<arch>/<mcu>/soc.h"
#endif
#define QUILLA_QBOOT_OTP_BASE          SOC_OTP_WRPROT_BASE
#define QUILLA_QBOOT_OTP_SIZE          SOC_OTP_WRPROT_SIZE
#endif /* selected carrier */

/* Counter cell granularity (bytes per program unit). Defaults to 1 (byte
 * granular, the GD32 OTP case) when the port does not declare it. */
#ifdef SOC_OTP_CELL_SIZE
#define QUILLA_QBOOT_OTP_CELL_SIZE     SOC_OTP_CELL_SIZE
#else
#define QUILLA_QBOOT_OTP_CELL_SIZE     1
#endif


#include <stdint.h>

#include "quilla-qboot-hal.h"

#include "quilla-version.h"   /* QUILLA_APP_SECURITY_VERSION / QUILLA_APP_SECVER_OFF */

/* Provisioned key. Included before the all-0xFF default so the repository
 * default branch (or a product branch you add there) bakes a real key; with
 * neither, the erased-key default below leaves the gate inactive. */
#include "quilla-qboot-pubkey.h"

/* ---- sizes --------------------------------------------------------------- */

#define QUILLA_SECURE_BOOT_KEY_SIZE     32U
#define QUILLA_SECURE_BOOT_SIG_SIZE     64U
#define QUILLA_SECURE_BOOT_HASH_SIZE    32U

/* Bytes covered by the signature: image_len(4) || sha256(32). The firmware
 * security version lives INSIDE image_len (at QUILLA_QBOOT_APP_START +
 * QUILLA_APP_SECVER_OFF), so it is covered by the digest and therefore by the
 * signature -- it is not a separately signed field. */
#define QUILLA_SECURE_BOOT_SIGNED_LEN   36U

/* ---- manifest ------------------------------------------------------------
 * The manifest is no longer a fixed Flash page. It is the manifest block inside
 * the wear-levelled META record (see quilla-qboot-meta.h): each 256 B record
 * carries a 64 B meta block (QUILLA_META_BLK_MAGIC "QMTL") and a 128 B manifest
 * block (QUILLA_MANI_BLK_MAGIC "QMF1") at QUILLA_MANI_BLK_OFF. Field offsets and
 * the block CRC are defined there, not here.
 *
 * The magic value "QMF1" is deliberately unchanged from the old fixed-page
 * layout so a freshly programmed device whose META is all-0xFF is never misread
 * as a valid manifest. The manifest is written only when an image is installed
 * (the QIAP SIGN / integrity-only END path via quilla_qboot_meta_set_manifest);
 * boot-time reads go through quilla_qboot_meta_get(). */

/* Largest image that can be signed: the whole APP region. */
#define QUILLA_SECURE_BOOT_MAX_IMAGE_LEN \
  (QUILLA_QBOOT_APP_END - QUILLA_QBOOT_APP_START + 1U)

/* (manifest field offsets moved to quilla-qboot-meta.h: QUILLA_MANI_OFF_*) */

/* ---- provisioned public key ---------------------------------------------- */

/* Override per product, e.g.
 *   -DQUILLA_SECURE_BOOT_PUBKEY={0x11,0x22,...,0x33}
 * The default is the erased (all-0xFF) key, which disables the gate. */
#ifndef QUILLA_SECURE_BOOT_PUBKEY
#define QUILLA_SECURE_BOOT_PUBKEY                                             \
  { 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,                   \
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,                   \
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,                   \
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU }
#endif

/* ---- boot-time verification MODE ----------------------------------------
 *
 * The boot gate decides, before handing control to the APP, whether the APP
 * image is acceptable. Three mutually exclusive postures, selected by
 * QUILLA_SB_BOOT_MODE:
 *
 * QUILLA_SB_BOOT_MODE_SECURE
 *   Re-run the full Ed25519 signature verify on EVERY boot (~20 s on
 *   PY32F040xB @ 8 MHz). Strongest re-attestation: a reboot can never execute
 *   an image that was not signed by the provisioned key. Cost: ~20 s added to
 *   every boot -- unacceptable for devices with frequent resets / power
 *   glitches, and it inflates boot-time power draw. Select this only when the
 *   target IEC 62443 component profile / threat model requires re-verification
 *   on every startup (typically the higher SL bands). See doc/secure-boot.md.
 *
 * QUILLA_SB_BOOT_MODE_SEALED  (default)
 *   Ed25519 runs at most once per DISTINCT APP image -- at install (the QIAP
 *   SIGN path) or on the first authenticated boot of that image. The verified
 *   image digest is "sealed" into the flag page; subsequent boots skip
 *   Ed25519 and compare the freshly computed SHA-256 to the seal (~0.6 s). A
 *   changed image (SWD reflash, lost/old seal) falls back to the full
 *   Ed25519 check, and a rollback is still rejected by the floor. This is a
 *   SEALED / MEASURED boot. Its claim holds ONLY if the debug interface is
 *   locked (RDP): with SWD open an attacker rewrites APP + manifest + seal and
 *   the seal check is defeated. See the SEAL section below and
 *   doc/secure-boot.md.
 *
 * QUILLA_SB_BOOT_MODE_CHECK
 *   INTEGRITY ONLY, no signature. The image digest is compared against the
 *   digest recorded in the manifest, so a corrupted or truncated image (bad
 *   flash, interrupted upgrade, transport error) is rejected. This does NOT
 *   authenticate the image's origin: without a signature an attacker who can
 *   rewrite both APP and manifest passes the check. It is the right posture for
 *   development / field units that need transport integrity but have no secure
 *   boot key provisioned, and it never claims "secure boot".
 *
 * SECURE and SEALED require a provisioned key. With no key compiled the gate
 *   reports QUILLA_SB_INACTIVE (secure boot OFF) regardless of mode -- see
 *   quilla_qboot_secure_pubkey_active(). */
#define QUILLA_SB_BOOT_MODE_SECURE   0
#define QUILLA_SB_BOOT_MODE_SEALED  1
#define QUILLA_SB_BOOT_MODE_CHECK   2

#ifndef QUILLA_SB_BOOT_MODE
#define QUILLA_SB_BOOT_MODE  QUILLA_SB_BOOT_MODE_SEALED
#endif

/* Boot banner variant tag. Derived from the mode so build projects specify
 * QUILLA_SB_BOOT_MODE once and never carry a duplicate string define. An
 * unsupported mode fails the build instead of printing a misleading tag. */
#if QUILLA_SB_BOOT_MODE == QUILLA_SB_BOOT_MODE_CHECK
#define QUILLA_QBOOT_VARIANT_STR  "CHECK"
#elif QUILLA_SB_BOOT_MODE == QUILLA_SB_BOOT_MODE_SEALED
#define QUILLA_QBOOT_VARIANT_STR  "SEALED"
#elif QUILLA_SB_BOOT_MODE == QUILLA_SB_BOOT_MODE_SECURE
#define QUILLA_QBOOT_VARIANT_STR  "SECURED"
#else
#error "QUILLA_SB_BOOT_MODE must be CHECK, SEALED or SECURE"
#endif

/* ---- anti-rollback floor -------------------------------------------------
 * The floor is a field of the meta block (QUILLA_META_OFF_FLOOR), not a separate
 * page: quilla_qboot_meta_set_floor() appends a META record carrying it, and the
 * flag / manifest in that record are carried forward rather than erased. */

/* "QMVF" was the legacy floor magic; the floor is now just a field in the QMTL
 * meta block (QUILLA_META_OFF_FLOOR) with no per-field magic of its own. */

/* ---- sealed-image record (sealed boot) -----------------------------------
 * On a device that provisions a key, the Ed25519 verify (~20 s on PY32F040xB)
 * is run at most once per distinct APP image. The first authenticated boot (or
 * the QIAP SIGN install path) writes a seal into the META record (a field of
 * the QMTL meta block: QUILLA_META_OFF_SEAL_HASH / _LEN / _VER) recording the
 * SHA-256 of the image it verified. Subsequent boots skip Ed25519 and compare
 * the freshly computed digest to the seal instead -- the digest cost (~0.6 s)
 * is unchanged, the 20 s signature check is gone.
 *
 * This is a SEALED / MEASURED boot, not a per-power-up signature check. The
 * seal is authoritative ONLY because the META record is written solely by the
 * authenticated QIAP SIGN path AND the debug interface is locked (RDP). With
 * SWD open an attacker could rewrite APP + manifest + seal, so the secure-boot
 * claim REQUIRES the chip read-protection to be enabled in production.
 *
 * "QSL1" was the legacy seal magic; the seal is now just fields of the QMTL
 * meta block with no per-field magic of its own. */

/* ---- results ------------------------------------------------------------- */

#define QUILLA_SB_OK              0    /* APP authentic and fresh            */
#define QUILLA_SB_INACTIVE        1    /* no key: unsigned APP allowed       */
/* Digest matched, but the SIGNATURE WAS NOT CHECKED in this pass. Historically
 * produced by the dev-only "skip signature at boot" build; with the current
 * SECURE / SEALED modes this code is no longer returned -- a build without a
 * provisioned key returns QUILLA_SB_INACTIVE instead. Retained for API
 * stability. Callers that accept this code MUST say "integrity only". */
#define QUILLA_SB_OK_INTEGRITY_ONLY 2
/* Authentic via a trusted seal: the image previously passed Ed25519 on a prior
 * boot or at install, so this boot skipped the signature check. Distinct from
 * QUILLA_SB_OK only for boot-log clarity; both mean "authentic". */
#define QUILLA_SB_OK_SEALED       3
#define QUILLA_SB_ERR_NO_MANIFEST (-1) /* no manifest / bad magic            */
#define QUILLA_SB_ERR_LEN         (-2) /* image length out of range          */
#define QUILLA_SB_ERR_HASH        (-3) /* image digest != manifest digest    */
#define QUILLA_SB_ERR_SIG         (-4) /* signature does not verify          */
#define QUILLA_SB_ERR_ROLLBACK    (-5) /* version below the persisted floor  */

/**
 * \brief  Is a verification key provisioned?
 * \return 1 if a key is present (gate on), 0 if the key area is blank
 *
 * Blank means all-0xFF (erased) or all-0x00. With no key the device keeps the
 * pre-secure-boot behaviour of accepting any mechanically valid APP.
 */
int quilla_qboot_secure_pubkey_active(void);

/**
 * \brief     Verify a signature over an image descriptor
 * \param image_len  Byte count the digest was computed over
 * \param hash       SHA-256 of the image (32 bytes)
 * \param sig        Ed25519 signature (64 bytes)
 * \retval 1 signature valid
 * \retval 0 invalid, or no key provisioned
 *
 * Shared by the boot-time gate and by the SIGN frame of the upgrade protocol,
 * so both paths enforce exactly the same binding of length and content. The
 * firmware security version is NOT passed here: it lives inside image_len (at
 * QUILLA_QBOOT_APP_START + QUILLA_APP_SECVER_OFF) and is bound indirectly by
 * the SHA-256 digest and the signature. Neither the host nor QBOOT can set it;
 * only a genuine (signed) release fixes its value. The anti-rollback gate reads
 * it via quilla_qboot_secure_fw_version() and compares it to the persisted
 * floor (see the Freshness gate above).
 */
int quilla_qboot_secure_verify_descriptor(uint32_t image_len,
                                         const uint8_t hash[QUILLA_SECURE_BOOT_HASH_SIZE],
                                         const uint8_t sig[QUILLA_SECURE_BOOT_SIG_SIZE]);

/**
 * \brief      Hash a range of Flash through the boot HAL
 * \param addr Start address
 * \param len  Byte count
 * \param out  32-byte SHA-256 destination
 */
void quilla_qboot_secure_hash_range(uint32_t addr, uint32_t len,
                                   uint8_t out[QUILLA_SECURE_BOOT_HASH_SIZE]);

/**
 * \brief  Check the APP image against the secure-boot policy
 * \return QUILLA_SB_OK, QUILLA_SB_INACTIVE, or a negative QUILLA_SB_ERR_*
 *
 * Reads the manifest, re-hashes the image it describes, verifies the
 * signature and enforces the anti-rollback floor. On QUILLA_SB_OK the floor
 * has been raised to this image's version.
 */
int quilla_qboot_secure_verify_app(void);

/**
 * \brief  Read the anti-rollback floor
 * \return The persisted minimum version, or 0 when never written
 */
uint32_t quilla_qboot_secure_min_version_get(void);

/**
 * \brief      Raise the anti-rollback floor
 * \param version New floor; values below the current floor are ignored
 * \retval 0   written, or already at or above \p version
 * \retval <0  Flash write failed
 */
int quilla_qboot_secure_min_version_set(uint32_t version);

/**
 * \brief  Read the firmware security version baked into the APP image
 * \return The security version from the signed image region, or 0 if the
 *         region is missing / corrupted
 *
 * This is the value the anti-rollback gate compares against the floor. It is
 * taken from the signed image region (not the META manifest), so it cannot be
 * forged without the private key.
 */
uint32_t quilla_qboot_secure_fw_version(void);

/**
 * \brief  Read the security version of the installed APP (for diagnostics)
 * \return The firmware security version, or 0 if the region is missing
 *
 * Convenience alias of quilla_qboot_secure_fw_version() for the pinfo command.
 */
uint32_t quilla_qboot_secure_app_version(void);

/**
 * \brief   Read the sealed-image record from the flag page
 * \param hash_out  Receives the 32-byte sealed digest (only if non-NULL and a
 *                  valid seal is present)
 * \param len_out   Receives the sealed image length (only if non-NULL)
 * \retval 1 a valid seal is present
 * \retval 0 no seal (unprogrammed page, or a different magic)
 */
int quilla_qboot_secure_seal_get(uint8_t hash_out[QUILLA_SECURE_BOOT_HASH_SIZE],
                                 uint32_t *len_out);

/**
 * \brief      Persist a seal after an authenticated image
 * \param hash  SHA-256 of the authenticated image (32 bytes)
 * \param len   Authenticated image length
 * \retval 0   written
 * \retval <0  Flash error (the caller must treat this as a failed commit)
 *
 * Appends a META record carrying the seal (quilla_qboot_meta_set_seal); the
 * interaction flag and the anti-rollback floor are carried forward by the
 * record, not erased. Called from the QIAP SIGN path and from the first
 * authenticated boot of a new image.
 */
int quilla_qboot_secure_seal_set(const uint8_t hash[QUILLA_SECURE_BOOT_HASH_SIZE],
                                 uint32_t len);

#endif /* QUILLA_SECURE_BOOT_H_ */
/** @} */
/** @} */

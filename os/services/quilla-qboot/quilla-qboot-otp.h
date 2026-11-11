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
 * ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * This file is part of the Quilla operating system.
 */

/**
 * \addtogroup quilla-qboot
 * @{
 *
 * \defgroup quilla-qboot-otp Hardware monotonic counter (OTP-backed)
 *
 * The anti-rollback floor of the secure-boot gate lives in the META flash log
 * (quilla-qboot-meta.c), which is ordinary rewritable flash: anyone who can
 * erase the META region can replay any OLDER but genuinely signed manifest /
 * image pair, and the signature gate alone cannot detect that. A
 * hardware-backed monotonic counter closes the same gap: eFuse bits where the
 * silicon provides them, or a flash-simulated NV counter where it does not.
 * This module gives QBOOT the equivalent for MCUs that provide a
 * one-time-programmable area.
 *
 * Storage scheme (see doc/anti-rollback-counter.md):
 *
 *   - Some designs store the security counter as a 4-byte uint32 TLV in the
 *     image and let the platform implement the update; their eFuse-based ports
 *     consume 1 fuse BIT per increment, so capacity = available fuse bits.
 *   - GD32 OTP cells are byte-granular and accept exactly ONE program per
 *     cell, so this module consumes 1 OTP BYTE per version increment: the
 *     counter value equals the number of leading non-0xFF bytes in the
 *     reserved OTP region. Capacity = (reserved bytes - 1 marker byte), e.g.
 *     255 increments on the GD32F502VE 256-byte OTP2 block. The comparison
 *     with those schemes' byte budget is documented in
 *     doc/anti-rollback-counter.md.
 *
 * Floor semantics: the counter value IS a firmware security version -- the
 * boot gate and the QIAP SIGN path compare the image version against
 * max(META floor, OTP counter) through quilla_qboot_secure_min_version_get().
 * Every release must therefore carry a QUILLA_APP_SECURITY_VERSION that is
 * strictly sequential (+1 per release): skipping version numbers burns one
 * OTP byte per skipped number.
 *
 * Exhaustion: OTP capacity is finite. When a ratchet does not fit into the
 * remaining cells, the module burns the marker byte (the LAST byte of the
 * region) after read-back confirmation and returns 1; from then on the
 * rewritable META floor is the sole carrier of anti-rollback state, exactly
 * the pre-OTP behaviour. The marker lives in OTP so a replay of the META
 * region cannot resurrect the OTP path. The already-burned counter bytes
 * remain valid as a floor forever.
 *
 * Enablement (see the warning policy in quilla-qboot-otp.c):
 *
 *   The feature level QUILLA_QBOOT_HW_COUNTER_LEVEL is a Quilla system-level
 *   decision defined in quilla-qboot-secure.h (NOT in the per-MCU soc.h and
 *   NOT in the build projects). It
 *   takes one of NONE / WPP / OTP, and its default is derived from the MCU's
 *   declared capability (soc.h SOC_OTP_AVAILABLE / SOC_WPP_AVAILABLE).
 *   The internal enable flag QUILLA_QBOOT_OTP_COUNTER is derived from that
 *   level and the hardware in quilla-qboot-hal.h, and must not be set
 *   directly by a port.
 *
 * @{
 */

#ifndef QUILLA_QBOOT_OTP_H_
#define QUILLA_QBOOT_OTP_H_

#include <stdint.h>

#include "quilla-qboot-hal.h"

/* QUILLA_QBOOT_OTP_COUNTER is derived from the system-level feature level
 * (QUILLA_QBOOT_HW_COUNTER_LEVEL) and the MCU capability. It is not
 * defined here. */

/**
 * \brief  Read the OTP-backed monotonic counter
 * \return The counter value (== the lowest firmware security version every
 *         future image must meet or exceed), or 0 when OTP storage is not
 *         compiled in. The value persists across resets and can only grow.
 */
uint32_t quilla_qboot_otp_counter_get(void);

/**
 * \brief  Is the OTP counter exhausted?
 * \retval 1 the marker byte is burned: capacity is used up and the rewritable
 *           META flash floor is the sole carrier of future anti-rollback state
 * \retval 0 OTP capacity remains (or OTP storage is not compiled in)
 */
int quilla_qboot_otp_exhausted(void);

/**
 * \brief      Raise the OTP-backed counter to \p target (a firmware security
 *             version), burning one OTP byte per increment
 * \param target Desired counter value after the call
 * \retval 0   counter now reads >= \p target
 * \retval 1   capacity exhausted: the marker byte was burned after read-back
 *             confirmation and the META flash floor carries future state
 * \retval <0  OTP program/verify failure (best effort: the signature gate is
 *             unaffected, the META floor ratchet still happened)
 */
int quilla_qboot_otp_floor_ratchet(uint32_t target);

/**
 * \brief      Settle the hardware counter against the META floor at boot
 * \param floor Effective anti-rollback floor
 *             (quilla_qboot_secure_min_version_get())
 *
 * True-OTP parts (GD32) burn synchronously inside quilla_qboot_otp_floor_
 * ratchet() during QIAP SIGN, so this is a no-op for them. write-protected NV counter
 * parts (PY32 / CH32, SOC_WPP_AVAILABLE=1) cannot: opening the
 * write-protection window resets the chip. This routine therefore performs
 * the deferred burn at boot time:
 *
 *   counter < floor && region protected  -> lift protection (reset)
 *   counter < floor && region open       -> burn cells, re-protect (reset)
 *   counter >= floor or exhausted        -> return (no reset)
 *
 * The flow is idempotent and power-loss safe (each boot resumes from the
 * scan); a full upgrade costs two extra resets, ONLY when
 * QUILLA_QBOOT_OTP_COUNTER = 1 (i.e. LEVEL is not NONE); builds with the
 * counter disabled compile this to a no-op with zero resets and zero
 * option-byte wear. Call once from the boot path, before the secure-boot gate.
 */
void quilla_qboot_otp_sync(uint32_t floor);

#endif /* QUILLA_QBOOT_OTP_H_ */
/** @} */
/** @} */

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
 * Ed25519 signature verification (RFC 8032) for the Quilla secure-boot
 * chain.  Verification only -- the bootloader never signs; signing is the
 * host tool's job.  The implementation is constant-time-ish (it does not
 * branch on secret data; the only secret is the verification equation
 * itself) and uses a self-contained big-integer field arithmetic so it has
 * no dependency on any platform crypto and runs on Cortex-M0+.
 */

#ifndef QUILLA_ED25519_H
#define QUILLA_ED25519_H

#include <stdint.h>
#include <stddef.h>

/**
 * Verify an Ed25519 signature (RFC 8032) over an arbitrary message.
 *
 * \param sig     signature buffer, exactly 64 bytes (R || S).
 * \param siglen  length of \c sig (must be 64).
 * \param msg     message that was signed.
 * \param msglen  length of \c msg.
 * \param pk      public key, exactly 32 bytes.
 *
 * \return 1 if the signature is valid for (msg, pk), 0 otherwise.
 */
int quilla_ed25519_verify(const uint8_t *sig, size_t siglen,
                          const uint8_t *msg, size_t msglen,
                          const uint8_t *pk);

/** @} */

#endif /* QUILLA_ED25519_H */

/*
 * Copyright (c) 2026, JiChiRen Info. Tech.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the conditions of the BSD-3
 * license referenced in the project LICENSE file are met.
 *
 * This file is part of the Quilla operating system.
 */

/**
 * \addtogroup quilla-qboot
 * @{
 *
 * \defgroup quilla-app-secver APP-side anti-rollback support (OBFUSCATION ONLY)
 *
 * This header is included ONLY by the APP build (main.c). It places the
 * firmware security version at the fixed flash address the linkers reserve
 * (.quilla_secver, at QUILLA_QBOOT_APP_START + QUILLA_APP_SECVER_OFF) and, when
 * QUILLA_APP_SECVER_SELFCHECK is enabled, performs an early self-check that
 * refuses to run if the in-flash region is internally inconsistent.
 *
 * HONEST SCOPE -- read before relying on this:
 * This self-check is OBFUSCATION, not a cryptographic control. Its only job is
 * to raise the bar for an AUTHORIZED-KEY end user who would otherwise bump the
 * security version with a one-line edit and re-sign, defeating the QBOOT
 * anti-rollback gate. It does NOT stop a determined attacker who can re-sign and
 * patch both the flash region and the baked constant: both live in the signed
 * image and are covered by the signature, so a key holder can always produce a
 * consistent pair. Sealed / secure boot (signature + QBOOT floor) is the real
 * gate; production still requires disciplined private-key authorization, and a
 * hardware monotonic counter (eFuse/OTP) is the only control that stops a key
 * holder from rolling back. See doc/secure-boot.md ("Dual anti-rollback
 * mechanism").
 *
 * @{
 */

#ifndef QUILLA_APP_SECVER_H_
#define QUILLA_APP_SECVER_H_

#include <stdint.h>

#include "quilla-version.h"

#ifndef QUILLA_APP_SECVER_SELFCHECK
#define QUILLA_APP_SECVER_SELFCHECK  1
#endif

/* The region QBOOT reads: a 4-byte magic followed by the 32-bit security
 * version. Must match the layout QBOOT expects in quilla-qboot-secure.c. */
typedef struct {
  uint32_t magic;     /* QUILLA_APP_SECVER_MAGIC_U32 ("QAV1") */
  uint32_t version;   /* QUILLA_APP_SECURITY_VERSION */
} quilla_app_secver_t;

/* Forward declaration so quilla_app_secver_selfcheck() (defined below, in the
 * header) can resolve the region symbol regardless of whether the header is
 * included before or after QUILLA_APP_SECVER_PLACE() is invoked in main.c.
 * The symbol's storage (and its .quilla_secver section placement) is supplied
 * by exactly one QUILLA_APP_SECVER_PLACE() at file scope in the APP. */
extern const quilla_app_secver_t quilla_app_secver;

/* Place the region at the fixed flash address QBOOT reads
 * (QUILLA_APP_SECVER_ADDR == QUILLA_QBOOT_APP_START + QUILLA_APP_SECVER_OFF).
 * Invoke once at file scope in the APP (main.c). External linkage (not static)
 * so the forward declaration above is satisfied; no other translation unit
 * defines it, so there is no multiple-definition risk.
 *
 * Placement is toolchain-specific on purpose:
 *   armclang (Keil, ARM Compiler 6) -> a section named .ARM.__at_<addr>, which
 *     armlink places at that absolute address. This does NOT depend on the
 *     scatter file, so it survives Keil regenerating the auto-generated
 *     (gitignored) QAPP.sct on every build. (armclang has no __at() attribute;
 *     that is an ARM Compiler 5 / armcc feature, not Compiler 6.) A hand-edited
 *     .quilla_secver scatter region is therefore NOT used: it gets overwritten
 *     and the region silently falls back to an arbitrary .ANY (+RO) offset --
 *     exactly the GD32 failure this avoids.
 *   GCC (GNU EIDE) -> the .quilla_secver section, pinned by the APP linker
 *     script at 0x08007400 (the *_app.ld files). */
#define QUILLA_APP_SECVER_STR_(x)  #x
#define QUILLA_APP_SECVER_STR(x)   QUILLA_APP_SECVER_STR_(x)
#define QUILLA_APP_SECVER_ATSECT   ".ARM.__at_" QUILLA_APP_SECVER_STR(QUILLA_APP_SECVER_ADDR)

#if defined(__ARMCC_VERSION)
#define QUILLA_APP_SECVER_PLACE()                                          \
  const quilla_app_secver_t quilla_app_secver                              \
    __attribute__((section(QUILLA_APP_SECVER_ATSECT), used)) = {           \
      QUILLA_APP_SECVER_MAGIC_U32, QUILLA_APP_SECURITY_VERSION             \
    }
#else
#define QUILLA_APP_SECVER_PLACE()                                          \
  const quilla_app_secver_t quilla_app_secver                              \
    __attribute__((section(".quilla_secver"), used)) = {                   \
      QUILLA_APP_SECVER_MAGIC_U32, QUILLA_APP_SECURITY_VERSION             \
    }
#endif

#if QUILLA_APP_SECVER_SELFCHECK
/* Safe stop for the APP when the anti-rollback self-check fails. Disable
 * interrupts and park; this must never return. Compiler-agnostic inline asm so
 * the header needs no CMSIS dependency. */
static inline void
quilla_app_halt(void)
{
  __asm volatile ("cpsid i" : : : "memory");
  for(;;) {
    __asm volatile ("wfi" : : : "memory");
  }
}

/* Early self-check: the in-flash region must carry the expected magic and the
 * version must equal the value baked at compile time. A patcher who edits only
 * the flash region (or only the constant) trips this and the APP refuses to
 * run. quilla_app_halt() must never return. */
static inline void
quilla_app_secver_selfcheck(void)
{
  const quilla_app_secver_t *p = &quilla_app_secver;

  if(p->magic != QUILLA_APP_SECVER_MAGIC_U32) {
    quilla_app_halt();
  }
  if(p->version != (uint32_t)QUILLA_APP_SECURITY_VERSION) {
    quilla_app_halt();
  }
  /* Deliberate non-obvious recombination so a naive "search-and-replace the
   * version bytes" patch leaves a dead branch. This is still obfuscation, NOT a
   * security boundary. */
  if(((p->version ^ 0x5A5A5A5AU) + 1U) !=
     (((uint32_t)QUILLA_APP_SECURITY_VERSION ^ 0x5A5A5A5AU) + 1U)) {
    quilla_app_halt();
  }
}
#endif /* QUILLA_APP_SECVER_SELFCHECK */

#endif /* QUILLA_APP_SECVER_H_ */
/** @} */
/** @} */

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
 * \addtogroup quilla-version
 * @{
 *
 * \file
 * Quilla firmware version -- the single source of truth for the build version.
 *
 * Bump QUILLA_VERSION_MAJOR / QUILLA_VERSION_MINOR when a new firmware image
 * is released. Every version string/banner in the project derives from these
 * two numbers via QUILLA_VERSION_STRING, so there is never a second copy to
 * drift out of sync.
 *
 * @}
 */

#ifndef QUILLA_VERSION_H_
#define QUILLA_VERSION_H_

#include <stdint.h>

/* Quilla (OS + shell) version, semantic "major.minor.build".
 *
 * This is the version of the Quilla operating system and its shell. The QBOOT
 * (bootloader) banner prints it as "[BOOT] Quilla V<ver>". It is deliberately
 * kept separate from the application (project) version shown by QAPP, so the
 * two are never confused on the wire.
 *
 * major.minor is the released OS version. The QAPP / project version lives in
 * QUILLA_PROJECT_* below and is independent of this.
 *
 * build (xxxx) is the ONLY field bumped while developing Quilla: each firmware
 * change increments it, giving V0.11.0001, V0.11.0002, ... If the boot stamp
 * does not show the number you just built, the board is NOT running that image.
 *
 * NOTE: keep leading zeros (0001, 0002, ...) so the string form is always a
 * 4-digit field. The token is display-only -- do not evaluate it numerically,
 * because a leading zero makes it an octal literal in C. */
#define QUILLA_VERSION_MAJOR   0
#define QUILLA_VERSION_MINOR   11
#define QUILLA_VERSION_BUILD   0001

/* Packed 16-bit form: (major << 8) | minor. V0.11 -> 0x000B. This stays the
 * released version on purpose: it is what anti-rollback compares, so debug
 * iterations (build field) must not move it. */
#define QUILLA_VERSION \
  ((uint16_t)((QUILLA_VERSION_MAJOR << 8) | QUILLA_VERSION_MINOR))

/* Stringify helpers (two levels so the macros above are expanded first). */
#define QUILLA_VER_STR2(x)  #x
#define QUILLA_VER_STR(x)   QUILLA_VER_STR2(x)

/* Human-readable "major.minor.build" string -- single source, no second copy.
 * Adjacent literals concatenate, so it drops into any banner as-is. */
#define QUILLA_VERSION_STRING \
  QUILLA_VER_STR(QUILLA_VERSION_MAJOR) "." \
  QUILLA_VER_STR(QUILLA_VERSION_MINOR) "." \
  QUILLA_VER_STR(QUILLA_VERSION_BUILD)

/* ------------------------------------------------------------------ */
/* Application (project) version, shown by the QAPP banner as
 *   [QAPP] <QUILLA_PROJECT_NAME> V<major.minor.build>
 * QUILLA_PROJECT_NAME is overridable per product via -D so each project can
 * rename itself on the boot line (defaults to "Project"). The project version
 * is independent of the Quilla OS version above. */
#ifndef QUILLA_PROJECT_NAME
#define QUILLA_PROJECT_NAME        "Project"
#endif
#define QUILLA_PROJECT_VERSION_MAJOR   0
#define QUILLA_PROJECT_VERSION_MINOR   10
#define QUILLA_PROJECT_VERSION_BUILD   0001

#define QUILLA_PROJECT_VERSION_STRING \
  QUILLA_VER_STR(QUILLA_PROJECT_VERSION_MAJOR) "." \
  QUILLA_VER_STR(QUILLA_PROJECT_VERSION_MINOR) "." \
  QUILLA_VER_STR(QUILLA_PROJECT_VERSION_BUILD)

/* Boot-time diagnostics in QAPP (reset cause / watchdog / clock lines).
 * These are debug output and are OFF by default. Define QUILLA_APP_BOOT_DIAG
 * (e.g. pass -DQUILLA_APP_BOOT_DIAG on the compiler command line) to emit them
 * at boot. */

/* ------------------------------------------------------------------ */
/* Anti-rollback security version of the APP firmware image.
 *
 * This is the monotonic "epoch" the secure-boot anti-rollback gate compares
 * against the persisted floor. It is baked into the signed image region at a
 * fixed address (QUILLA_QBOOT_APP_START + QUILLA_APP_SECVER_OFF, see
 * quilla-qboot-secure.h) and is therefore covered by the image SHA-256 and the
 * Ed25519 signature -- it cannot be forged without the private key.
 *
 * BUMP THIS NUMBER ON EVERY FIRMWARE RELEASE THAT MUST NOT BE ROLLED BACK.
 * A downgrade to an image with a lower QUILLA_APP_SECURITY_VERSION is rejected
 * at boot (QUILLA_SB_ERR_ROLLBACK) and by the QIAP SIGN path. The value is 32
 * bits wide: start at 1 and never reuse a value for a different image. */
#ifndef QUILLA_APP_SECURITY_VERSION
#define QUILLA_APP_SECURITY_VERSION  1U
#endif

/* Offset (from QUILLA_QBOOT_APP_START) of the firmware security-version region
 * in the APP Flash. It sits well past the vector table and inside the real
 * image, so it is always covered by the signed digest. 0x400 == 1 KiB. */
#define QUILLA_APP_SECVER_OFF        0x400U

/* Magic of the firmware security-version region ("QAV1"). QBOOT reads this
 * before trusting the adjacent version word; a mismatch means the region was
 * not emitted (missing .quilla_secver section) and the version is reported as
 * 0 (which the gate treats as "not present / corrupt"). */
#define QUILLA_APP_SECVER_MAGIC_U32  0x31564151U   /* "QAV1" */

/* Absolute flash address of the security-version region: the APP places it
 * here, QBOOT reads it here. Kept self-contained (no partition-header
 * dependency) so the APP-side header works for every MCU and toolchain; the
 * QBOOT build cross-checks it against
 * QUILLA_QBOOT_APP_START + QUILLA_APP_SECVER_OFF (see quilla-qboot-secure.c).
 * Equals 0x08007000 (APP_START) + 0x400. No 'U' suffix so the value can be
 * stringified into the armclang .ARM.__at_<addr> section name (see
 * quilla-app-secver.h). */
#define QUILLA_APP_SECVER_ADDR       0x08007400

#endif /* QUILLA_VERSION_H_ */

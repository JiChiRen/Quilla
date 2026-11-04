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
 * Quilla META store: wear-levelled, append-only log for the QBOOT/APP
 * interaction flag, the anti-rollback floor, the sealed-image record and the
 * secure-boot manifest.
 *
 * Layout: the 4 KiB META region is split into two equal slots (SLOT_A / SLOT_B),
 * each QUILLA_QBOOT_META_SIZE/2 bytes. Every record is QUILLA_META_RECORD_SIZE
 * bytes (a fixed STRIDE chosen so a record lands on exactly one PY32 erase page
 * of 256 B; on GD32 it is a sub-page word-programmed block). A record carries
 * two independently readable blocks:
 *
 *   meta  block   (QUILLA_META_BLK_*)  -- flag / floor / seal, 64 B
 *   manifest block (QUILLA_MANI_BLK_*) -- version / len / hash / sig, 128 B
 *
 * Each block has its own magic and CRC-32, so a torn write to one does not
 * invalidate the other ("independent logs").
 *
 * Write policy (the reason this module exists): records are APPENDED to the
 * active slot with quilla_qboot_hal_flash_program (program only, never erase),
 * and a slot is only ever erased when it is the STANDBY (the slot that does NOT
 * hold the latest valid record). Therefore the slot that holds the only valid
 * copy is never erased during an operation, and a power loss at any point leaves
 * a fully readable record -- the flash cannot be corrupted by a mid-write erase
 * of the live region. The standby is pre-erased once the active slot holds more
 * than one record, so it is always clean before the active slot fills.
 *
 * Read policy: both slots are scanned forward to the first gap (bad magic /
 * CRC / sequence); the record with the highest sequence across both slots is
 * authoritative. The latest meta block and the latest manifest block are read
 * independently from that record.
 *
 * See quilla-qboot-meta.c for the implementation and quilla-qboot-hal.h for the
 * (non-erasing) program primitive this design depends on.
 * ----------------------------------------------------------------------------
 */

#ifndef QUILLA_QBOOT_META_H_
#define QUILLA_QBOOT_META_H_

#include <stdint.h>

#include "quilla-qboot-hal.h"
#include "quilla-qboot-secure.h"
#include "quilla-qboot-flag.h"

/* ---- geometry ------------------------------------------------------------ */

/* Record stride. 64 (meta) + 128 (manifest) = 192 B payload, padded to 256 so a
 * record aligns to one 256 B PY32 erase page and stays word aligned on GD32. */
#define QUILLA_META_RECORD_SIZE   256U
#define QUILLA_META_SLOT_SIZE     (QUILLA_QBOOT_META_SIZE / 2U)
#define QUILLA_META_SLOTS         2U
#define QUILLA_META_RECORDS_PER_SLOT \
  (QUILLA_META_SLOT_SIZE / QUILLA_META_RECORD_SIZE)

/* Block offsets/sizes inside a record. */
#define QUILLA_META_BLK_MAGIC     0x514D544CU   /* "QMTL" */
#define QUILLA_META_BLK_OFF       0U
#define QUILLA_META_BLK_SIZE      64U
#define QUILLA_MANI_BLK_MAGIC     0x514D4631U   /* "QMF1" (unchanged) */
#define QUILLA_MANI_BLK_OFF       64U
#define QUILLA_MANI_BLK_SIZE      128U

/* Field offsets within the meta block. */
#define QUILLA_META_OFF_MAGIC     0U
#define QUILLA_META_OFF_SEQ       4U
#define QUILLA_META_OFF_FLAG      8U
#define QUILLA_META_OFF_FLOOR     12U
#define QUILLA_META_OFF_SEAL_HASH 16U
#define QUILLA_META_OFF_SEAL_LEN  48U
#define QUILLA_META_OFF_CRC       52U

/* Field offsets within the manifest block. */
#define QUILLA_MANI_OFF_MAGIC     0U
#define QUILLA_MANI_OFF_VERSION   4U
#define QUILLA_MANI_OFF_IMAGE_LEN 8U
#define QUILLA_MANI_OFF_HASH      12U
#define QUILLA_MANI_OFF_SIG       44U
#define QUILLA_MANI_OFF_CRC       108U

/* ---- cached, authoritative copy of the latest committed record ---------- */

typedef struct {
  uint32_t rec_seq;
  uint32_t flag;                 /* QUILLA_QBOOT_FLAG_* */
  uint32_t floor;                /* anti-rollback floor, 0 = none */
  uint8_t  seal_hash[QUILLA_SECURE_BOOT_HASH_SIZE];
  uint32_t seal_len;
  uint8_t  has_seal;             /* 1 = valid seal present */
  uint32_t mani_version;
  uint32_t mani_image_len;
  uint8_t  mani_hash[QUILLA_SECURE_BOOT_HASH_SIZE];
  uint8_t  mani_sig[QUILLA_SECURE_BOOT_SIG_SIZE];
  uint8_t  has_manifest;         /* 1 = full manifest; 0 = QMF1+ver marker only */
} quilla_meta_state_t;

/* Initialise the store: scan both slots, populate the cached state and pick the
 * active slot. Safe to call once at start-up of either image; the caller must
 * have the system clock running (Flash needs HCLK). */
void quilla_qboot_meta_init(void);

/* Explicit wipe of both slots back to the unprogrammed state (operator-initiated
 * reset, not a runtime write -- the only path that erases the currently-active
 * slot). Used by quilla_qboot_flag_erase(). */
void quilla_qboot_meta_reset(void);

/* Snapshot the cached state. Returns 0 on success (state always valid after
 * quilla_qboot_meta_init). */
int quilla_qboot_meta_get(quilla_meta_state_t *out);

/* Mutators: update one field and append a new record. Each returns 0 on success
 * or <0 on a Flash failure (the caller must treat <0 as a failed commit). */
int quilla_qboot_meta_set_flag(uint32_t flag);
int quilla_qboot_meta_set_floor(uint32_t version);
int quilla_qboot_meta_set_seal(const uint8_t hash[QUILLA_SECURE_BOOT_HASH_SIZE],
                               uint32_t len);
int quilla_qboot_meta_set_manifest(uint32_t version, uint32_t image_len,
                                   const uint8_t hash[QUILLA_SECURE_BOOT_HASH_SIZE],
                                   const uint8_t sig[QUILLA_SECURE_BOOT_SIG_SIZE]);

#endif /* QUILLA_QBOOT_META_H_ */
/** @} */
/** @} */

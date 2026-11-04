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
 * Quilla META store -- see quilla-qboot-meta.h for the layout and the safety
 * argument. This file is hardware independent: all Flash access goes through
 * quilla_qboot_hal_*.
 * ----------------------------------------------------------------------------
 */

#include "quilla-qboot-meta.h"

#include <string.h>

#include "quilla-qboot-secure.h"

/* Aligned staging buffer: doubles as the scanned-record scratch and the
 * composed-record output. Word aligned so the Flash programmer is happy. */
static uint8_t rec_buf[QUILLA_META_RECORD_SIZE] QUILLA_QBOOT_ALIGNED;

/* Cached, authoritative copy of the latest committed record. */
static quilla_meta_state_t meta_cache;
static uint8_t  meta_active;     /* 0 = SLOT_A, 1 = SLOT_B */
static uint32_t meta_head;       /* next record index in the active slot */
static uint8_t  meta_init_done;

static uint32_t
rd32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void
wr32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)(v >> 0);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

/* IEEE 802.3 CRC-32 (polynomial 0xEDB88320, reflected). */
static uint32_t
meta_crc32(const uint8_t *p, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFU;
  uint32_t i;

  for(i = 0U; i < len; i++) {
    uint32_t j;
    crc ^= (uint32_t)p[i];
    for(j = 0U; j < 8U; j++) {
      crc = (crc & 1U) ? ((crc >> 1U) ^ 0xEDB88320U) : (crc >> 1U);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

static uint32_t
meta_slot_base(uint8_t slot)
{
  return (slot == 0U) ? QUILLA_QBOOT_META_BASE
                      : (QUILLA_QBOOT_META_BASE + QUILLA_META_SLOT_SIZE);
}

/* Whether a 256 B region at addr is still fully erased (all 0xFFFFFFFF). */
static uint8_t
meta_region_erased(uint32_t addr, uint32_t len)
{
  uint32_t off;

  for(off = 0U; off < len; off += 4U) {
    uint32_t w;
    quilla_qboot_hal_flash_read(addr + off, (uint8_t *)&w, 4U);
    if(w != 0xFFFFFFFFU) {
      return 0U;
    }
  }
  return 1U;
}

/* Read and validate the meta block of record idx in slot; if valid, extract the
 * manifest block into out and return the record sequence (or 0 if invalid). */
static uint32_t
meta_record_seq(uint8_t slot, uint32_t idx)
{
  uint32_t base = meta_slot_base(slot) + idx * QUILLA_META_RECORD_SIZE;
  uint32_t magic, seq, crc;

  quilla_qboot_hal_flash_read(base, rec_buf, QUILLA_META_RECORD_SIZE);

  magic = rd32(rec_buf + QUILLA_META_OFF_MAGIC);
  if(magic != QUILLA_META_BLK_MAGIC) {
    return 0U;                       /* no record here -> gap */
  }
  crc = meta_crc32(rec_buf + QUILLA_META_OFF_MAGIC, QUILLA_META_OFF_CRC);
  if(crc != rd32(rec_buf + QUILLA_META_OFF_CRC)) {
    return 0U;                       /* torn / corrupted -> gap */
  }
  seq = rd32(rec_buf + QUILLA_META_OFF_SEQ);
  if(seq == 0U) {
    return 0U;                       /* 0 is the invalid-sequence sentinel */
  }
  return seq;
}

/* Extract the cached state from the currently-scanned record (rec_buf already
 * holds it and its meta block is known valid). */
static void
meta_extract_into_cache(uint32_t seq)
{
  uint32_t mani_magic, mani_len;

  meta_cache.rec_seq        = seq;
  meta_cache.flag          = rd32(rec_buf + QUILLA_META_OFF_FLAG);
  meta_cache.floor         = rd32(rec_buf + QUILLA_META_OFF_FLOOR);
  (void)memcpy(meta_cache.seal_hash, rec_buf + QUILLA_META_OFF_SEAL_HASH,
               QUILLA_SECURE_BOOT_HASH_SIZE);
  meta_cache.seal_len      = rd32(rec_buf + QUILLA_META_OFF_SEAL_LEN);
  meta_cache.has_seal      = (meta_cache.seal_len != 0U) ? 1U : 0U;

  mani_magic = rd32(rec_buf + QUILLA_MANI_BLK_OFF + QUILLA_MANI_OFF_MAGIC);
  if(mani_magic == QUILLA_MANI_BLK_MAGIC) {
    /* The manifest block carries its own CRC ("independent logs"): a torn
     * manifest write must not be trusted even though the meta block is valid.
     * On a CRC mismatch we keep the meta fields but drop the manifest. */
    uint32_t mani_crc = meta_crc32(rec_buf + QUILLA_MANI_BLK_OFF,
                                   QUILLA_MANI_OFF_CRC);
    if(mani_crc != rd32(rec_buf + QUILLA_MANI_BLK_OFF +
                        QUILLA_MANI_OFF_CRC)) {
      meta_cache.has_manifest = 0U;
      meta_cache.mani_version = 0U;
      meta_cache.mani_image_len = 0U;
    } else {
      meta_cache.mani_version = rd32(rec_buf + QUILLA_MANI_BLK_OFF +
                                     QUILLA_MANI_OFF_VERSION);
      mani_len = rd32(rec_buf + QUILLA_MANI_BLK_OFF +
                      QUILLA_MANI_OFF_IMAGE_LEN);
      if(mani_len != 0U) {
        meta_cache.has_manifest = 1U;
        meta_cache.mani_image_len = mani_len;
        (void)memcpy(meta_cache.mani_hash,
                     rec_buf + QUILLA_MANI_BLK_OFF + QUILLA_MANI_OFF_HASH,
                     QUILLA_SECURE_BOOT_HASH_SIZE);
        (void)memcpy(meta_cache.mani_sig,
                     rec_buf + QUILLA_MANI_BLK_OFF + QUILLA_MANI_OFF_SIG,
                     QUILLA_SECURE_BOOT_SIG_SIZE);
      } else {
        meta_cache.has_manifest = 0U;
        meta_cache.mani_image_len = 0U;
      }
    }
  } else {
    meta_cache.has_manifest = 0U;
    meta_cache.mani_version = 0U;
    meta_cache.mani_image_len = 0U;
  }
}

/* Scan both slots, find the record with the highest sequence, populate the
 * cache and choose the active slot + write head. */
static void
meta_scan(void)
{
  uint8_t  slot;
  uint32_t best_seq = 0U;
  uint8_t  best_slot = 0U;
  uint32_t best_idx = 0U;
  uint8_t  found = 0U;

  /* Defaults for a fresh / empty store. */
  (void)memset(&meta_cache, 0U, sizeof(meta_cache));
  meta_cache.flag = QUILLA_QBOOT_FLAG_APP1;
  meta_active = 0U;
  meta_head = 0U;

  for(slot = 0U; slot < QUILLA_META_SLOTS; slot++) {
    uint32_t idx;
    uint32_t prev_head = 0U;

    for(idx = 0U; idx < QUILLA_META_RECORDS_PER_SLOT; idx++) {
      uint32_t seq = meta_record_seq(slot, idx);
      if(seq == 0U) {
        break;                       /* first gap: stop scanning this slot */
      }
      prev_head = idx + 1U;
      if((found == 0U) || (seq > best_seq)) {
        best_seq = seq;
        best_slot = slot;
        best_idx = idx;
        found = 1U;
      }
    }
    if(slot == best_slot) {
      meta_head = prev_head;
    }
  }

  if(found != 0U) {
    uint32_t base = meta_slot_base(best_slot) +
                    best_idx * QUILLA_META_RECORD_SIZE;
    quilla_qboot_hal_flash_read(base, rec_buf, QUILLA_META_RECORD_SIZE);
    meta_extract_into_cache(best_seq);
    meta_active = best_slot;
  }
}

static void
meta_ensure_init(void)
{
  if(meta_init_done == 0U) {
    meta_scan();
    meta_init_done = 1U;
  }
}

void
quilla_qboot_meta_init(void)
{
  meta_scan();
  meta_init_done = 1U;
}

int
quilla_qboot_meta_get(quilla_meta_state_t *out)
{
  meta_ensure_init();
  if(out != NULL) {
    (void)memcpy(out, &meta_cache, sizeof(*out));
  }
  return 0;
}

/* Compose the 256 B record from the current cache (rec_buf) and append it to
 * the active slot, switching slots (and pre-erasing the now-standby) when the
 * active slot is full. Returns 0 on success, <0 on Flash failure. */
static int
meta_append(void)
{
  uint8_t  other;
  uint32_t base;
  uint32_t off;
  uint32_t new_seq;
  int      rc;

  /* Sequence is strictly increasing and never 0 (0 is the "gap" sentinel used
   * by meta_record_seq). On a fresh store rec_seq == 0, so the first record is
   * emitted as sequence 1. */
  new_seq = meta_cache.rec_seq + 1U;

  /* Build the record. */
  (void)memset(rec_buf, 0xFFU, QUILLA_META_RECORD_SIZE);
  wr32(rec_buf + QUILLA_META_OFF_MAGIC, QUILLA_META_BLK_MAGIC);
  wr32(rec_buf + QUILLA_META_OFF_SEQ,   new_seq);
  wr32(rec_buf + QUILLA_META_OFF_FLAG,  meta_cache.flag);
  wr32(rec_buf + QUILLA_META_OFF_FLOOR, meta_cache.floor);
  (void)memcpy(rec_buf + QUILLA_META_OFF_SEAL_HASH, meta_cache.seal_hash,
               QUILLA_SECURE_BOOT_HASH_SIZE);
  wr32(rec_buf + QUILLA_META_OFF_SEAL_LEN, meta_cache.seal_len);
  wr32(rec_buf + QUILLA_META_OFF_CRC,
       meta_crc32(rec_buf + QUILLA_META_OFF_MAGIC, QUILLA_META_OFF_CRC));

  wr32(rec_buf + QUILLA_MANI_BLK_OFF + QUILLA_MANI_OFF_MAGIC,
       QUILLA_MANI_BLK_MAGIC);
  wr32(rec_buf + QUILLA_MANI_BLK_OFF + QUILLA_MANI_OFF_VERSION,
       meta_cache.mani_version);
  if(meta_cache.has_manifest != 0U) {
    wr32(rec_buf + QUILLA_MANI_BLK_OFF + QUILLA_MANI_OFF_IMAGE_LEN,
         meta_cache.mani_image_len);
    (void)memcpy(rec_buf + QUILLA_MANI_BLK_OFF + QUILLA_MANI_OFF_HASH,
                 meta_cache.mani_hash, QUILLA_SECURE_BOOT_HASH_SIZE);
    (void)memcpy(rec_buf + QUILLA_MANI_BLK_OFF + QUILLA_MANI_OFF_SIG,
                 meta_cache.mani_sig, QUILLA_SECURE_BOOT_SIG_SIZE);
  } else {
    wr32(rec_buf + QUILLA_MANI_BLK_OFF + QUILLA_MANI_OFF_IMAGE_LEN, 0U);
  }
  wr32(rec_buf + QUILLA_MANI_BLK_OFF + QUILLA_MANI_OFF_CRC,
       meta_crc32(rec_buf + QUILLA_MANI_BLK_OFF, QUILLA_MANI_OFF_CRC));

  base = meta_slot_base(meta_active);
  off = meta_head * QUILLA_META_RECORD_SIZE;

  if((off + QUILLA_META_RECORD_SIZE) > QUILLA_META_SLOT_SIZE) {
    /* Active slot full: the other slot is the standby and is already clean
     * (pre-erased once the active slot grew past one record), so switch to it. */
    other = (uint8_t)(1U - (uint32_t)meta_active);
    rc = quilla_qboot_hal_flash_erase_range(meta_slot_base(other),
                                            QUILLA_META_SLOT_SIZE);
    if(rc != 0) {
      return rc;
    }
    meta_active = other;
    meta_head = 0U;
    base = meta_slot_base(meta_active);
    off = 0U;
  }

  /* The append-only design only works when the target page is erased: Flash can
   * only clear bits, never set them, so programming onto a page that still
   * carries data silently corrupts the record while the program call still
   * reports success. That is exactly the "SIGN ACKs but the next boot sees no
   * manifest" failure -- the meta block (clean bits) survives but the manifest
   * block (dirty bits) fails its CRC.
   *
   * A board that reaches the upgrade path without a chip-erase (or that carries
   * stale data from an earlier firmware) can have arbitrary bits set across the
   * whole META region. When this slot still holds NO valid record (meta_head ==
   * 0 -- the scan found nothing authoritative here) and is not already erased,
   * erase it first. meta_head == 0 guarantees we destroy nothing live; every
   * valid record lives at an index below the write head. */
  if(meta_head == 0U &&
     meta_region_erased(base, QUILLA_META_SLOT_SIZE) == 0U) {
    rc = quilla_qboot_hal_flash_erase_range(base, QUILLA_META_SLOT_SIZE);
    if(rc != 0) {
      return rc;
    }
  }

  rc = quilla_qboot_hal_flash_program(base + off, rec_buf,
                                     QUILLA_META_RECORD_SIZE);
  if(rc != 0) {
    return rc;
  }
  meta_head++;
  meta_cache.rec_seq = new_seq;

  /* Pre-erase the standby once the active slot holds more than one record, so it
   * is always clean before the active slot fills. Only erases when the standby
   * still carries stale data (the slot that does NOT hold the latest record). */
  if((meta_head * QUILLA_META_RECORD_SIZE) > QUILLA_META_RECORD_SIZE) {
    other = (uint8_t)(1U - (uint32_t)meta_active);
    if(meta_region_erased(meta_slot_base(other), QUILLA_META_RECORD_SIZE) == 0U) {
      rc = quilla_qboot_hal_flash_erase_range(meta_slot_base(other),
                                              QUILLA_META_SLOT_SIZE);
      if(rc != 0) {
        return rc;
      }
    }
  }
  return 0;
}

int
quilla_qboot_meta_set_flag(uint32_t flag)
{
  meta_ensure_init();
  meta_cache.flag = flag;
  return meta_append();
}

int
quilla_qboot_meta_set_floor(uint32_t version)
{
  meta_ensure_init();
  if(version <= meta_cache.floor) {
    return 0;                         /* already at or above; no commit */
  }
  meta_cache.floor = version;
  return meta_append();
}

int
quilla_qboot_meta_set_seal(const uint8_t hash[QUILLA_SECURE_BOOT_HASH_SIZE],
                           uint32_t len)
{
  meta_ensure_init();
  (void)memcpy(meta_cache.seal_hash, hash, QUILLA_SECURE_BOOT_HASH_SIZE);
  meta_cache.seal_len = len;
  meta_cache.has_seal = 1U;
  return meta_append();
}

int
quilla_qboot_meta_set_manifest(uint32_t version, uint32_t image_len,
                               const uint8_t hash[QUILLA_SECURE_BOOT_HASH_SIZE],
                               const uint8_t sig[QUILLA_SECURE_BOOT_SIG_SIZE])
{
  meta_ensure_init();
  meta_cache.mani_version = version;
  meta_cache.mani_image_len = image_len;
  (void)memcpy(meta_cache.mani_hash, hash, QUILLA_SECURE_BOOT_HASH_SIZE);
  (void)memcpy(meta_cache.mani_sig, sig, QUILLA_SECURE_BOOT_SIG_SIZE);
  meta_cache.has_manifest = 1U;
  return meta_append();
}

/* Explicit reset: erase both slots and return to the default state. Used by
 * quilla_qboot_flag_erase(). This is the one place that intentionally erases the
 * currently-active slot; it is an operator-initiated wipe, not a runtime write. */
void
quilla_qboot_meta_reset(void)
{
  uint8_t slot;
  for(slot = 0U; slot < QUILLA_META_SLOTS; slot++) {
    (void)quilla_qboot_hal_flash_erase_range(meta_slot_base(slot),
                                             QUILLA_META_SLOT_SIZE);
  }
  (void)memset(&meta_cache, 0U, sizeof(meta_cache));
  meta_cache.flag = QUILLA_QBOOT_FLAG_APP1;
  meta_active = 0U;
  meta_head = 0U;
  meta_init_done = 1U;
}

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
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
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
 * \addtogroup quilla-qiap
 * @{
 */

#include "quilla-qiap.h"

#include "quilla-qboot-flag.h"
#include "quilla-qboot-secure.h"
#include "quilla-qboot-meta.h"
#include "crc16.h"

/* ---- transport-agnostic state ------------------------------------------- */
typedef enum {
  S_SOF = 0, S_CMD, S_LEN1, S_LEN2, S_PAYLOAD, S_CRC1, S_CRC2
} iap_state_t;

static iap_state_t state = S_SOF;
static uint8_t  cmd;
static uint16_t len;
static uint16_t got;
static uint8_t  payload[QUILLA_QIAP_MAX_PAYLOAD];
static uint16_t calc_crc;
static uint8_t  crc_lo, crc_hi;

static uint32_t rx_total_len;
static uint16_t rx_total_crc;
static int      session_active;
static int      end_seen;      /* END accepted; waiting for SIGN to commit   */
static int      done;

/* fp_staging removed: the manifest is now written through the wear-levelled
 * META store (quilla_qboot_meta_set_manifest), not a dedicated staging page. */

/* ---- APP page-write coalescing -------------------------------------------
 * DATA frames carry 48-byte chunks (the host chunk is bounded by the RX DMA
 * design) but a Flash page is 256 bytes, so the naive read-modify-erase-write
 * per chunk costs ~5.3 erase/program cycles for every page -- an erase is what
 * actually costs tens of milliseconds, and that is where the ~20 s upgrade time
 * went. Instead the incoming bytes are merged into one staged page and the page
 * is committed exactly once, when the stream moves to another page or the
 * session is finalised. Same read-back verification, ~5x fewer erase cycles.
 *
 * No dynamic allocation -- static, sized to exactly one page. */
static uint8_t app_page[QUILLA_QBOOT_PAGE_SIZE] QUILLA_QBOOT_ALIGNED;
static uint8_t fp_verify[QUILLA_QBOOT_PAGE_SIZE] QUILLA_QBOOT_ALIGNED;
static uint32_t app_page_base = 0U;    /* page currently staged            */
static uint8_t  app_page_loaded = 0U;  /* 1 = app_page holds a real page   */
static uint8_t  app_page_dirty = 0U;   /* 1 = staged page needs committing */

/* Commit the staged page: one erase + one program + one full-page verify.
 * Returns 0 on success, the HAL code on erase/program failure, -4 on a
 * read-back mismatch. A clean (not dirty) stage is a no-op. */
static int
app_page_commit(void)
{
  uint32_t i;
  int rc;

  if(app_page_dirty == 0U) {
    return 0;
  }
  rc = quilla_qboot_hal_flash_write_page(app_page_base, app_page);
  if(rc != 0) {
    return rc;
  }
  quilla_qboot_hal_flash_read(app_page_base, fp_verify, QUILLA_QBOOT_PAGE_SIZE);
  for(i = 0U; i < QUILLA_QBOOT_PAGE_SIZE; i++) {
    if(fp_verify[i] != app_page[i]) {
      return -4;   /* read-back mismatch */
    }
  }
  app_page_dirty = 0U;
  return 0;
}

static const uint8_t unlock_key[QUILLA_QIAP_UNLOCK_LEN] = QUILLA_QBOOT_UNLOCK_KEY;

static uint32_t
rd32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Compare two byte strings without pulling in libc (no memcmp in the
 * bootloader). Returns non-zero when equal. */
static int
key_match(const uint8_t *a, const uint8_t *b, uint16_t len)
{
  uint16_t i;
  uint8_t diff = 0U;

  for(i = 0U; i < len; i++) {
    diff = (uint8_t)(diff | (uint8_t)(a[i] ^ b[i]));
  }
  return diff == 0U;
}

static void
send_frame(uint8_t fcmd, const uint8_t *pl, uint16_t plen)
{
  uint8_t buf[10];
  uint16_t crc;
  uint8_t i;

  buf[0] = QUILLA_QIAP_SOF;
  buf[1] = fcmd;
  buf[2] = (uint8_t)(plen & 0xFFU);
  buf[3] = (uint8_t)((plen >> 8) & 0xFFU);
  for(i = 0; i < plen; i++) {
    buf[4 + i] = pl[i];
  }
  crc = crc16_data(&buf[1], (int)(1 + 2 + plen), 0);
  buf[4 + plen] = (uint8_t)(crc & 0xFFU);
  buf[5 + plen] = (uint8_t)((crc >> 8) & 0xFFU);
  quilla_qboot_hal_transmit(buf, (uint16_t)(6 + plen));
}

static void
send_ack(uint8_t fcmd, uint8_t status)
{
  send_frame(fcmd, &status, 1U);
}

static void
send_nak(uint8_t phase, uint8_t reason)
{
  uint8_t buf[1];
  buf[0] = reason;
  send_frame((uint8_t)(0xC0U | phase), buf, 1U);
}

/* Merge [addr, addr+datalen) into the APP region.
 *
 * Bytes are merged into the single staged page (app_page) and the page is
 * committed once -- on the move to a different page, or by the explicit flush
 * at session finalisation. Partial-page frames stay safe because the page is
 * always read in before the first merge. Returns 0 on success, negative on
 * failure (the commit, including its read-back verify, is where a real Flash
 * error surfaces). */
static int
flash_write_range(uint32_t addr, const uint8_t *data, uint32_t datalen)
{
  uint32_t off = 0U;

  while(off < datalen) {
    uint32_t cur = addr + off;
    uint32_t page_base = cur & ~(QUILLA_QBOOT_PAGE_SIZE - 1U);
    uint32_t page_off = cur - page_base;
    uint32_t room = QUILLA_QBOOT_PAGE_SIZE - page_off;
    uint32_t n = (room < (datalen - off)) ? room : (datalen - off);
    uint32_t i;
    int rc;

    if((app_page_loaded == 0U) || (app_page_base != page_base)) {
      /* Switching pages: commit the outgoing one so no merged byte is lost,
       * then read the new page in exactly once. */
      rc = app_page_commit();
      if(rc != 0) {
        return rc;
      }
      quilla_qboot_hal_flash_read(page_base, app_page, QUILLA_QBOOT_PAGE_SIZE);
      app_page_base = page_base;
      app_page_loaded = 1U;
    }
    for(i = 0U; i < n; i++) {
      app_page[page_off + i] = data[off + i];
    }
    app_page_dirty = 1U;
    off += n;
  }
  return 0;
}

/* Commit the session: mark the APP runnable and tell the caller to reset. */
static int
commit_app(void)
{
  int rc;
  /* Safety net. The END handler already flushed, but a commit must never mark
   * an image runnable while part of it is still only staged in RAM. */
  rc = app_page_commit();
  if(rc != 0) {
    return rc;
  }
  if(quilla_qboot_flag_set(QUILLA_QBOOT_FLAG_APP1) != 0) {
    return -1;
  }
  done = 1;
  return 0;
}

/* The secure-boot manifest is no longer written to a dedicated page. The QIAP
 * SIGN / integrity-only END paths call quilla_qboot_meta_set_manifest()
 * (quilla-qboot-meta.c), which appends a record to the wear-levelled META log
 * without erasing the live slot. The boot gate reads it back via
 * quilla_qboot_meta_get(). */

static void
dispatch(void)
{
  if(cmd == QUILLA_QIAP_CMD_ENTER) {
    uint32_t app_start;
    if(len < (4U + QUILLA_QIAP_UNLOCK_LEN)) {
      send_nak(1U, QUILLA_QIAP_RC_SZ);
      return;
    }
    app_start = rd32(&payload[0]);
    if(app_start != QUILLA_QBOOT_APP_START) {
      send_nak(1U, QUILLA_QIAP_RC_ADDR);
      return;
    }
    if(!key_match(&payload[4], unlock_key, QUILLA_QIAP_UNLOCK_LEN)) {
      send_nak(1U, QUILLA_QIAP_RC_KEY);
      return;
    }
    rx_total_len = 0U;
    rx_total_crc = 0U;
    end_seen = 0;
    session_active = 1;
    /* Lock in the upgrade so a reset / power-loss mid-flash re-enters the serial
     * upgrade on the next boot instead of jumping a half-written APP. */
    if(quilla_qboot_flag_set(QUILLA_QBOOT_FLAG_QIAP) != 0) {
      send_nak(1U, QUILLA_QIAP_RC_PROG);
      return;
    }
    send_ack(QUILLA_QIAP_ACK_ENTER, 0U);
    return;
  }

  if(cmd == QUILLA_QIAP_CMD_DATA) {
    uint32_t start, end, datalen;
    const uint8_t *data;
    int rc;
    if(!session_active) {
      send_nak(2U, QUILLA_QIAP_RC_SZ);
      return;
    }
    if(len < 8U) {
      send_nak(2U, QUILLA_QIAP_RC_SZ);
      return;
    }
    start = rd32(&payload[0]);
    end = rd32(&payload[4]);
    data = &payload[8];
    datalen = (uint32_t)len - 8U;
    if(start < QUILLA_QBOOT_APP_START || end > QUILLA_QBOOT_APP_END ||
       end < start || (end - start + 1U) != datalen) {
      send_nak(2U, QUILLA_QIAP_RC_ADDR);
      return;
    }
    rc = flash_write_range(start, data, datalen);
    if(rc == -4) {
      send_nak(2U, QUILLA_QIAP_RC_PROG);
      return;
    } else if(rc != 0) {
      send_nak(2U, QUILLA_QIAP_RC_PROG);
      return;
    }
    rx_total_len += datalen;
    rx_total_crc = crc16_data(data, (int)datalen, rx_total_crc);
    {
      uint8_t rsp[4];
      rsp[0] = (uint8_t)(start & 0xFFU);
      rsp[1] = (uint8_t)((start >> 8) & 0xFFU);
      rsp[2] = (uint8_t)((start >> 16) & 0xFFU);
      rsp[3] = (uint8_t)((start >> 24) & 0xFFU);
      send_frame(QUILLA_QIAP_ACK_DATA, rsp, 4U);
    }
    return;
  }

  if(cmd == QUILLA_QIAP_CMD_END) {
    uint32_t total_len;
    uint16_t total_crc;
    if(!session_active) {
      send_nak(3U, QUILLA_QIAP_RC_SZ);
      return;
    }
    if(len < 6U) {
      send_nak(3U, QUILLA_QIAP_RC_SZ);
      return;
    }
    total_len = rd32(&payload[0]);
    total_crc = (uint16_t)payload[4] | ((uint16_t)payload[5] << 8);
    if(rx_total_len != total_len || rx_total_crc != total_crc) {
      send_nak(3U, QUILLA_QIAP_RC_LEN);
      return;
    }
    end_seen = 1;

    /* The stream is complete and validated: commit the last (partial) staged
     * page NOW. This must happen before anything looks at the APP region as a
     * whole -- the SIGN path hashes [APP_START, +rx_total_len) and would
     * otherwise digest a page that is still only in RAM. */
    if(app_page_commit() != 0) {
      send_nak(3U, QUILLA_QIAP_RC_PROG);
      return;
    }

    /* With a verification key provisioned, END only acknowledges: the APP is
     * committed by the SIGN frame once the signature checks out, so an
     * unsigned or mis-signed image is never marked runnable.
     *
     * With no key (secure boot inactive) there is nothing to verify, so END
     * commits directly and the pre-secure-boot host flow keeps working. */
    if(!quilla_qboot_secure_pubkey_active()) {
      /* Integrity-only provisioning: record the image digest so CHECK boot
       * mode (and the legacy inactive mode) can verify transport integrity
       * on every boot. No trusted signature is stored -- this does NOT
       * authenticate the image's origin. */
      uint8_t hash[QUILLA_SECURE_BOOT_HASH_SIZE];
      uint8_t zero_sig[QUILLA_SECURE_BOOT_SIG_SIZE] = { 0U };
      if(rx_total_len != 0U &&
         rx_total_len <= QUILLA_SECURE_BOOT_MAX_IMAGE_LEN) {
        quilla_qboot_secure_hash_range(QUILLA_QBOOT_APP_START, rx_total_len, hash);
        (void)quilla_qboot_meta_set_manifest(
                  quilla_qboot_secure_min_version_get(), rx_total_len, hash, zero_sig);
      }
      if(commit_app() != 0) {
        send_nak(3U, QUILLA_QIAP_RC_PROG);
        return;
      }
    }
    send_ack(QUILLA_QIAP_ACK_END, 0U);
    return;
  }

  if(cmd == QUILLA_QIAP_CMD_SIGN) {
    uint32_t nv, fw_version;
    uint8_t hash[QUILLA_SECURE_BOOT_HASH_SIZE];

    if(!session_active || !end_seen) {
      send_nak(4U, QUILLA_QIAP_RC_SZ);
      return;
    }
    if(len != QUILLA_QIAP_SIGN_LEN) {
      send_nak(4U, QUILLA_QIAP_RC_SZ);
      return;
    }

    /* The SIGN frame carries only the signature; the version is taken from the
     * firmware image (QUILLA_APP_SECURITY_VERSION), not supplied by the host. */

    /* Integrity-only provisioning (no key flashed, anti-rollback NOT enforced):
     * commit the image as-is so an unprovisioned device is not bricked by a
     * SIGN frame. The recorded version is the current floor (placeholder); the
     * firmware's own version is authoritative only under a keyed install. */
    if(!quilla_qboot_secure_pubkey_active()) {
      if(rx_total_len != 0U &&
         rx_total_len <= QUILLA_SECURE_BOOT_MAX_IMAGE_LEN) {
        quilla_qboot_secure_hash_range(QUILLA_QBOOT_APP_START, rx_total_len, hash);
        (void)quilla_qboot_meta_set_manifest(
                  quilla_qboot_secure_min_version_get(), rx_total_len, hash, payload);
      }
      if(commit_app() != 0) {
        send_nak(4U, QUILLA_QIAP_RC_PROG);
        return;
      }
      send_ack(QUILLA_QIAP_ACK_SIGN, 0U);
      return;
    }

    if(rx_total_len == 0U ||
       rx_total_len > QUILLA_SECURE_BOOT_MAX_IMAGE_LEN) {
      send_nak(4U, QUILLA_QIAP_RC_SZ);
      return;
    }

    /* Bind the signature to what is actually in Flash right now. */
    quilla_qboot_secure_hash_range(QUILLA_QBOOT_APP_START, rx_total_len, hash);

    if(!quilla_qboot_secure_verify_descriptor(rx_total_len, hash, payload)) {
      send_nak(4U, QUILLA_QIAP_RC_SIGN);
      return;
    }

    /* The firmware version is taken from the signed image (not supplied by the
     * host, not invented by QBOOT). QBOOT only ratchets its floor up to it.
     * See doc/secure-boot.md ("Dual anti-rollback mechanism"). */
    fw_version = quilla_qboot_secure_fw_version();
    if(fw_version == 0U) {
      /* Image carries no usable security-version region: refuse rather than
       * record a meaningless version. Reported distinctly from a signature
       * failure so the host can say WHY (missing .quilla_secver / magic). */
      send_nak(4U, QUILLA_QIAP_RC_VER);
      return;
    }
    if(fw_version < quilla_qboot_secure_min_version_get()) {
      /* Downgrade: the firmware version is below the persisted floor, so a
       * signed-but-older image cannot be installed over a newer one. Reported
       * distinctly (ROLLBACK) so the host does not mislabel it as a bad
       * signature. */
      send_nak(4U, QUILLA_QIAP_RC_ROLLBACK);
      return;
    }
    nv = fw_version;

    if(quilla_qboot_meta_set_manifest(nv, rx_total_len, hash, payload) != 0) {
      send_nak(4U, QUILLA_QIAP_RC_PROG);
      return;
    }

    (void)quilla_qboot_secure_min_version_set(nv);

    /* Persist the seal so the next boot skips the ~20 s Ed25519 verify. The
     * seal is authoritative only because the META record is written solely by
     * the authenticated SIGN path and the first verified boot of a new image. */
    if(quilla_qboot_secure_seal_set(hash, rx_total_len) != 0) {
      send_nak(4U, QUILLA_QIAP_RC_PROG);
      return;
    }

    if(commit_app() != 0) {
      send_nak(4U, QUILLA_QIAP_RC_PROG);
      return;
    }
    send_ack(QUILLA_QIAP_ACK_SIGN, 0U);
    return;
  }

  /* Unknown command: NAK as a generic protocol error. */
  send_nak(3U, QUILLA_QIAP_RC_CRC);
}

void
quilla_qiap_init(void)
{
  state = S_SOF;
  cmd = 0U;
  len = 0U;
  got = 0U;
  rx_total_len = 0U;
  rx_total_crc = 0U;
  session_active = 0;
  end_seen = 0;
  done = 0;
  /* Drop any staged page from an earlier (aborted) session instead of
   * committing it: the host retransmits the whole image, so a stale partial
   * page in RAM must never reach Flash. */
  app_page_loaded = 0U;
  app_page_dirty = 0U;
}

void
quilla_qiap_feed(uint8_t b)
{
  switch(state) {
    case S_SOF:
      if(b == QUILLA_QIAP_SOF) {
        state = S_CMD;
        calc_crc = 0U;
      }
      break;

    case S_CMD:
      cmd = b;
      calc_crc = crc16_data(&b, 1, calc_crc);
      state = S_LEN1;
      break;

    case S_LEN1:
      len = b;
      calc_crc = crc16_data(&b, 1, calc_crc);
      state = S_LEN2;
      break;

    case S_LEN2:
      len |= (uint16_t)((uint16_t)b << 8);
      calc_crc = crc16_data(&b, 1, calc_crc);
      got = 0U;
      if(len == 0U) {
        state = S_CRC1;
      } else if(len > QUILLA_QIAP_MAX_PAYLOAD) {
        state = S_SOF;   /* oversized -> discard, wait for next SOF */
      } else {
        state = S_PAYLOAD;
      }
      break;

    case S_PAYLOAD:
      payload[got++] = b;
      calc_crc = crc16_data(&b, 1, calc_crc);
      if(got >= len) {
        state = S_CRC1;
      }
      break;

    case S_CRC1:
      crc_lo = b;
      state = S_CRC2;
      break;

    case S_CRC2:
      crc_hi = b;
      if((uint16_t)(((uint16_t)crc_hi << 8) | crc_lo) == calc_crc) {
        dispatch();
      } else {
        /* Frame CRC error: report phase-agnostically and resync. */
        send_nak(3U, QUILLA_QIAP_RC_CRC);
      }
      state = S_SOF;
      break;

    default:
      state = S_SOF;
      break;
  }
}

int
quilla_qiap_is_done(void)
{
  return done;
}

/** @} */

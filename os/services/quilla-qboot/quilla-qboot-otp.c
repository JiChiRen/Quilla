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
 * \addtogroup quilla-qboot-otp
 * @{
 *
 * Counter layout in the reserved OTP region [BASE, BASE + SIZE):
 *
 *     [0, COUNTER_BYTES)  counter cells: 0xFF = unused, any other value =
 *                         burned. Cells are burned strictly in increasing
 *                         order, so the counter value is the index of the
 *                         first still-0xFF cell.
 *     [COUNTER_BYTES]     exhaustion marker: 0xFF = OTP still has capacity,
 *                         any other value = burned, "META flash floor is the
 *                         sole carrier of future anti-rollback state".
 *
 * COUNTER_BYTES defaults to OTP size - 1 (the marker occupies the last cell).
 * The byte-per-increment scheme matches the one-time-program-per-cell nature
 * of GD32 OTP; the comparison (4-byte image TLV, 1 fuse bit per
 * increment on eFuse parts) is recorded in doc/anti-rollback-counter.md.
 *
 * The scan treats a failed/interrupted program like a burn: any non-0xFF cell
 * within the prefix counts as consumed, which keeps the counter monotonic
 * under power loss (one cell wasted at worst).
 * @{
 */

#include "quilla-qboot-otp.h"

/* ---- build-time warning policy -------------------------------------------
 *
 * Driven by QUILLA_QBOOT_HW_COUNTER_LEVEL from quilla-qboot-secure.h;
 * carrier availability is resolved in quilla-qboot-hal.h:
 *
 *   - NONE           : warn that the hardware counter is not enabled, and
 *                      report the highest level this MCU CAN configure
 *                      (OTP / WPP / neither).
 *   - OTP requested but SOC_OTP_AVAILABLE=0 : warn that the chip has no OTP;
 *                      if the chip also has a WRP sector, tell the integrator
 *                      to enable the WPP partition for counter storage.
 *   - WPP requested but SOC_WPP_AVAILABLE=0 : symmetric warning.
 *   - level honoured (requested carrier present) : silent (enabled).
 * ------------------------------------------------------------------------- */
#if QUILLA_QBOOT_HW_COUNTER_LEVEL == QUILLA_QBOOT_HW_COUNTER_NONE
#warning "QBOOT: HW counter disabled (LEVEL=NONE)"
#if QUILLA_QBOOT_OTP_AVAIL
#warning "QBOOT: LEVEL=OTP available on this MCU"
#elif QUILLA_QBOOT_WPP_AVAIL
#warning "QBOOT: LEVEL=WPP available on this MCU"
#else
#warning "QBOOT: no counter carrier on this MCU"
#endif
#elif QUILLA_QBOOT_HW_COUNTER_LEVEL == QUILLA_QBOOT_HW_COUNTER_OTP
#if !QUILLA_QBOOT_OTP_AVAIL
#warning "QBOOT: LEVEL=OTP selected but this MCU has no OTP"
#if QUILLA_QBOOT_WPP_AVAIL
#warning "QBOOT: use LEVEL=WPP instead"
#endif
#endif
#elif QUILLA_QBOOT_HW_COUNTER_LEVEL == QUILLA_QBOOT_HW_COUNTER_WPP
#if !QUILLA_QBOOT_WPP_AVAIL
#warning "QBOOT: LEVEL=WPP selected but this MCU has no write-protect sector"
#if QUILLA_QBOOT_OTP_AVAIL
#warning "QBOOT: use LEVEL=OTP instead"
#endif
#endif
#endif

/* Write-protected NV counter reserves a whole WRP sector inside main flash: the default APP
 * partition (APP_END = SOC_FLASH_END) usually reaches into it. The hardware
 * protection itself makes any QIAP erase/write there fail loudly (WRPERR /
 * WRPRTERR), but the integrator must know the top sector is off-limits to
 * images while the counter is enabled. */
#if QUILLA_QBOOT_OTP_COUNTER && QUILLA_QBOOT_WPP
#if QUILLA_QBOOT_APP_END >= QUILLA_QBOOT_OTP_BASE
#warning "QBOOT: APP_END overlaps WPP counter sector; shrink APP_END"
#endif
#endif

#if QUILLA_QBOOT_OTP_COUNTER

/* Counter capacity in cells; the last cell is the exhaustion marker. */
#ifndef QUILLA_QBOOT_OTP_COUNTER_CELLS
#define QUILLA_QBOOT_OTP_COUNTER_CELLS \
  ((QUILLA_QBOOT_OTP_SIZE / QUILLA_QBOOT_OTP_CELL_SIZE) - 1U)
#endif

/* Marker cell: the last cell of the reserved counter region. */
#define QUILLA_QBOOT_OTP_MARKER_ADDR \
  (QUILLA_QBOOT_OTP_BASE + \
   ((uint32_t)QUILLA_QBOOT_OTP_COUNTER_CELLS * \
    (uint32_t)QUILLA_QBOOT_OTP_CELL_SIZE))

/* Is one whole cell still erased (all bytes 0xFF)? */
static int
otp_cell_virgin(uint32_t addr)
{
  uint32_t i;

  for(i = 0U; i < (uint32_t)QUILLA_QBOOT_OTP_CELL_SIZE; i++) {
    uint8_t b;

    quilla_qboot_hal_flash_read(addr + i, &b, 1U);
    if(b != 0xFFU) {
      return 0;
    }
  }
  return 1;
}

/* Count the burned (non-0xFF) leading cells; also report the marker state. */
static void
otp_scan(uint32_t *burned, int *exhausted)
{
  uint32_t i;

  *burned = 0U;
  *exhausted = 0;

  for(i = 0U; i < QUILLA_QBOOT_OTP_COUNTER_CELLS; i++) {
    if(otp_cell_virgin(QUILLA_QBOOT_OTP_BASE +
                       (i * (uint32_t)QUILLA_QBOOT_OTP_CELL_SIZE)) != 0) {
      break;                    /* first free cell: everything after is free */
    }
    *burned = i + 1U;           /* burned or partially programmed: consumed */
  }

  *exhausted = otp_cell_virgin(QUILLA_QBOOT_OTP_MARKER_ADDR) ? 0 : 1;
}

uint32_t
quilla_qboot_otp_counter_get(void)
{
  uint32_t burned;
  int exhausted;

  otp_scan(&burned, &exhausted);
  return burned;
}

int
quilla_qboot_otp_exhausted(void)
{
  uint32_t burned;
  int exhausted;

  otp_scan(&burned, &exhausted);
  return exhausted;
}

int
quilla_qboot_otp_floor_ratchet(uint32_t target)
{
#if QUILLA_QBOOT_WPP
  /* write-protected NV counter parts cannot burn synchronously: opening the write-
   * protection window resets the chip. The META floor (already raised by the
   * caller) is the pending target; quilla_qboot_otp_sync() burns the cells
   * during the next boot. Cost: two extra resets per upgrade, and only when
   * QUILLA_QBOOT_OTP_COUNTER is enabled. */
  (void)target;
  return 0;
#else
  uint32_t burned;
  int exhausted;
  uint32_t i;
  uint32_t room;
  uint32_t want;
  uint32_t burn_now;

  otp_scan(&burned, &exhausted);

  if(exhausted != 0) {
    /* No OTP capacity left: the META flash floor is the sole carrier. The
     * caller keeps ratcheting it independently (quilla-qboot-secure.c). */
    return 1;
  }
  if(burned >= target) {
    return 0;
  }

  /* Burn as far as capacity allows, strictly in order: the strongest OTP
   * floor is kept even when the target does not fully fit. */
  room = (uint32_t)QUILLA_QBOOT_OTP_COUNTER_CELLS - burned;
  want = target - burned;
  burn_now = (want < room) ? want : room;

  for(i = burned; i < (burned + burn_now); i++) {
    if(quilla_qboot_hal_otp_program_cell(
         QUILLA_QBOOT_OTP_BASE +
         (i * (uint32_t)QUILLA_QBOOT_OTP_CELL_SIZE)) != 0) {
      return -1;
    }
  }

  /* Synchronous confirmation: re-scan and require the new value to hold. */
  otp_scan(&burned, &exhausted);
  if(burned < target) {
    if(exhausted == 0) {
      /* Capacity exhausted before reaching the target: burn the marker
       * (read-back confirmed inside the HAL) and hand anti-rollback state
       * over to the META flash floor. The burned cells remain a valid floor
       * forever. */
      if(quilla_qboot_hal_otp_program_cell(QUILLA_QBOOT_OTP_MARKER_ADDR) != 0) {
        return -1;
      }
    }
    return 1;
  }
  return 0;
#endif /* QUILLA_QBOOT_WPP */
}

#else /* !QUILLA_QBOOT_OTP_COUNTER */

/* Inactive build: the gate simply never sees an OTP contribution. The stubs
 * keep the secure-boot code path identical across configurations. */

uint32_t
quilla_qboot_otp_counter_get(void)
{
  return 0U;
}

int
quilla_qboot_otp_exhausted(void)
{
  return 0;
}

int
quilla_qboot_otp_floor_ratchet(uint32_t target)
{
  (void)target;
  return 0;
}

#endif /* QUILLA_QBOOT_OTP_COUNTER */

void
quilla_qboot_otp_sync(uint32_t floor)
{
#if QUILLA_QBOOT_OTP_COUNTER && QUILLA_QBOOT_WPP
  uint32_t burned;
  int exhausted;
  uint32_t i;
  uint32_t limit;

  otp_scan(&burned, &exhausted);
  if((exhausted != 0) || (burned >= floor)) {
    /* Settled: the counter already covers the floor (or is exhausted, in
     * which case the META flash floor carries future state). This is the
     * steady state and the ONLY exit that does not reset the chip. */
    return;
  }

  /* Pending burn. Open the write-protection window first if the region is
   * still protected: option-byte change + reload, which resets the chip and
   * never returns on parts like PY32. Parts whose option bytes reload only
   * on power-on reset (CH32, unconfirmed) leave the protection latched until
   * the next power cycle; the burn then fails harmlessly and is retried on
   * every boot, with the device still able to run the (signed) new image. */
  if(quilla_qboot_hal_otp_wrprot_status() != 0) {
    quilla_qboot_hal_otp_wrprot_set(0);   /* resets; never returns */
  }

  /* Window open: burn the pending cells strictly in order. A failed or
   * interrupted burn is idempotent -- the next boot resumes from the scan. */
  limit = (floor < (uint32_t)QUILLA_QBOOT_OTP_COUNTER_CELLS)
          ? floor : (uint32_t)QUILLA_QBOOT_OTP_COUNTER_CELLS;
  for(i = burned; i < limit; i++) {
    if(quilla_qboot_hal_otp_program_cell(
         QUILLA_QBOOT_OTP_BASE +
         (i * (uint32_t)QUILLA_QBOOT_OTP_CELL_SIZE)) != 0) {
      return;                 /* leave the window open; retry next boot */
    }
  }

  /* The floor may exceed the capacity: burn the exhaustion marker so the
   * next boot takes the settled path instead of repeating this dance. */
  otp_scan(&burned, &exhausted);
  if((burned < floor) && (exhausted == 0)) {
    (void)quilla_qboot_hal_otp_program_cell(QUILLA_QBOOT_OTP_MARKER_ADDR);
  }

  /* Close the window again (another reset). If the reload only happens on
   * power-on reset this is harmless: the counter already reached the floor,
   * so the next boot takes the settled path and cannot loop here. */
  quilla_qboot_hal_otp_wrprot_set(1);     /* resets; never returns */
#else
  /* True-OTP parts burn synchronously inside floor_ratchet() during QIAP
   * SIGN, and builds with QUILLA_QBOOT_OTP_COUNTER = 0 have nothing to sync:
   * both take this no-op path with zero resets and zero option-byte wear. */
  (void)floor;
#endif
}

/** @} */
/** @} */

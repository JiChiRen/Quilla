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
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * This file is part of the Quilla operating system.
 */

/**
 * \file
 * Clock cross-checking implementation. See dev/qclk.h for the rationale,
 * for what this technique can and cannot detect, and for why the blocking
 * calibration loop is bounded.
 *
 * The reference timer is driven straight through CMSIS rather than through
 * the HAL TIM driver: all that is needed is a free-running counter, and the
 * HAL TIM sources are not in either build's source list. Pulling them in
 * would cost ROM for no behavioural benefit.
 *
 * Failure policy: every failure path returns a kern_status_t and never
 * freezes the kernel itself. qclk is an optional cross-check - its only
 * product is a narrower watchdog window - so a failed measurement must not
 * cost availability. main() owns the degradation decision (see the comment at
 * the qclk_init() call site): on failure the watchdog falls back to its
 * compile-time assumption and the periodic check keeps watching the ratio.
 */

#include "rtimer-arch.h"
#include "dev/qclk.h"
#include "py32f0xx_hal.h"

#include <stdint.h>


/* The reference counter is 16 bits wide, so a window longer than its full
 * scale could never be observed to close. */
#if QCLK_CONF_WINDOW_TICKS > 0xFFFFUL
#error "QCLK_CONF_WINDOW_TICKS exceeds the 16-bit reference counter"
#endif

#if QCLK_CONF_TOLERANCE_PPT >= 1000UL
#error "QCLK_CONF_TOLERANCE_PPT must stay below 1000 (parts per thousand)"
#endif

#if QCLK_CONF_LSI_MIN_HZ >= QCLK_CONF_LSI_MAX_HZ
#error "QCLK_CONF_LSI_MIN_HZ must be below QCLK_CONF_LSI_MAX_HZ"
#endif

/* Widest prescaler field. The divisor is PSC + 1, so the field's maximum
 * value 0xFFFF admits 0x10000 as a divisor. */
#define QCLK_PSC_MAX                0xFFFFUL
#define QCLK_DIV_MAX                (QCLK_PSC_MAX + 1UL)

#define QCLK_CNT_MASK               0xFFFFUL

static uint16_t qclk_ref_start;
static uint32_t qclk_tick_start;
static uint8_t  qclk_window_open;
static uint32_t qclk_window_hal_ms;
static unsigned int qclk_skips;
static uint8_t  qclk_ready;
static unsigned long qclk_ref_hz;
static unsigned long qclk_measured_hz;
static unsigned long qclk_last_hz;
static unsigned int qclk_windows;
static unsigned int qclk_violations;

/*---------------------------------------------------------------------------*/
static uint16_t
qclk_ref_now(void)
{
  return (uint16_t)((uint32_t)TIM14->CNT & QCLK_CNT_MASK);
}
/*---------------------------------------------------------------------------*/
/*
 * Unsigned subtraction on the counter width, so a wrap inside the window is
 * handled by the arithmetic rather than by special-casing it.
 */
static uint16_t
qclk_ref_elapsed(uint16_t from)
{
  return (uint16_t)(((uint32_t)qclk_ref_now() - (uint32_t)from) & QCLK_CNT_MASK);
}
/*---------------------------------------------------------------------------*/
static uint32_t
qclk_ticks_now(void)
{
  return QCLK_CONF_READ_TICKS();
}
/*---------------------------------------------------------------------------*/
/*
 * Rate implied by one window, in Hz.
 *
 * The window is a compile-time constant, so the only division is by that
 * constant and the compiler turns it into a multiply. No 64-bit helper is
 * pulled in. The product is checked before it is formed: window counts times
 * the reference rate is bounded by the window length times the real rate, but
 * a corrupt reading must still not wrap.
 */
static kern_status_t
qclk_rate_from_window(uint32_t tick_delta, uint32_t ref_window_ticks,
                      unsigned long *hz)
{
  unsigned long ref_hz = qclk_ref_hz;

  if((ref_hz == 0UL) || (hz == NULL)) {
    return KERN_ERR_RANGE;
  }
  if((ref_window_ticks == 0UL) ||
     (tick_delta > (uint32_t)(0xFFFFFFFFUL / ref_hz))) {
    return KERN_ERR_RANGE;
  }

  /* The window length has to be the reference counts the window ACTUALLY
   * ran, not the nominal QCLK_CONF_WINDOW_TICKS: the poll loop notices the
   * expiry only on the next idle pass, so every window runs at least one
   * pass latency long. Dividing by the nominal length turns that latency
   * straight into a positive rate error, and one long shell output (a
   * stretched pass) is enough to push a perfectly healthy clock out of the
   * band and into a false fault. */
  *hz = (unsigned long)(((uint32_t)ref_hz * tick_delta) / ref_window_ticks);

  if((*hz < (unsigned long)QCLK_CONF_LSI_MIN_HZ) ||
     (*hz > (unsigned long)QCLK_CONF_LSI_MAX_HZ)) {
    return KERN_ERR_RANGE;
  }
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
kern_status_t
qclk_init(void)
{
  unsigned long pclk;
  unsigned long divisor;
  unsigned long ref_hz;

  qclk_ready = 0U;

  __HAL_RCC_TIM14_CLK_ENABLE();

  pclk = (unsigned long)HAL_RCC_GetPCLK1Freq();
  if(pclk == 0UL) {
    /* Cannot derive a reference divisor. Reported, not frozen: main() degrades
     * the watchdog to its compile-time assumption. */
    return KERN_ERR_RANGE;
  }

  /* Round the divisor up so the resulting rate lands at or under the target
   * rather than over it: a faster reference only shortens the window and
   * costs resolution, a slower one stretches the blocking calibration. */
  divisor = (pclk + ((unsigned long)QCLK_CONF_REF_HZ - 1UL)) /
            (unsigned long)QCLK_CONF_REF_HZ;
  if(divisor == 0UL) {
    divisor = 1UL;
  }
  if(divisor > QCLK_DIV_MAX) {
    /* Reference rate too low for the timer to cover the window. Reported, not
     * frozen: main() degrades the watchdog to its compile-time assumption. */
    return KERN_ERR_RANGE;
  }

  ref_hz = pclk / divisor;
  if((ref_hz < (unsigned long)QCLK_CONF_REF_MIN_HZ) ||
     (ref_hz > (unsigned long)QCLK_CONF_REF_MAX_HZ)) {
    /* Reference rate outside the band the watchdog math was sized for.
     * Reported, not frozen: main() degrades to its compile-time assumption. */
    return KERN_ERR_RANGE;
  }

  TIM14->PSC = (uint16_t)(divisor - 1UL);
  TIM14->ARR = (uint16_t)QCLK_CNT_MASK;
  TIM14->EGR = TIM_EGR_UG;      /* latch PSC and ARR */
  TIM14->SR = 0U;               /* discard the update the UG bit produced */
  TIM14->CR1 = TIM_CR1_CEN;

  qclk_ref_hz = ref_hz;
  qclk_window_open = 0U;
  qclk_measured_hz = 0UL;
  qclk_last_hz = 0UL;
  qclk_windows = 0U;
  qclk_violations = 0U;
  qclk_skips = 0U;
  qclk_ready = 1U;

  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
kern_status_t
qclk_calibrate(void)
{
  uint32_t t0;
  uint32_t ref_ticks;
  unsigned long hz = 0UL;
  kern_status_t st;

  if(qclk_ready == 0U) {
    return KERN_ERR_NOT_READY;
  }

  t0 = qclk_ticks_now();
  qclk_ref_start = qclk_ref_now();

  /* Bounded: qclk_init() has already rejected a reference rate below
   * QCLK_CONF_REF_MIN_HZ, so this cannot take longer than
   * QCLK_CONF_WINDOW_TICKS / QCLK_CONF_REF_MIN_HZ seconds. See dev/qclk.h. */
  while(qclk_ref_elapsed(qclk_ref_start) < (uint16_t)QCLK_CONF_WINDOW_TICKS) {
    ;
  }

  qclk_windows++;
  ref_ticks = (uint32_t)qclk_ref_elapsed(qclk_ref_start);
  st = qclk_rate_from_window(qclk_ticks_now() - t0, ref_ticks, &hz);
  if(st != KERN_OK) {
    qclk_last_hz = 0UL;
    qclk_violations++;
    return KERN_ERR_CLOCK;
  }

  qclk_measured_hz = hz;
  qclk_last_hz = hz;
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
kern_status_t
qclk_poll(void)
{
  uint32_t delta;
  unsigned long hz = 0UL;
  unsigned long expected;
  unsigned long lo;
  unsigned long hi;
  kern_status_t st;

  if(qclk_ready == 0U) {
    return KERN_ERR_NOT_READY;
  }

  if(qclk_window_open == 0U) {
    qclk_tick_start = qclk_ticks_now();
    qclk_ref_start = qclk_ref_now();
    qclk_window_hal_ms = HAL_GetTick();
    qclk_window_open = 1U;
    return KERN_OK;
  }

  if(qclk_ref_elapsed(qclk_ref_start) < (uint16_t)QCLK_CONF_WINDOW_TICKS) {
    return KERN_OK;
  }

  /* The reference counter is 16 bits wide and wraps every 65.536 ms. If one
   * idle-pass gap stretched past a full counter turn (long shell output can
   * easily do that), the elapsed value above is truncated and the rate
   * computed below is wildly wrong - a false KERN_ERR_CLOCK that ends in
   * the fault freeze. Detect the overrun through the 32-bit HAL millisecond
   * tick and reopen the window instead of comparing a corrupted reading:
   * the next window closes with a clean one. A genuine LSI drift keeps
   * closing windows inside this guard and still faults through the band
   * check below. The guard is the window length plus a pass-latency
   * margin, always below one full counter turn. */
  if((HAL_GetTick() - qclk_window_hal_ms) >
     (QCLK_CONF_WINDOW_TICKS / 1000UL) + 10UL) {
    qclk_window_open = 0U;
    qclk_skips++;
    return KERN_OK;
  }

  qclk_window_open = 0U;
  delta = qclk_ticks_now() - qclk_tick_start;
  qclk_windows++;

  /* The guard above caps the true window at 60 ms, safely below the 16-bit
   * counter's 65.536 ms turn, so this elapsed reading is the untruncated
   * real window length - the correct denominator for the rate. */
  st = qclk_rate_from_window(delta,
                             (uint32_t)qclk_ref_elapsed(qclk_ref_start), &hz);
  if(st != KERN_OK) {
    qclk_last_hz = 0UL;
    qclk_violations++;
    return KERN_ERR_CLOCK;
  }

  qclk_last_hz = hz;

  /* Until a calibration has produced a number, the band is built around the
   * assumed nominal rate and is correspondingly wide in practice. */
  expected = (qclk_measured_hz != 0UL) ? qclk_measured_hz
                                       : (unsigned long)QCLK_CONF_LSI_NOMINAL_HZ;
  lo = (expected * (1000UL - (unsigned long)QCLK_CONF_TOLERANCE_PPT)) / 1000UL;
  hi = (expected * (1000UL + (unsigned long)QCLK_CONF_TOLERANCE_PPT)) / 1000UL;

  if((hz < lo) || (hz > hi)) {
    qclk_violations++;
    return KERN_ERR_CLOCK;
  }
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
unsigned long
qclk_measured_lsi_hz(void)
{
  return qclk_measured_hz;
}
/*---------------------------------------------------------------------------*/
unsigned long
qclk_reference_hz(void)
{
  return qclk_ref_hz;
}
/*---------------------------------------------------------------------------*/
unsigned int
qclk_window_count(void)
{
  return qclk_windows;
}
/*---------------------------------------------------------------------------*/
unsigned int
qclk_violation_count(void)
{
  return qclk_violations;
}
/*---------------------------------------------------------------------------*/
unsigned long
qclk_last_window_hz(void)
{
  return qclk_last_hz;
}
/*---------------------------------------------------------------------------*/
static char *
qclk_put_str(char *dst, const char *src, char *limit)
{
  while((*src != '\0') && (dst < limit)) {
    *dst++ = *src++;
  }
  return dst;
}
/*---------------------------------------------------------------------------*/
static char *
qclk_put_dec(char *dst, unsigned long v, char *limit)
{
  char tmp[12];
  unsigned int n = 0U;

  if(v == 0UL) {
    tmp[n++] = '0';
  }
  while(v != 0UL) {
    tmp[n++] = (char)('0' + (char)(v % 10UL));
    v /= 10UL;
  }
  while((n > 0U) && (dst < limit)) {
    *dst++ = tmp[--n];
  }
  return dst;
}
/*---------------------------------------------------------------------------*/
unsigned int
qclk_describe(char *dst, unsigned int len)
{
  char *p = dst;
  char *limit;

  if((dst == NULL) || (len < 2U)) {
    return 0U;
  }
  limit = dst + (len - 1U);

  if(qclk_ready == 0U) {
    /* Init failed or never ran: the watchdog is dimensioned on the
     * compile-time assumption. Report that instead of staying silent, so the
     * line on the shell states the assumption actually in use. */
    p = qclk_put_str(p, "clk: NOT READY (watchdog on assumed ", limit);
    p = qclk_put_dec(p, (unsigned long)QCLK_CONF_LSI_NOMINAL_HZ, limit);
    p = qclk_put_str(p, "Hz)", limit);
    *p = '\0';
    return (unsigned int)(p - dst);
  }

  p = qclk_put_str(p, "clk: ref=", limit);
  p = qclk_put_dec(p, qclk_ref_hz, limit);
  p = qclk_put_str(p, "Hz lsi=", limit);

  if(qclk_measured_hz == 0UL) {
    p = qclk_put_str(p, "NOT MEASURED", limit);
  } else {
    p = qclk_put_dec(p, qclk_measured_hz, limit);
    p = qclk_put_str(p, "Hz", limit);
  }

  p = qclk_put_str(p, " last=", limit);
  p = qclk_put_dec(p, qclk_last_hz, limit);
  p = qclk_put_str(p, "Hz win=", limit);
  p = qclk_put_dec(p, (unsigned long)qclk_windows, limit);
  p = qclk_put_str(p, " viol=", limit);
  p = qclk_put_dec(p, (unsigned long)qclk_violations, limit);
  p = qclk_put_str(p, " tol=+/-", limit);
  p = qclk_put_dec(p, (unsigned long)QCLK_CONF_TOLERANCE_PPT, limit);
  p = qclk_put_str(p, "ppt", limit);

  *p = '\0';
  return (unsigned int)(p - dst);
}
/*---------------------------------------------------------------------------*/

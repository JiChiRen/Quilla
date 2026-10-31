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
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS
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
 * Clock cross-checking implementation. See dev/qclk.h for the rationale, for
 * what this technique can and cannot detect, and for why every loop here is
 * bounded.
 *
 * The RTC is driven through the vendor's small register API rather than
 * hand-written stores: this port already links gd32f50x_pmu.c (lpm.c needs it)
 * and adding gd32f50x_rtc.c costs only the four functions actually referenced,
 * because every image links with section elimination on. The handshake is then
 * the library's to get right rather than ours to get wrong.
 *
 * Failure policy: every failure path returns a kern_status_t and never
 * freezes the kernel itself. qclk is an optional cross-check - its only
 * product is a narrower watchdog window - so a failed measurement must not
 * cost availability. main() owns the degradation decision (see the comment at
 * the qclk_init() call site): on failure the watchdog falls back to its
 * compile-time assumption and the periodic check keeps watching the ratio.
 */

#include "contiki.h"

#include "rtimer-arch.h"
#include "dev/qclk.h"

#include "gd32f50x.h"

#include <stdint.h>


/* The window constant is multiplied by the tick rate below; keep the product
 * inside 32 bits so no helper has to be pulled in for it. */
#if QCLK_CONF_WINDOW_IRC40K > (0xFFFFFFFFUL / RTIMER_ARCH_SECOND)
#error "QCLK_CONF_WINDOW_IRC40K * RTIMER_ARCH_SECOND overflows 32 bits"
#endif

/* Modulus of the RTC divider: it reloads from QCLK_CONF_RTC_PSC, so one
 * revolution is that value plus one. The window is two orders of magnitude
 * smaller than either candidate reading of this modulus, which is why the
 * arithmetic below does not have to be exact about it. */
#define QCLK_REF_MODULO       ((unsigned long)QCLK_CONF_RTC_PSC + 1UL)

static unsigned long qclk_expected_ticks;
static unsigned long qclk_tick_limit;
static unsigned long qclk_ref_start;
static uint32_t qclk_tick_start;
static uint8_t qclk_window_open;
static uint8_t qclk_ready;
static unsigned long qclk_measured_hz;
static unsigned long qclk_last_hz;
static unsigned long qclk_last_ticks;
static unsigned int qclk_windows;
static unsigned int qclk_violations;

/*---------------------------------------------------------------------------*/
/*
 * One coherent sample of the RTC divider. Reading DIVH then DIVL crosses a
 * clock boundary, so a stray read can straddle a reload; two identical reads
 * in a row are taken as coherent. Bounded: after QCLK_CONF_READ_RETRIES the
 * newest value wins.
 */
static unsigned long
qclk_ref_read(void)
{
  unsigned long v = (unsigned long)rtc_divider_get();
  unsigned long next;
  unsigned int i;

  for(i = 0U; i < (unsigned int)QCLK_CONF_READ_RETRIES; i++) {
    next = (unsigned long)rtc_divider_get();
    if(next == v) {
      break;
    }
    v = next;
  }
  return v;
}
/*---------------------------------------------------------------------------*/
/* The divider counts down, so elapsed is (start - now) taken modulo this
 * counter's own modulus rather than modulo its register width. */
static unsigned long
qclk_ref_elapsed(unsigned long from)
{
  unsigned long now = qclk_ref_read();

  if(now <= from) {
    return from - now;
  }
  return (QCLK_REF_MODULO - now) + from;
}
/*---------------------------------------------------------------------------*/
static uint32_t
qclk_tick_now(void)
{
  return (uint32_t)rtimer_arch_now();
}
/*---------------------------------------------------------------------------*/
static uint32_t
qclk_tick_elapsed(uint32_t from)
{
  return (uint32_t)(qclk_tick_now() - from);
}
/*---------------------------------------------------------------------------*/
/*
 * Rate implied by one window, in Hz.
 *
 * Only called on a completed window, and the guard below is what keeps the
 * product inside 32 bits: a count larger than this cannot be a good reading
 * any more anyway.
 */
static kern_status_t
qclk_rate(unsigned long irc_delta, uint32_t tick_delta, unsigned long *hz)
{
  unsigned long num;

  if(hz == NULL) {
    return KERN_ERR_RANGE;
  }
  if((tick_delta == 0U) || (irc_delta == 0UL)) {
    return KERN_ERR_RANGE;
  }
  if(irc_delta > (unsigned long)(0xFFFFFFFFUL / (unsigned long)RTIMER_ARCH_SECOND)) {
    return KERN_ERR_RANGE;
  }

  num = irc_delta * (unsigned long)RTIMER_ARCH_SECOND;
  *hz = num / (unsigned long)tick_delta;

  if((*hz < (unsigned long)QCLK_CONF_IRC40K_MIN_HZ) ||
     (*hz > (unsigned long)QCLK_CONF_IRC40K_MAX_HZ)) {
    return KERN_ERR_RANGE;
  }
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
/*
 * What one window should count, and how far apart its two edges may drift.
 * Both are derived from compile-time constants, so this runs once.
 */
static void
qclk_derive_window(void)
{
  unsigned long ticks;

  ticks = (unsigned long)(((unsigned long)QCLK_CONF_WINDOW_IRC40K *
                           (unsigned long)RTIMER_ARCH_SECOND) /
                          (unsigned long)QCLK_CONF_IRC40K_NOMINAL_HZ);
  if(ticks == 0UL) {
    ticks = 1UL;
  }
  qclk_expected_ticks = ticks;
  qclk_tick_limit = ticks * (unsigned long)QCLK_CONF_MAX_TICK_STRETCH_X;
}
/*---------------------------------------------------------------------------*/
static void
qclk_window_open_at(void)
{
  qclk_ref_start = qclk_ref_read();
  qclk_tick_start = qclk_tick_now();
  qclk_window_open = 1U;
}
/*---------------------------------------------------------------------------*/
/* Open or advance the current window, then close and judge it if it is due.
 * Both completed-window paths land here so calibrate() and poll() cannot
 * disagree about what a window is. */
static kern_status_t
qclk_step(void)
{
  uint32_t tick_delta;
  unsigned long irc_delta;
  unsigned long hz = 0UL;
  unsigned long centre;
  unsigned long tol;
  unsigned long lo;
  unsigned long hi;
  kern_status_t st;

  if(qclk_window_open == 0U) {
    qclk_window_open_at();
    return KERN_OK;
  }

  tick_delta = qclk_tick_elapsed(qclk_tick_start);

  /* Close an overstretched window too: that is the case in which the IRC40K
   * side has stopped advancing and would otherwise never close it. */
  if((tick_delta < qclk_tick_limit) &&
     (qclk_ref_elapsed(qclk_ref_start) <
      (unsigned long)QCLK_CONF_WINDOW_IRC40K)) {
    return KERN_OK;
  }

  irc_delta = qclk_ref_elapsed(qclk_ref_start);
  qclk_window_open = 0U;
  qclk_windows++;

  st = qclk_rate(irc_delta, tick_delta, &hz);
  qclk_last_ticks = (unsigned long)tick_delta;
  if(st != KERN_OK) {
    qclk_last_hz = 0UL;
    qclk_violations++;
    return KERN_ERR_CLOCK;
  }
  qclk_last_hz = hz;

  /* Before the first measurement the band sits on the assumed rate and is
   * correspondingly wide; afterwards it sits on what this board did. */
  if(qclk_measured_hz != 0UL) {
    centre = (unsigned long)tick_delta;
    tol = (unsigned long)QCLK_CONF_TOLERANCE_PPT;
  } else {
    centre = qclk_expected_ticks;
    tol = (unsigned long)QCLK_CONF_ASSUMED_TOLERANCE_PPT;
  }
  lo = (centre * (1000UL - tol)) / 1000UL;
  hi = (centre * (1000UL + tol)) / 1000UL;

  if(((unsigned long)tick_delta < lo) || ((unsigned long)tick_delta > hi)) {
    qclk_violations++;
    return KERN_ERR_CLOCK;
  }
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/
/*
 * Close the backup domain again: unlock the PMU register file (it is
 * write-protected behind PMU_CTL0.LOCK, see qclk_init()), clear BKPWEN and
 * re-lock so no backup-domain write is left enabled for the rest of the run.
 */
static void
qclk_backup_close(void)
{
  pmu_unlock();
  pmu_backup_write_disable();
  pmu_lock();
}
/*---------------------------------------------------------------------------*/
kern_status_t
qclk_init(void)
{
  unsigned long ticks_wanted;

  qclk_ready = 0U;
  qclk_measured_hz = 0UL;
  qclk_last_hz = 0UL;
  qclk_last_ticks = 0UL;
  qclk_windows = 0U;
  qclk_violations = 0U;
  qclk_window_open = 0U;

  qclk_derive_window();
  ticks_wanted = qclk_expected_ticks;
  if(ticks_wanted == 0UL) {
    /* Static configuration is inconsistent. Report it and let main() fall
     * back to the compile-time assumption like any other init failure. */
    return KERN_ERR_RANGE;
  }

  /* The RTC and its clock entry live in the backup domain, which is write
   * protected until PMU says otherwise. On the F50x the PMU register file is
   * itself write-protected behind PMU_CTL0.LOCK (bit 31), which resets to 0
   * (locked), and the library's pmu_backup_write_enable() only ORs BKPWEN -
   * it does not unlock. Without the unlock that OR is silently dropped by
   * hardware, BKPWEN stays 0, and every later write to RCU_BDCTL (RTCSRC,
   * RTCEN) is dropped as well: BDCTL then reads back its reset default and
   * the RTC register file never sees CK_RTC. (Confirmed against the
   * GD32F50x SVD: PMU_CTL0 reset value has LOCK = 0.) */
  rcu_periph_clock_enable(RCU_PMU);
  rcu_periph_clock_enable(RCU_BKPI);
  pmu_unlock();
  pmu_backup_write_enable();

  rcu_osci_on(RCU_IRC40K);
  if(ERROR == rcu_osci_stab_wait(RCU_IRC40K)) {
    /* No stable reference to measure against. Reported, not frozen: main()
     * degrades the watchdog to its compile-time assumption. */
    qclk_backup_close();
    return KERN_ERR_INTERNAL;
  }

  /* Select the source, then open the RTC's own gate. rcu_rtc_clock_config()
   * only writes the RTCSRC field of RCU_BDCTL; RTCEN is a separate bit of the
   * same register (RCU_RTC = BDCTL bit 15) and nothing else turns it on. With
   * the source selected but the gate closed the divider does not advance and
   * every window would close on the tick bound instead. */
  rcu_rtc_clock_config(RCU_RTCSRC_IRC40K);
  rcu_periph_clock_enable(RCU_RTC);

  /* Once CK_RTC is gated on, the RTC register file has to resynchronise with
   * the APB side and any write left over from before the gate opened has to
   * drain. Until both have happened a write to a RTC register is not accepted
   * at all, and the LWOFF handshake below can then never complete. Both waits
   * are the library's own bounded forms. */
  rtc_register_sync_wait();
  rtc_lwoff_wait();

  rtc_prescaler_set((uint32_t)QCLK_CONF_RTC_PSC);

  /* Wait for that write to land rather than reading the divider immediately:
   * until it does, the counter is still running at whatever modulus survived
   * the last reset, which is the one number the arithmetic above assumes it
   * is not. rtc_lwoff_wait() is the library's own bounded form of this wait. */
  if(ERROR == rtc_lwoff_wait()) {
    /* The prescaler write never landed, so the divider is not counting at
     * the modulus the arithmetic above assumes. Reported, not frozen: main()
     * degrades the watchdog to its compile-time assumption. */
    qclk_backup_close();
    return KERN_ERR_INTERNAL;
  }

  /* Do not leave the backup domain writable for the rest of the run. */
  qclk_backup_close();

  qclk_ready = 1U;
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
kern_status_t
qclk_calibrate(void)
{
  unsigned long hz = 0UL;
  unsigned long irc_delta;
  uint32_t tick_delta;
  uint32_t t0;
  unsigned long ref0;
  kern_status_t st;

  if(qclk_ready == 0U) {
    return KERN_ERR_NOT_READY;
  }

  /* A dedicated blocking window rather than a call into qclk_step(): that
   * routine opens a window and returns, which is what the periodic path wants
   * but not what a caller asking "what rate is it?" needs. Bounded from both
   * sides - see dev/qclk.h, TERMINATION. */
  ref0 = qclk_ref_read();
  t0 = qclk_tick_now();

  while(1) {
    tick_delta = qclk_tick_elapsed(t0);
    irc_delta = qclk_ref_elapsed(ref0);
    if(irc_delta >= (unsigned long)QCLK_CONF_WINDOW_IRC40K) {
      break;
    }
    if(tick_delta >= qclk_tick_limit) {
      break;
    }
  }

  qclk_windows++;
  tick_delta = qclk_tick_elapsed(t0);
  irc_delta = qclk_ref_elapsed(ref0);
  qclk_last_ticks = (unsigned long)tick_delta;

  st = qclk_rate(irc_delta, tick_delta, &hz);
  if(st != KERN_OK) {
    qclk_last_hz = 0UL;
    qclk_violations++;
    return KERN_ERR_CLOCK;
  }

  qclk_last_hz = hz;
  qclk_measured_hz = hz;
  qclk_window_open = 0U;
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
kern_status_t
qclk_poll(void)
{
  if(qclk_ready == 0U) {
    return KERN_ERR_NOT_READY;
  }
  return qclk_step();
}
/*---------------------------------------------------------------------------*/
unsigned long
qclk_measured_irc40k_hz(void)
{
  return qclk_measured_hz;
}
/*---------------------------------------------------------------------------*/
unsigned long
qclk_reference_hz(void)
{
  return (qclk_ready != 0U) ? (unsigned long)RTIMER_ARCH_SECOND : 0UL;
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
unsigned long
qclk_last_window_ticks(void)
{
  return qclk_last_ticks;
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
    p = qclk_put_dec(p, (unsigned long)QCLK_CONF_IRC40K_NOMINAL_HZ, limit);
    p = qclk_put_str(p, "Hz)", limit);
    *p = '\0';
    return (unsigned int)(p - dst);
  }

  p = qclk_put_str(p, "clk: ticks=", limit);
  p = qclk_put_dec(p, (unsigned long)RTIMER_ARCH_SECOND, limit);
  p = qclk_put_str(p, "Hz vs irc40k=", limit);

  if(qclk_measured_hz == 0UL) {
    p = qclk_put_str(p, "NOT MEASURED", limit);
  } else {
    p = qclk_put_dec(p, qclk_measured_hz, limit);
    p = qclk_put_str(p, "Hz", limit);
  }

  p = qclk_put_str(p, " last=", limit);
  p = qclk_put_dec(p, qclk_last_hz, limit);
  p = qclk_put_str(p, "Hz ticks=", limit);
  p = qclk_put_dec(p, qclk_last_ticks, limit);
  p = qclk_put_str(p, "/", limit);
  p = qclk_put_dec(p, qclk_expected_ticks, limit);
  p = qclk_put_str(p, " win=", limit);
  p = qclk_put_dec(p, (unsigned long)qclk_windows, limit);
  p = qclk_put_str(p, " viol=", limit);
  p = qclk_put_dec(p, (unsigned long)qclk_violations, limit);
  p = qclk_put_str(p, " tol=+/-", limit);
  p = qclk_put_dec(p, (unsigned long)((qclk_measured_hz != 0UL)
                                        ? QCLK_CONF_TOLERANCE_PPT
                                        : QCLK_CONF_ASSUMED_TOLERANCE_PPT),
                   limit);
  p = qclk_put_str(p, "ppt", limit);

  *p = '\0';
  return (unsigned int)(p - dst);
}
/*---------------------------------------------------------------------------*/

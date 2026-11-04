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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
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
 * \file
 * Clock cross-checking for the CH32V305VB: measure the IWDG's LSI clock
 * against the rtimer time base. See dev/qclk.h for the rationale, for what
 * this technique can and cannot detect, and for why every loop here is
 * bounded. Functional design is aligned to the GD32F502VE qclk.c; side A is
 * TIM1 (rtimer, 1 MHz), side B is LSI counted by the RTC divider.
 *
 * Failure policy: every failure path returns a kern_status_t and never
 * freezes the kernel. A failed measurement must not cost availability; main()
 * owns the degradation decision (the watchdog falls back to its compile-time
 * assumption).
 */

#include "contiki.h"

#include "rtimer-arch.h"
#include "dev/qclk.h"

#include "ch32v30x.h"

#include <stdint.h>

#if QCLK_CONF_WINDOW_IRC40K > (0xFFFFFFFFUL / RTIMER_ARCH_SECOND)
#error "QCLK_CONF_WINDOW_IRC40K * RTIMER_ARCH_SECOND overflows 32 bits"
#endif

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
static unsigned long
qclk_ref_read(void)
{
  unsigned long v = (unsigned long)RTC_GetDivider();
  unsigned long next;
  unsigned int i;

  for(i = 0U; i < (unsigned int)QCLK_CONF_READ_RETRIES; i++) {
    next = (unsigned long)RTC_GetDivider();
    if(next == v) {
      break;
    }
    v = next;
  }
  return v;
}
/*---------------------------------------------------------------------------*/
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
kern_status_t
qclk_init(void)
{
  unsigned long ticks_wanted;
  uint32_t to;

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
    return KERN_ERR_RANGE;
  }

  /* The RTC lives in the backup domain; open the PWR/BKP clocks, start LSI,
   * select it as the RTC clock entry and get the divider counting. */
  RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
  RCC_LSICmd(ENABLE);
  to = 0x00100000U;
  while((RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET) && (to != 0U)) {
    to--;
  }
  if(RCC_GetFlagStatus(RCC_FLAG_LSIRDY) == RESET) {
    return KERN_ERR_INTERNAL;
  }

  RCC_RTCCLKConfig(RCC_RTCCLKSource_LSI);
  RCC_RTCCLKCmd(ENABLE);
  RTC_WaitForLastTask();

  RTC_SetPrescaler((uint32_t)QCLK_CONF_RTC_PSC);
  RTC_WaitForLastTask();

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
    p = qclk_put_str(p, "clk: NOT READY (watchdog on assumed ", limit);
    p = qclk_put_dec(p, (unsigned long)QCLK_CONF_IRC40K_NOMINAL_HZ, limit);
    p = qclk_put_str(p, "Hz)", limit);
    *p = '\0';
    return (unsigned int)(p - dst);
  }

  p = qclk_put_str(p, "clk: ticks=", limit);
  p = qclk_put_dec(p, (unsigned long)RTIMER_ARCH_SECOND, limit);
  p = qclk_put_str(p, "Hz vs lsi=", limit);

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

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
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
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
 * GD32F502VE FWDGT configuration. See dev/qwdt.h for the rationale.
 *
 * This module owns the peripheral: it is the only place that converts a
 * requested timeout into a divider/reload pair and the only place that writes
 * those registers.
 *
 * Prescaler table note. The vendor header lists divisors up to DIV32768, but
 * the two definitions it uses are not consistent about the field width:
 * FWDGT_PSC_PSC says BITS(0,2) while PSC_PSC() masks with BITS(0,3). Rather
 * than bet on either, the table below stops at the widest divisor every
 * reading agrees exists (DIV256). Anything this design needs is reachable with
 * it: at the nominal rate and the default 500 ms target the selection lands on
 * DIV8 with a reload of 2499.
 */

#include "contiki.h"
#include "dev/qwdt.h"

#include "gd32f50x.h"
#include "sys/int-master.h"

#include <stdint.h>

/* Widest reload the down-counter accepts: GD32F50x FWDGT has a 12-bit counter. */
#define QWDT_RELOAD_MAX             0x0FFFU
#define QWDT_COUNT_MAX              (QWDT_RELOAD_MAX + 1U)

/* Static bound, written as a division because a cast is not permitted in #if. */
#if QWDT_CONF_TIMEOUT_MS > (4294967295UL / QWDT_CONF_IRC40K_NOMINAL_HZ)
#error "QWDT_CONF_TIMEOUT_MS too large: the tick product overflows 32 bits"
#endif

typedef struct {
  unsigned long div;
  uint8_t       lib;
} qwdt_div_t;

/* Ascending order. The first entry the timeout fits inside is selected, which
 * maximises the reload and therefore the timer resolution. */
static const qwdt_div_t qwdt_divs[] = {
  { 4UL,    FWDGT_PSC_DIV4   },
  { 8UL,    FWDGT_PSC_DIV8   },
  { 16UL,   FWDGT_PSC_DIV16  },
  { 32UL,   FWDGT_PSC_DIV32  },
  { 64UL,   FWDGT_PSC_DIV64  },
  { 128UL,  FWDGT_PSC_DIV128 },
  { 256UL,  FWDGT_PSC_DIV256 }
};

#define QWDT_NDIVS  (sizeof(qwdt_divs) / sizeof(qwdt_divs[0]))

static unsigned long qwdt_div_idx;
static uint32_t qwdt_count;         /* reload + 1                          */
static uint8_t qwdt_configured;
static uint8_t qwdt_running;
static unsigned long qwdt_measured_hz;  /* 0 = still assuming            */

/*---------------------------------------------------------------------------*/
/* Round up. Used wherever being optimistic would weaken the claim. */
static uint32_t
div_ceil(uint32_t a, uint32_t b)
{
  return (b == 0U) ? 0U : ((a + b - 1U) / b);
}
/*---------------------------------------------------------------------------*/
static unsigned int
timeout_ms_at(uint32_t hz, uint8_t round_up)
{
  uint32_t num;

  if(hz == 0U) {
    return 0U;
  }

  /* count <= 4096, div <= 256 -> product <= 1.05e6 ticks; *1000 fits in 32
   * bits with room to spare. */
  num = qwdt_count * (uint32_t)qwdt_divs[qwdt_div_idx].div * 1000U;
  return round_up ? (unsigned int)div_ceil(num, hz)
                  : (unsigned int)(num / hz);
}
/*---------------------------------------------------------------------------*/
/* Rate the divider/reload pair is chosen for: measured when available,
 * otherwise the assumption this module ships with. */
static uint32_t
qwdt_rate_nominal(void)
{
  return (qwdt_measured_hz != 0UL) ? (uint32_t)qwdt_measured_hz
                                   : (uint32_t)QWDT_CONF_IRC40K_NOMINAL_HZ;
}
/*---------------------------------------------------------------------------*/
/* Fastest rate admitted. Drives the shortest timeout, i.e. how often the
 * software must feed. */
static uint32_t
qwdt_rate_fastest(void)
{
  unsigned long v;

  if(qwdt_measured_hz != 0UL) {
    v = (qwdt_measured_hz *
         (1000UL + (unsigned long)QWDT_CONF_MEASURED_SPREAD_PPT)) / 1000UL;
  } else {
    v = (unsigned long)QWDT_CONF_IRC40K_MAX_HZ;
  }
  return (v == 0UL) ? 1U : (uint32_t)v;
}
/*---------------------------------------------------------------------------*/
/* Slowest rate admitted. Drives the longest timeout, i.e. what is compared
 * against the process safety time. */
static uint32_t
qwdt_rate_slowest(void)
{
  unsigned long v;

  if(qwdt_measured_hz != 0UL) {
    v = (qwdt_measured_hz *
         (1000UL - (unsigned long)QWDT_CONF_MEASURED_SPREAD_PPT)) / 1000UL;
  } else {
    v = (unsigned long)QWDT_CONF_IRC40K_MIN_HZ;
  }
  return (v == 0UL) ? 1U : (uint32_t)v;
}
/*---------------------------------------------------------------------------*/
/* Pick the first divider the timeout fits inside, which maximises the reload
 * and therefore the resolution. */
static kern_status_t
qwdt_select(uint32_t hz)
{
  unsigned int i;

  for(i = 0U; i < (unsigned int)QWDT_NDIVS; i++) {
    uint32_t denom = 1000U * (uint32_t)qwdt_divs[i].div;
    uint32_t count = ((uint32_t)QWDT_CONF_TIMEOUT_MS * hz) / denom;

    if((count >= 1U) && (count <= QWDT_COUNT_MAX)) {
      qwdt_div_idx = (unsigned long)i;
      qwdt_count = count;
      return KERN_OK;
    }
  }
  return KERN_ERR_RANGE;
}
/*---------------------------------------------------------------------------*/
kern_status_t
qwdt_init(void)
{
  qwdt_configured = 0U;

  if(qwdt_select(qwdt_rate_nominal()) != KERN_OK) {
    /* No divider/reload combination reaches the requested timeout. */
    kern_raise(KERN_ERR_RANGE, KERN_MOD_WDT, (unsigned int)__LINE__);
    return KERN_ERR_RANGE;
  }

  /* Post-condition rather than a guard on previous state: qwdt_div_idx is
   * what every accessor below indexes the table with, so it is validated
   * where it is produced. */
  if(qwdt_div_idx >= (unsigned long)QWDT_NDIVS) {
    kern_raise(KERN_ERR_RANGE, KERN_MOD_WDT, (unsigned int)__LINE__);
    return KERN_ERR_RANGE;
  }

  /* The longest admitted timeout is the one that matters against the hazard:
   * a slow IRC40K stretches it. If that already exceeds the process safety
   * time, this configuration cannot be defended and must not be armed
   * silently. */
  if((QWDT_CONF_SAFE_TIME_MS != 0U) &&
     (qwdt_timeout_max_ms() > (unsigned int)QWDT_CONF_SAFE_TIME_MS)) {
    kern_raise(KERN_ERR_RANGE, KERN_MOD_WDT, (unsigned int)__LINE__);
    return KERN_ERR_RANGE;
  }

  qwdt_configured = 1U;
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
kern_status_t
qwdt_start(void)
{
  uint16_t reload;

  if(qwdt_configured == 0U) {
    return KERN_ERR_NOT_READY;
  }
  if(qwdt_running != 0U) {
    return KERN_OK;
  }

  reload = (uint16_t)(qwdt_count - 1U);

  /* fwdgt_config() writes both registers with write access enabled and waits
   * for each update to land (FWDGT_STAT_PUD / RUD); it returns ERROR if that
   * wait timed out, which is exactly the failure worth reporting rather than
   * swallowing. Enabling afterwards means nothing is armed unless both division
   * and reload are known to have taken effect. */
  if(ERROR == fwdgt_config(reload, qwdt_divs[qwdt_div_idx].lib)) {
    kern_raise(KERN_ERR_INTERNAL, KERN_MOD_WDT, (unsigned int)__LINE__);
    return KERN_ERR_INTERNAL;
  }

  fwdgt_counter_reload();
  fwdgt_enable();

  qwdt_running = 1U;
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
kern_status_t
qwdt_refresh(void)
{
  if(qwdt_running == 0U) {
    return KERN_ERR_ALREADY;
  }
  fwdgt_counter_reload();
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
void
qwdt_reboot(void)
{
  /* Nested cycle entry order: mask interrupts first, then let the peripheral
   * expire on its shortest setting, then spin. The previous PRIMASK is
   * deliberately discarded: this path ends in reset and nothing restores it. */
  (void)int_master_read_and_disable();

  /* Shortest interval this module can express, applied whether or not the
   * watchdog was already armed: a deliberate reset must not wait out whatever
   * timeout the application happens to be running. */
  (void)fwdgt_config(0U, FWDGT_PSC_DIV4);
  fwdgt_counter_reload();
  fwdgt_enable();

  while(1) {
  }
}
/*---------------------------------------------------------------------------*/
unsigned int
qwdt_is_started(void)
{
  return (qwdt_running != 0U) ? 1U : 0U;
}
/*---------------------------------------------------------------------------*/
unsigned int
qwdt_timeout_nominal_ms(void)
{
  if(qwdt_configured == 0U) {
    return 0U;
  }
  return timeout_ms_at(QWDT_CONF_IRC40K_NOMINAL_HZ, 0U);
}
/*---------------------------------------------------------------------------*/
unsigned int
qwdt_timeout_min_ms(void)
{
  if(qwdt_configured == 0U) {
    return 0U;
  }
  /* Rounded down on purpose: the bound that constrains feeding must never be
   * flattered. */
  return timeout_ms_at(qwdt_rate_fastest(), 0U);
}
/*---------------------------------------------------------------------------*/
unsigned int
qwdt_timeout_max_ms(void)
{
  if(qwdt_configured == 0U) {
    return 0U;
  }
  /* Rounded up: compared against the process safety time, so pessimism is the
   * right direction here. */
  return timeout_ms_at(qwdt_rate_slowest(), 1U);
}
/*---------------------------------------------------------------------------*/
unsigned long
qwdt_prescaler(void)
{
  if(qwdt_configured == 0U) {
    return 0UL;
  }
  return qwdt_divs[qwdt_div_idx].div;
}
/*---------------------------------------------------------------------------*/
unsigned int
qwdt_reload(void)
{
  if(qwdt_configured == 0U) {
    return 0U;
  }
  return (unsigned int)(qwdt_count - 1U);
}
/*---------------------------------------------------------------------------*/
kern_status_t
qwdt_calibrate(unsigned long measured_hz)
{
  if(measured_hz == 0UL) {
    qwdt_measured_hz = 0UL;
    return KERN_OK;
  }

  if((measured_hz < (unsigned long)QWDT_CONF_IRC40K_MIN_HZ) ||
     (measured_hz > (unsigned long)QWDT_CONF_IRC40K_MAX_HZ)) {
    kern_raise(KERN_ERR_RANGE, KERN_MOD_WDT, (unsigned int)__LINE__);
    return KERN_ERR_RANGE;
  }

  qwdt_measured_hz = measured_hz;

  /* Re-select only while the counter is not yet armed: qwdt_start() has no way
   * to tell whether the peripheral is running, so the ordering is the caller's
   * responsibility and is documented in dev/qwdt.h. */
  if(qwdt_configured != 0U) {
    if(qwdt_select((uint32_t)measured_hz) != KERN_OK) {
      qwdt_measured_hz = 0UL;
      kern_raise(KERN_ERR_RANGE, KERN_MOD_WDT, (unsigned int)__LINE__);
      return KERN_ERR_RANGE;
    }
  }

  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
unsigned long
qwdt_measured_irc40k_hz(void)
{
  return qwdt_measured_hz;
}
/*---------------------------------------------------------------------------*/
static char *
qwdt_put_str(char *dst, const char *src, char *limit)
{
  while((*src != '\0') && (dst < limit)) {
    *dst++ = *src++;
  }
  return dst;
}
/*---------------------------------------------------------------------------*/
static char *
qwdt_put_dec(char *dst, unsigned long v, char *limit)
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
qwdt_describe(char *dst, unsigned int len)
{
  char *p = dst;
  char *limit;

  if((dst == NULL) || (len < 2U) || (qwdt_configured == 0U)) {
    return 0U;
  }
  limit = dst + (len - 1U);   /* reserve the terminator */

  p = qwdt_put_str(p, "wdt: IRC40K ", limit);
  if(qwdt_measured_hz == 0UL) {
    p = qwdt_put_str(p, "nom=", limit);
    p = qwdt_put_dec(p, (unsigned long)QWDT_CONF_IRC40K_NOMINAL_HZ, limit);
    p = qwdt_put_str(p, "Hz (assumed)", limit);
  } else {
    p = qwdt_put_str(p, "measured=", limit);
    p = qwdt_put_dec(p, qwdt_measured_hz, limit);
    p = qwdt_put_str(p, "Hz +/-ppt=", limit);
    p = qwdt_put_dec(p, (unsigned long)QWDT_CONF_MEASURED_SPREAD_PPT, limit);
  }
  p = qwdt_put_str(p, " div=", limit);
  p = qwdt_put_dec(p, qwdt_divs[qwdt_div_idx].div, limit);
  p = qwdt_put_str(p, " reload=", limit);
  p = qwdt_put_dec(p, (unsigned long)(qwdt_count - 1U), limit);
  p = qwdt_put_str(p, " timeout=", limit);
  p = qwdt_put_dec(p, (unsigned long)qwdt_timeout_nominal_ms(), limit);
  p = qwdt_put_str(p, "ms min=", limit);
  p = qwdt_put_dec(p, (unsigned long)qwdt_timeout_min_ms(), limit);
  p = qwdt_put_str(p, "ms max=", limit);
  p = qwdt_put_dec(p, (unsigned long)qwdt_timeout_max_ms(), limit);
  p = qwdt_put_str(p, "ms safe=", limit);

  if(QWDT_CONF_SAFE_TIME_MS == 0U) {
    p = qwdt_put_str(p, "NOT STATED", limit);
  } else {
    p = qwdt_put_dec(p, (unsigned long)QWDT_CONF_SAFE_TIME_MS, limit);
    p = qwdt_put_str(p, "ms ", limit);
    if(qwdt_timeout_max_ms() > (unsigned int)QWDT_CONF_SAFE_TIME_MS) {
      p = qwdt_put_str(p, "VIOLATION", limit);
    } else {
      p = qwdt_put_str(p, "ok", limit);
    }
  }

  *p = '\0';
  return (unsigned int)(p - dst);
}
/*---------------------------------------------------------------------------*/

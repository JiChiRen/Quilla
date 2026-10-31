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
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * This file is part of the Quilla operating system.
 */

/**
 * \file
 * PY32F040xB independent watchdog: timeout selection and its justification.
 *
 * The watchdog is the last line of defence, and it is only worth anything if
 * its timeout is derived from the process safety time rather than from
 * whatever the peripheral happens to offer. Two bounds matter and they pull
 * in opposite directions:
 *
 *   - too long, and a dangerous failure is tolerated longer than the system
 *     it guards may take to reach the safe state;
 *   - too short, and the timer trips during normal operation, turning a
 *     watchdog into a reset generator.
 *
 * The IWDG is clocked from the LSI, which is an RC oscillator. It is nowhere
 * near accurate enough to treat its nominal value as the real one, so this
 * module keeps the dispersion explicit: it picks the divider and reload that
 * best approximate the requested timeout at the nominal rate, then reports
 * the shortest and longest timeout implied by the *worst-case* ends of the
 * LSI range. Callers use the shortest bound to constrain how often they may
 * feed, and the longest bound to compare against the process safety time.
 *
 * CALIBRATION OBLIGATION
 * ----------------------
 * QWDT_CONF_LSI_NOMINAL_HZ / MIN / MAX below are conservative engineering
 * assumptions, not measurements of this board. Before any of these numbers
 * enter a safety case they must be replaced with either (a) the values given
 * in the PY32F040xB datasheet for the exact part number and temperature range,
 * or (b) values measured on this hardware.
 *
 * Route (b) is now available: dev/qclk.h measures the LSI against the system
 * clock, and qwdt_calibrate() feeds that number in here. After calibration
 * this module stops using the assumed nominal rate and derives both bounds
 * from the measured one, widened by QWDT_CONF_MEASURED_SPREAD_PPT to cover
 * drift the single measurement cannot see. That spread is itself an
 * assumption and is reported as such; the measurement removes the error in
 * the centre of the range, not the uncertainty around it.
 */

#ifndef QWDT_H_
#define QWDT_H_

#include "sys/kern-status.h"

#include <stdint.h>

/**
 * \brief Target watchdog timeout, milliseconds.
 *
 * This is the requested *nominal* value. The real interval lands anywhere
 * inside [qwdt_timeout_min_ms(), qwdt_timeout_max_ms()]. Set it from the
 * process safety time, not from convenience.
 */
#ifndef QWDT_CONF_TIMEOUT_MS
#define QWDT_CONF_TIMEOUT_MS            500U
#endif

/** Nominal LSI rate used to choose the divider and reload. */
#ifndef QWDT_CONF_LSI_NOMINAL_HZ
#define QWDT_CONF_LSI_NOMINAL_HZ        32000U
#endif

/**
 * Slowest LSI rate admitted. At this rate the watchdog takes longest to
 * expire, so this bound decides whether the timeout fits the safety time.
 */
#ifndef QWDT_CONF_LSI_MIN_HZ
#define QWDT_CONF_LSI_MIN_HZ            24000U
#endif

/**
 * Fastest LSI rate admitted. At this rate the watchdog expires soonest, so
 * this bound decides how often the software must feed it.
 */
#ifndef QWDT_CONF_LSI_MAX_HZ
#define QWDT_CONF_LSI_MAX_HZ            60000U
#endif

/**
 * \brief Process safety time of this application, milliseconds.
 *
 * 0 means "not stated yet", which disables the check. Until a real number is
 * filled in, the watchdog timeout has no defensible relationship to the
 * hazard and must not be claimed as a safety measure.
 */
#ifndef QWDT_CONF_SAFE_TIME_MS
#define QWDT_CONF_SAFE_TIME_MS          0U
#endif

/**
 * \brief Drift admitted around a measured LSI rate, parts per thousand.
 *
 * Used only once qwdt_calibrate() has supplied a measurement. It has to
 * cover how far the oscillator moves over the product's temperature range
 * and life - not the accuracy of the measurement, which is far finer.
 * Without a datasheet figure for this part it stays an assumption and is
 * printed as one by qwdt_describe().
 */
#ifndef QWDT_CONF_MEASURED_SPREAD_PPT
#define QWDT_CONF_MEASURED_SPREAD_PPT   100U
#endif

#if QWDT_CONF_MEASURED_SPREAD_PPT >= 1000U
#error "QWDT_CONF_MEASURED_SPREAD_PPT must stay below 1000"
#endif

/**
 * \brief  Compute the divider / reload pair for the configured timeout
 * \retval KERN_OK         configuration computed
 * \retval KERN_ERR_RANGE  the timeout cannot be expressed with the available
 *                         prescalers and reload width, or it does not fit
 *                         inside the configured process safety time
 *
 * Does not touch any hardware. Two-phase for the same reason the rest of the
 * platform splits init and start: a fault during early boot must not already
 * be covered by a timer that was never verified.
 */
kern_status_t qwdt_init(void);

/** \brief Arm the watchdog. Idempotent. */
kern_status_t qwdt_start(void);

/** \brief Reload the counter. Return KERN_ERR_ALREADY if it is not armed. */
kern_status_t qwdt_refresh(void);

/** \brief Force an immediate watchdog reset. Never returns. */
void qwdt_reboot(void);

/** \brief 1 once the watchdog is armed. */
unsigned int qwdt_is_started(void);

/** \brief Timeout at the nominal LSI rate, milliseconds. */
unsigned int qwdt_timeout_nominal_ms(void);

/** \brief Shortest timeout, at the fastest admitted LSI rate. Bound on feeding. */
unsigned int qwdt_timeout_min_ms(void);

/** \brief Longest timeout, at the slowest admitted LSI rate. Compared with
 *         the process safety time. */
unsigned int qwdt_timeout_max_ms(void);

/** \brief Prescaler actually selected (the HAL macro value). */
unsigned long qwdt_prescaler(void);

/** \brief Reload value actually selected (0x000..0xFFF). */
unsigned int qwdt_reload(void);

/**
 * \brief          Supply a measured LSI rate for this board, Hz
 * \param measured_hz Rate measured on this hardware, or 0 to go back to
 *                    the compile-time assumption
 * \retval KERN_OK         accepted
 * \retval KERN_ERR_RANGE  outside [QWDT_CONF_LSI_MIN_HZ, MAX_HZ], or no
 *                         prescaler/reload pair expresses the timeout
 *                         at that rate
 *
 * Call this before qwdt_init() so the divider and reload are chosen for
 * the oscillator that is actually fitted. Calling it afterwards
 * re-selects the pair, which only means something while the counter is
 * not yet armed: once the IWDG runs, its reload must not be rewritten.
 */
kern_status_t qwdt_calibrate(unsigned long measured_hz);

/** \brief Measured rate in force, Hz. 0 while uncalibrated. */
unsigned long qwdt_measured_lsi_hz(void);

/**
 * \brief          Describe the resolved configuration into a caller's buffer
 * \param dst      Destination buffer, NUL terminated on success
 * \param len      Capacity of dst in bytes
 * \return         Number of bytes written excluding the terminator, 0 on
 *                 truncation or if the module is not initialised
 *
 * Provided so that the application, not the driver, decides where the line
 * goes. Keeping printf-style formatting out of the driver avoids pulling the
 * shell or the C library into every image that uses the watchdog.
 */
unsigned int qwdt_describe(char *dst, unsigned int len);

#endif /* QWDT_H_ */

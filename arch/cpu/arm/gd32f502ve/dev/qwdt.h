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
 * GD32F502VE independent watchdog (FWDGT) configuration.
 *
 * WHAT THIS DEVICE GIVES US
 * -------------------------
 * FWDGT is a 12-bit down-counter clocked from IRC40K, with a prescaler field
 * this port restricts to DIV4..DIV256 - see dev/qwdt.c for why the wider
 * divisors the vendor header lists are not used. Once started it cannot be
 * stopped by anything short of a reset, which is the property that makes it
 * the last line of defence rather than a nuisance: a stray write, a wedged
 * interrupt, and a runaway process all arrive at the same outcome.
 *
 * WHAT REMAINS AN ASSUMPTION
 * --------------------------
 * The clock. IRC40K is documented as "about 40 kHz" and nothing this project
 * has access to characterises it for this part number and temperature range,
 * so every timeout here is a band, not a number:
 *
 *   [qwdt_timeout_min_ms(), qwdt_timeout_max_ms()]
 *
 * The slowest admitted rate decides the long end, which is what gets compared
 * against the process safety time; the fastest decides the short end, which is
 * what constrains how often software has to feed. Until a real process safety
 * time exists, QWDT_CONF_SAFE_TIME_MS is 0 and qwdt_describe() prints
 * "safe=NOT STATED": without that number the timeout has no defensible
 * relationship to the hazard and must not be presented as a safety measure.
 *
 * Two routes narrow the band:
 *   (a) values from a datasheet for exactly this part number and temperature
 *       range - not available to this project;
 *   (b) values measured on this hardware.
 *
 * Route (b) is available: dev/qclk.h measures IRC40K against the rtimer time
 * base and qwdt_calibrate() feeds that in. After calibration this module stops
 * using the assumed rate and derives both bounds from the measured one,
 * widened by QWDT_CONF_MEASURED_SPREAD_PPT to cover drift one measurement
 * cannot see. That spread is itself an assumption and is printed as such; the
 * measurement removes the error in the centre of the range, not the
 * uncertainty around it.
 */

#ifndef QWDT_H_
#define QWDT_H_

#include "sys/kern-status.h"

#include <stdint.h>

/**
 * \brief Target watchdog timeout, milliseconds.
 *
 * Requested *nominal* value. The real interval lands anywhere inside
 * [qwdt_timeout_min_ms(), qwdt_timeout_max_ms()]. Set it from the process
 * safety time, not from convenience.
 */
#ifndef QWDT_CONF_TIMEOUT_MS
#define QWDT_CONF_TIMEOUT_MS            500U
#endif

/** Nominal IRC40K rate used to choose the divider and reload. */
#ifndef QWDT_CONF_IRC40K_NOMINAL_HZ
#define QWDT_CONF_IRC40K_NOMINAL_HZ     40000U
#endif

/**
 * Slowest IRC40K rate admitted. At this rate the watchdog takes longest to
 * expire, so this bound decides whether the timeout fits the safety time.
 */
#ifndef QWDT_CONF_IRC40K_MIN_HZ
#define QWDT_CONF_IRC40K_MIN_HZ         20000U
#endif

/**
 * Fastest IRC40K rate admitted. At this rate the watchdog expires soonest, so
 * this bound decides how often the software must feed it.
 */
#ifndef QWDT_CONF_IRC40K_MAX_HZ
#define QWDT_CONF_IRC40K_MAX_HZ         80000U
#endif

/**
 * \brief Process safety time of this application, milliseconds.
 *
 * 0 means "not stated yet", which disables the check. See the file comment.
 */
#ifndef QWDT_CONF_SAFE_TIME_MS
#define QWDT_CONF_SAFE_TIME_MS          0U
#endif

/**
 * \brief Drift admitted around a measured IRC40K rate, parts per thousand.
 *
 * Used only once qwdt_calibrate() has supplied a measurement, and it has to
 * cover how far this oscillator moves over the product's temperature range and
 * life - not the accuracy of the measurement, which is far finer. Without a
 * datasheet figure for this part it stays an assumption and is printed as one
 * by qwdt_describe().
 */
#ifndef QWDT_CONF_MEASURED_SPREAD_PPT
#define QWDT_CONF_MEASURED_SPREAD_PPT   100U
#endif

#if QWDT_CONF_MEASURED_SPREAD_PPT >= 1000U
#error "QWDT_CONF_MEASURED_SPREAD_PPT must stay below 1000"
#endif

#if QWDT_CONF_IRC40K_MIN_HZ >= QWDT_CONF_IRC40K_MAX_HZ
#error "QWDT_CONF_IRC40K_MIN_HZ must be below QWDT_CONF_IRC40K_MAX_HZ"
#endif

/**
 * \brief          Compute the divider / reload pair for the configured timeout
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

/** \brief Reload the counter. KERN_ERR_ALREADY if it is not armed. */
kern_status_t qwdt_refresh(void);

/** \brief Force an immediate watchdog reset. Never returns. */
void qwdt_reboot(void);

/** \brief 1 once the watchdog is armed. */
unsigned int qwdt_is_started(void);

/** \brief Timeout at the nominal IRC40K rate, milliseconds. */
unsigned int qwdt_timeout_nominal_ms(void);

/** \brief Shortest timeout, at the fastest admitted rate. Bound on feeding. */
unsigned int qwdt_timeout_min_ms(void);

/** \brief Longest timeout, at the slowest admitted rate. Compared with the
 *         process safety time. */
unsigned int qwdt_timeout_max_ms(void);

/** \brief Divider actually selected, as a divisor (4, 8, 16, ...). */
unsigned long qwdt_prescaler(void);

/** \brief Reload value actually selected (0x000..0xFFF). */
unsigned int qwdt_reload(void);

/**
 * \brief          Supply a measured IRC40K rate for this board, Hz
 * \param measured_hz Rate measured on this hardware, or 0 to go back to the
 *                    compile-time assumption
 * \retval KERN_OK         accepted
 * \retval KERN_ERR_RANGE  outside [QWDT_CONF_IRC40K_MIN_HZ, MAX_HZ], or no
 *                         divider/reload pair expresses the timeout at that
 *                         rate
 *
 * Call before qwdt_init() so the pair is chosen for the oscillator that is
 * actually fitted. Calling it afterwards re-selects the pair, which only means
 * something while the counter is not yet armed: once FWDGT runs its reload
 * must not be rewritten.
 */
kern_status_t qwdt_calibrate(unsigned long measured_hz);

/** \brief Measured rate in force, Hz. 0 while uncalibrated. */
unsigned long qwdt_measured_irc40k_hz(void);

/**
 * \brief          Describe the resolved configuration into a caller's buffer
 * \param dst      Destination buffer, NUL terminated on success
 * \param len      Capacity of dst in bytes
 * \return         Bytes written excluding the terminator, 0 if not configured
 *
 * Provided so that the application, not the driver, decides where the line
 * goes. Keeping printf-style formatting out of the driver avoids pulling a
 * formatter into every image that uses the watchdog.
 */
unsigned int qwdt_describe(char *dst, unsigned int len);

#endif /* QWDT_H_ */

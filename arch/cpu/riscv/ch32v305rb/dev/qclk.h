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
 * Clock cross-checking for the GD32F502VE: measure IRC40K against the rtimer
 * time base.
 *
 * WHY THIS EXISTS
 * ---------------
 * The free watchdog timer is clocked from IRC40K, an RC oscillator this
 * project has no characterisation data for. dev/qwdt.h therefore carries its
 * rate as an explicit engineering assumption and says so, and that assumption
 * is the widest single input to the watchdog's timeout bounds.
 *
 * This module replaces the assumption with a measurement, which is what makes
 * the difference between "the watchdog expires somewhere between X and Y" and
 * "the watchdog expires between x and y, and here is the measured centre".
 *
 * WHAT IS COMPARED
 * ----------------
 *   side A - the rtimer counter: TIMER0 on APB2, already running at
 *            RTIMER_ARCH_SECOND (1 MHz) by the time this module is used.
 *   side B - IRC40K, counted by the RTC's 20-bit divider with the RTC clock
 *            entry switched to RCU_RTCSRC_IRC40K.
 *
 * Those two clock trees share nothing downstream of their oscillators, so what
 * is observable is their RATIO. That covers the failure modes that matter -
 * one oscillator drifting, stopping, or being switched to another source -
 * because each of them changes the ratio.
 *
 * This port also has a clock monitor the PY32F040xB does not: RCU_CTL_HCKMEN
 * raises NMI when the HXTAL stops. Enabling it is a separate decision, made in
 * platform code, because NMI is not something a library gets to claim. Nothing
 * here either requires it or replaces it.
 *
 * WHAT IT CANNOT DETECT
 * ---------------------
 * Absolute accuracy. Neither side is trimmed against a reference, so a drift
 * that moved both by the same factor is invisible. Readings from here are not
 * a calibration certificate.
 *
 * Total loss of the system clock is likewise not observable by software: if
 * that clock stops, so does the CPU. The HXTAL monitor above is what covers
 * the common cause of it.
 *
 * HOW IT MEASURES
 * ---------------
 * A window is defined by counting N IRC40K edges, and the number of rtimer
 * ticks seen inside it is compared against a band. Because the window is fixed
 * on the independent side:
 *
 *   - a stopped, slow or mis-divided TIMER0 shows up as too few ticks, and
 *     the window still closes - the check fails loudly instead of hanging;
 *   - a stopped IRC40K would hold the window open forever, so the window is
 *     additionally closed once the tick count passes
 *     QCLK_CONF_MAX_TICK_STRETCH_X times its nominal value, which lands it
 *     outside the band by construction.
 *
 * TERMINATION
 * -----------
 * Every loop here is bounded. qclk_calibrate() takes one window and has no
 * iterative timeout, but both exits above bound it: the IRC40K side cannot
 * take longer than WINDOW / QCLK_CONF_IRC40K_MIN_HZ and the tick side cannot
 * take longer than MAX_TICK_STRETCH_X * expected ticks / RTIMER_ARCH_SECOND.
 * Past either bound the loop returns rather than spinning.
 */

#ifndef QCLK_H_
#define QCLK_H_

#include "rtimer-arch.h"
#include "sys/kern-status.h"

#include <stdint.h>

/**
 * \brief Reload value written to the RTC prescaler.
 *
 * The RTC divider counts down from this value once per RTC clock, so its
 * natural modulus is QCLK_CONF_RTC_PSC + 1. 39999 makes that modulus 40000,
 * i.e. one second per counter revolution when IRC40K runs at nominal.
 *
 * One wrap inside a window must be impossible: see QCLK_CONF_WINDOW_IRC40K,
 * which must stay well below this.
 */
#ifndef QCLK_CONF_RTC_PSC
#define QCLK_CONF_RTC_PSC               39999UL
#endif

/**
 * \brief Length of one measurement window, in IRC40K counts.
 *
 * 2000 counts is 50 ms at the nominal rate and 100 ms at the slowest admitted
 * one, which keeps the blocking calibration inside the usual bring-up budget
 * and well clear of the counter modulus.
 */
#ifndef QCLK_CONF_WINDOW_IRC40K
#define QCLK_CONF_WINDOW_IRC40K         2000UL
#endif

/**
 * \brief Rate IRC40K is assumed to run at before a measurement succeeds, Hz.
 *
 * Used only to sanity-check the first window and to build the comparison band
 * while no measurement exists.
 */
#ifndef QCLK_CONF_IRC40K_NOMINAL_HZ
#define QCLK_CONF_IRC40K_NOMINAL_HZ     40000UL
#endif

/** Slowest IRC40K rate accepted, Hz. Outside means fault. */
#ifndef QCLK_CONF_IRC40K_MIN_HZ
#define QCLK_CONF_IRC40K_MIN_HZ         20000UL
#endif

/** Fastest IRC40K rate accepted, Hz. Outside means fault. */
#ifndef QCLK_CONF_IRC40K_MAX_HZ
#define QCLK_CONF_IRC40K_MAX_HZ         80000UL
#endif

/**
 * \brief Half-width of the running band once a measurement exists, ppt.
 *
 * Applied to the measured tick count in both directions. It has to cover how
 * far this oscillator moves over the product's temperature range and life,
 * not the accuracy of the measurement, which is two orders of magnitude
 * finer. It is an assumption and is reported as one.
 */
#ifndef QCLK_CONF_TOLERANCE_PPT
#define QCLK_CONF_TOLERANCE_PPT         50UL
#endif

/**
 * \brief Half-width of the band used while no measurement exists, ppt.
 *
 * Deliberately much wider than QCLK_CONF_TOLERANCE_PPT: an assumed oscillator
 * rate is not a tight claim, and pretending otherwise would only manufacture
 * start-up violations.
 */
#ifndef QCLK_CONF_ASSUMED_TOLERANCE_PPT
#define QCLK_CONF_ASSUMED_TOLERANCE_PPT 300UL
#endif

/**
 * \brief Bound on how far a window may stretch, as a multiple of the expected
 *        tick count.
 *
 * This is what catches an IRC40K that has stopped advancing: without it the
 * window would never close and the periodic check would go permanently quiet.
 */
#ifndef QCLK_CONF_MAX_TICK_STRETCH_X
#define QCLK_CONF_MAX_TICK_STRETCH_X    3UL
#endif

/**
 * \brief Reads allowed for one coherent sample of the RTC divider.
 *
 * The RTC and its divider live in the backup domain and are read across a
 * clock boundary, so a single read can straddle a divider update. Two equal
 * consecutive reads are accepted; after this many attempts the last value
 * wins rather than looping.
 */
#ifndef QCLK_CONF_READ_RETRIES
#define QCLK_CONF_READ_RETRIES          4U
#endif

/** Clock cross-check is always enabled; the "clk" shell command was removed.
 *  QCLK_CONF_SHELL is retained for compatibility. */
#ifndef QCLK_CONF_SHELL
#define QCLK_CONF_SHELL                 1
#endif

#if QCLK_CONF_WINDOW_IRC40K >= (QCLK_CONF_RTC_PSC + 1UL)
#error "QCLK_CONF_WINDOW_IRC40K must stay below the RTC divider modulus"
#endif

#if QCLK_CONF_TOLERANCE_PPT >= 1000UL
#error "QCLK_CONF_TOLERANCE_PPT must stay below 1000 (parts per thousand)"
#endif

#if QCLK_CONF_ASSUMED_TOLERANCE_PPT >= 1000UL
#error "QCLK_CONF_ASSUMED_TOLERANCE_PPT must stay below 1000"
#endif

#if QCLK_CONF_IRC40K_MIN_HZ >= QCLK_CONF_IRC40K_MAX_HZ
#error "QCLK_CONF_IRC40K_MIN_HZ must be below QCLK_CONF_IRC40K_MAX_HZ"
#endif

/**
 * \brief          Switch the RTC to IRC40K and get the divider counting
 * \retval KERN_OK         reference running, ready to measure
 * \retval KERN_ERR_RANGE  the configuration does not fit together
 * \retval KERN_ERR_INTERNAL IRC40K did not become stable
 *
 * Does not start the clock under test: the rtimer owns TIMER0 and is already
 * running by the time anything calls this. This module only reads it, so
 * nothing about rtimer scheduling changes.
 */
kern_status_t qclk_init(void);

/**
 * \brief          Measure the IRC40K rate once, blocking
 * \retval KERN_OK         the measured rate is available
 * \retval KERN_ERR_NOT_READY qclk_init() has not run
 * \retval KERN_ERR_CLOCK  the window closed with an unusable result
 * \retval KERN_ERR_RANGE  the implied rate fell outside
 *                         [QCLK_CONF_IRC40K_MIN_HZ, QCLK_CONF_IRC40K_MAX_HZ]
 *
 * Takes one window (about 50 ms at the defaults). Call it before qwdt_init()
 * so the watchdog is dimensioned against the oscillator this board has.
 */
kern_status_t qclk_calibrate(void);

/**
 * \brief          Compare the two sides against the current band
 * \retval KERN_OK         window not finished yet, or finished inside the band
 * \retval KERN_ERR_CLOCK  a completed window fell outside the band
 * \retval KERN_ERR_NOT_READY qclk_init() has not run
 *
 * Intended for the idle loop. It opens a window on the first call and then
 * does nothing until that window closes, at which point exactly one comparison
 * is made. A violation is raised through kern_raise() with KERN_MOD_CLOCK
 * before this returns.
 */
kern_status_t qclk_poll(void);

/** \brief Measured IRC40K rate, Hz. 0 until a window completes. */
unsigned long qclk_measured_irc40k_hz(void);

/** \brief Rate of the tick counter used as the other side, Hz. 0 if idle. */
unsigned long qclk_reference_hz(void);

/** \brief Number of completed windows since start-up. */
unsigned int qclk_window_count(void);

/** \brief Number of windows that fell outside the band. */
unsigned int qclk_violation_count(void);

/** \brief Rate implied by the most recent window, Hz, band or not. */
unsigned long qclk_last_window_hz(void);

/** \brief rtimer ticks counted in the most recent window. */
unsigned long qclk_last_window_ticks(void);

/**
 * \brief          Describe the current state into a caller's buffer
 * \param dst      Destination buffer, NUL terminated on success
 * \param len      Capacity of dst in bytes
 * \return         Bytes written excluding the terminator, 0 if not ready
 *
 * Kept free of printf so that no image is forced to link a formatter.
 */
unsigned int qclk_describe(char *dst, unsigned int len);


#endif /* QCLK_H_ */

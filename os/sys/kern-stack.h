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
 * ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * INSTITUTE OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * This file is part of the Quilla operating system.
 */

/**
 * \file
 * Stack watermark (high-water mark) monitoring without an MPU.
 *
 * This port runs every process and every interrupt handler on the single
 * main stack, so one contiguous region has to be guarded. The scheme is
 * purely software and needs no hardware memory protection:
 *
 *   [ bottom of the stack reservation )
 *     canary words      never touched by the fill, checked verbatim
 *     watermark fill    written with KERN_STACK_FILL at start-up
 *     live stack        untouched by this module
 *   [ initial SP )
 *
 * A lost canary means the stack has reached the static data region and the
 * fault is reported through kern_raise(). The fill is then scanned from the
 * previous low-water mark downwards, so a periodic check costs O(bytes
 * newly consumed) rather than O(stack size) and can run in the idle loop.
 *
 * The region boundaries come from the linker. Both may be overridden from
 * the platform configuration when a toolchain uses different symbol names:
 *   KERN_CONF_STACK_BOTTOM   lowest byte of the stack reservation
 *   KERN_CONF_STACK_TOP      one byte past the highest byte the stack may use
 */

#ifndef KERN_STACK_H_
#define KERN_STACK_H_

#include "sys/kern-status.h"

#include <stdint.h>

/** Pattern written into unused stack space by kern_stack_init(). */
#define KERN_STACK_FILL              0xCDCDCDCDU

/** Pattern written into the canary words below the watermark area. */
#define KERN_STACK_CANARY            0xA5A5A5A5U

/** Canary size in words. Two words survive a single stray word store. */
#ifndef KERN_CONF_STACK_CANARY_WORDS
#define KERN_CONF_STACK_CANARY_WORDS 2U
#endif

/** Expose the "stack" shell command. Requires the Quilla shell to be linked. */
#ifndef KERN_CONF_STACK_SHELL
#define KERN_CONF_STACK_SHELL        0
#endif

/**
 * \brief          Paint the stack reservation and arm the monitor
 * \retval KERN_OK monitor armed
 * \retval KERN_ERR_RANGE the linker symbols describe an unusable region
 *
 * Must be called as early as possible after reset, before the watchdog is
 * started: painting a few kilobytes is not instantaneous. It only writes
 * below the current stack pointer, so live frames are never touched.
 */
kern_status_t kern_stack_init(void);

/**
 * \brief          Check the canary and refresh the low-water mark
 * \retval KERN_OK        stack intact
 * \retval KERN_ERR_STACK canary lost: the stack overran into static RAM
 * \retval KERN_ERR_NOT_READY kern_stack_init() has not run yet
 *
 * Raises KERN_ERR_STACK / KERN_MOD_STACK through kern_raise() before
 * returning, so the project error hook reaches its defined safe state.
 * Cheap enough for the idle loop: only newly consumed bytes are scanned.
 */
kern_status_t kern_stack_check(void);

/**
 * \brief Total size of the monitored stack reservation, in bytes.
 */
unsigned int kern_stack_size_bytes(void);

/**
 * \brief Peak stack usage since start-up, in bytes (high-water mark).
 */
unsigned int kern_stack_used_bytes(void);

/**
 * \brief Bytes of the reservation that were never touched, i.e. the margin.
 */
unsigned int kern_stack_free_bytes(void);

/**
 * \brief Number of canary failures recorded since start-up.
 */
unsigned int kern_stack_overflow_count(void);

#if KERN_CONF_STACK_SHELL
/**
 * \brief Register the "stack" command on the Quilla shell
 *
 * Reports size, peak usage, margin and canary state. Run once, after the
 * shell has been brought up.
 */
void kern_stack_shell_init(void);

/**
 * \brief Print the high-water mark reached so far, tagged with the call site
 *
 * For one-way boot paths (the secure-boot gate before the jump to the APP,
 * the end of an QIAP upgrade before the reset): the peak has to be emitted
 * before control leaves, otherwise the measurement is lost. The figure
 * includes this function's own frame, so it is a pessimistic upper bound.
 */
void kern_stack_report(const char *tag);
#endif

#endif /* KERN_STACK_H_ */

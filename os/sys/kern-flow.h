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
 * Program-flow sequence monitoring (IEC 61508-3 Annex A / Table A.9).
 *
 * A heartbeat only proves that *some* code is still running. It does not
 * prove that the code is running in the right order, at the right rate, or
 * that an inner loop has not silently stopped being reached. The classic
 * failure is a branch that bypasses one safety check and still reaches the
 * watchdog refresh: the watchdog stays fed and the unit keeps acting
 * although the guard that made it safe is gone.
 *
 * This module detects that class of failure. Each monitored flow declares
 * which checkpoints must be visited, in which order, and within how many
 * clock ticks. A checkpoint that is skipped, repeated or reached out of
 * order raises KERN_ERR_SEQUENCE; so does a flow that takes longer than its
 * budget to reach them all. Once all checkpoints of a flow are visited in
 * order the cycle closes and starts again, so a periodic loop needs no
 * explicit begin/end calls.
 *
 * Hardening notes:
 *   - the arrival set is stored twice, once complemented. A single bit flip
 *     in RAM - which is exactly the kind of corruption this class of device
 *     can suffer - is caught on the next kern_flow_check() rather than being
 *     mistaken for progress;
 *   - the budget is expressed in clock ticks and compared with an unsigned
 *     difference, so it is correct across a clock wrap.
 *
 * What it deliberately does NOT cover: checkpoints only prove that the
 * instrumented path ran. They say nothing about whether the values computed
 * on it are right, and nothing about interrupt handlers that were never
 * given a checkpoint. Both remain separate measures.
 */

#ifndef KERN_FLOW_H_
#define KERN_FLOW_H_

#include "sys/kern-status.h"
#include "sys/clock.h"

#include <stdint.h>

typedef uint16_t kern_flow_token_t;

/** \brief Token for checkpoint n. n must be in 0..15. */
#define KERN_FLOW_TOKEN(n)          ((kern_flow_token_t)(1U << (n)))

/** \brief Mask requiring every checkpoint 0..n-1 in order. */
#define KERN_FLOW_MASK(n)           ((kern_flow_token_t)((1U << (n)) - 1U))

/** Number of simultaneously monitored flows. Storage is static. */
#ifndef KERN_CONF_FLOW_MAX_TRACKS
#define KERN_CONF_FLOW_MAX_TRACKS   4U
#endif

/** Flow monitoring is always enabled; the "flow" shell diagnostic command was
 *  removed, and its KERN_CONF_FLOW_SHELL / KERN_FLOW_SHELL_ENABLED switches
 *  are gone with it. */

/* Resolved names used inside the module. They must NOT be spelled like the
 * KERN_CONF_* forms they are derived from: a project that defines
 * KERN_CONF_FLOW_MAX_TRACKS in contiki-conf.h makes the #ifndef above fail,
 * and if the implementation then used that same spelling nothing would guard
 * the array bound at all. Aliases make the trap impossible, and they keep
 * their own #ifndef so a -D override still works. */
#ifndef KERN_FLOW_MAX_TRACKS
#define KERN_FLOW_MAX_TRACKS        KERN_CONF_FLOW_MAX_TRACKS
#endif

/**
 * \brief          Register a flow to be monitored
 * \param name     Debug label, kept by pointer. Must outlive the monitor.
 * \param required Mask of checkpoints that must be visited in every cycle.
 *                 Checkpoint k carries token KERN_FLOW_TOKEN(k), so the
 *                 usual value is KERN_FLOW_MASK(n) for n checkpoints.
 * \param budget   Longest admissible time, in clock ticks, from the start of
 *                 a cycle to the visit of its last checkpoint. Zero disables
 *                 the timing check for this flow and keeps order only.
 * \return         Non-negative flow id, or -1 when no slot is free or an
 *                 argument is invalid (KERN_ERR_FULL / KERN_ERR_RANGE is
 *                 also raised).
 *
 * The first cycle is considered to start when this function is called.
 */
int kern_flow_register(const char *name,
                       kern_flow_token_t required,
                       clock_time_t budget);

/**
 * \brief          Record that a checkpoint has been reached
 * \param id       Flow identifier returned by kern_flow_register().
 * \param token    Exactly one bit, and a member of the required mask.
 * \retval KERN_OK              checkpoint accepted
 * \retval KERN_ERR_SEQUENCE    checkpoints skipped, repeated or out of order;
 *                              the cycle is restarted so a single disturbance
 *                              does not stall the flow forever
 * \retval KERN_ERR_RANGE       id or token invalid
 * \retval KERN_ERR_NOT_READY   the id does not name a registered flow
 */
kern_status_t kern_flow_checkpoint(int id, kern_flow_token_t token);

/**
 * \brief          Restart a cycle without scoring a violation.
 *
 * Intended for paths that legitimately do not reach the end of the sequence,
 * for example an upgrade that is abandoned by the host.
 */
kern_status_t kern_flow_reset(int id);

/**
 * \brief          Restart the cycle of every registered flow.
 *
 * For a suspension the sequence monitoring is not meant to see: the core
 * is about to be stopped, and the software clock will jump by the whole
 * sleep when it wakes. Both cycles are restarted so the jump is not scored
 * as a budget violation. Unlike a violation this is not counted, because
 * the flow did nothing wrong - it was not running.
 */
void kern_flow_reset_all(void);

/**
 * \brief          Check every registered flow
 * \retval KERN_OK            all flows consistent and inside budget
 * \retval KERN_ERR_SEQUENCE  at least one flow is stalled or its arrival set
 *                            failed the complement check
 *
 * Cheap enough to call from the idle loop. Feed the watchdog only when this
 * returns KERN_OK, otherwise a stalled loop keeps the dog alive by itself.
 */
kern_status_t kern_flow_check(void);

/** \brief Number of flows currently registered. */
unsigned int kern_flow_track_count(void);

/** \brief Sum of violations found since start-up, over all flows. */
unsigned int kern_flow_violation_total(void);

/** \brief Cycle complete count of one flow, or 0 for an invalid id. */
unsigned int kern_flow_cycles(int id);

/** \brief Violation count of one flow, or 0 for an invalid id. */
unsigned int kern_flow_violations(int id);

/** \brief Longest completed cycle of one flow, in clock ticks. */
clock_time_t kern_flow_worst_cycle(int id);

/** \brief Registered label of one flow, or NULL for an invalid id. */
const char *kern_flow_name(int id);


#endif /* KERN_FLOW_H_ */

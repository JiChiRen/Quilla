/*
 * Copyright (c) 2025, JiChiRen Info. Tech.
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
 * Kernel object registry and internal data consistency checking.
 *
 * The registry is deliberately kept outside struct process so that no
 * existing object layout changes and every current construction site
 * keeps working unchanged.
 */

#ifndef KERN_GUARD_H_
#define KERN_GUARD_H_

#include "sys/kern-status.h"

/*
 * Forward declaration only. Including sys/process.h here would pull the
 * whole Contiki header chain into every translation unit that includes
 * this file, and process.h can be reached mid-parse, which breaks the
 * ordering the rest of the kernel headers rely on.
 */
struct process;

/**
 * \brief Reset the registry. Must run before any process is started.
 */
kern_status_t kern_guard_init(void);

/**
 * \brief Register a process, called on successful start.
 */
kern_status_t kern_guard_register(struct process *p);

/**
 * \brief Remove a process from the registry, called on exit.
 */
kern_status_t kern_guard_unregister(struct process *p);

/**
 * \brief Verify the integrity of the process list.
 *
 * Detects unregistered processes, illegal states and list cycles.
 */
kern_status_t kern_guard_check(void);

/**
 * \brief Number of processes currently registered.
 */
unsigned int kern_guard_process_count(void);

/**
 * \brief Last recorded fault, or NULL when none was recorded.
 */
const kern_fault_record_t *kern_guard_last_fault(void);

#endif /* KERN_GUARD_H_ */

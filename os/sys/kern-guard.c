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
 * 3. Neither the name of the Institute nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
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
 */

/*
 * contiki.h must come first: it pulls in sys/process.h in the order the
 * other kernel headers expect. Including sys/kern-guard.h first would
 * start process.h half way through and leave PROCESS_NONE undefined by
 * the time sys/ctimer.h and sys/etimer.h are reached.
 */
#include "contiki.h"

#include "sys/process.h"
#include "sys/kern-guard.h"

#include <stddef.h>

/* Process state values, must stay in step with process.c */
#define KERN_PROC_STATE_NONE      0
#define KERN_PROC_STATE_RUNNING   1
#define KERN_PROC_STATE_CALLED    2
#define KERN_PROC_STATE_MAX       KERN_PROC_STATE_CALLED

#ifndef KERN_CONF_MAX_PROCESSES
#define KERN_CONF_MAX_PROCESSES 16
#endif

static struct process *registered[KERN_CONF_MAX_PROCESSES];
static unsigned int registered_count;

static kern_fault_record_t last_fault;
static uint16_t fault_sequence;
static volatile unsigned int fault_freeze;
/*---------------------------------------------------------------------------*/
void
kern_error_hook_default(kern_status_t status,
                        kern_module_t module,
                        unsigned int line)
{
  /* Record the fault before freezing so that it survives inspection. */
  last_fault.signature = KERN_FAULT_SIGNATURE;
  last_fault.status = (uint8_t)status;
  last_fault.module = (uint8_t)module;
  last_fault.line = (uint16_t)line;
  fault_sequence++;
  last_fault.sequence = fault_sequence;

  /*
   * Default safe state: freeze the kernel and let the independent
   * watchdog reset the device. Override with KERN_CONF_ERROR_HOOK to
   * reach a project specific safe state instead.
   */
  fault_freeze = 1U;
  while(fault_freeze != 0U) {
    /* Intentionally stop here. */
  }
}
/*---------------------------------------------------------------------------*/
const kern_fault_record_t *
kern_guard_last_fault(void)
{
  if(last_fault.signature != KERN_FAULT_SIGNATURE) {
    return NULL;
  }
  return &last_fault;
}
/*---------------------------------------------------------------------------*/
kern_status_t
kern_guard_init(void)
{
  unsigned int i;

  for(i = 0U; i < KERN_CONF_MAX_PROCESSES; i++) {
    registered[i] = NULL;
  }
  registered_count = 0U;
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
static unsigned int
find_slot(const struct process *p)
{
  unsigned int i;

  for(i = 0U; i < KERN_CONF_MAX_PROCESSES; i++) {
    if(registered[i] == p) {
      return i;
    }
  }
  return KERN_CONF_MAX_PROCESSES;
}
/*---------------------------------------------------------------------------*/
kern_status_t
kern_guard_register(struct process *p)
{
  unsigned int i;

  kern_return_if_fail(p != NULL, KERN_ERR_NULL, KERN_MOD_GUARD);

  if(find_slot(p) != KERN_CONF_MAX_PROCESSES) {
    return KERN_ERR_ALREADY;
  }

  for(i = 0U; i < KERN_CONF_MAX_PROCESSES; i++) {
    if(registered[i] == NULL) {
      registered[i] = p;
      registered_count++;
      return KERN_OK;
    }
  }

  kern_raise(KERN_ERR_FULL, KERN_MOD_GUARD, (unsigned int)__LINE__);
  return KERN_ERR_FULL;
}
/*---------------------------------------------------------------------------*/
kern_status_t
kern_guard_unregister(struct process *p)
{
  unsigned int slot;

  kern_return_if_fail(p != NULL, KERN_ERR_NULL, KERN_MOD_GUARD);

  slot = find_slot(p);
  if(slot == KERN_CONF_MAX_PROCESSES) {
    return KERN_ERR_NOT_READY;
  }

  registered[slot] = NULL;
  if(registered_count > 0U) {
    registered_count--;
  }
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
unsigned int
kern_guard_process_count(void)
{
  return registered_count;
}
/*---------------------------------------------------------------------------*/
kern_status_t
kern_guard_check(void)
{
  struct process *p;
  unsigned int steps = 0U;

  /*
   * Walk the live process list. The step count is bounded by the number
   * of registered objects, which turns a corrupted cyclic list into a
   * detected fault instead of a hang.
   */
  for(p = process_list; p != NULL; p = p->next) {
    steps++;

    if(steps > KERN_CONF_MAX_PROCESSES) {
      kern_raise(KERN_ERR_INTERNAL, KERN_MOD_GUARD, (unsigned int)__LINE__);
      return KERN_ERR_INTERNAL;
    }

    if(find_slot(p) == KERN_CONF_MAX_PROCESSES) {
      kern_raise(KERN_ERR_INTERNAL, KERN_MOD_GUARD, (unsigned int)__LINE__);
      return KERN_ERR_INTERNAL;
    }

    if(p->state > KERN_PROC_STATE_MAX) {
      kern_raise(KERN_ERR_INTERNAL, KERN_MOD_GUARD, (unsigned int)__LINE__);
      return KERN_ERR_INTERNAL;
    }

    if(p->thread == NULL) {
      kern_raise(KERN_ERR_INTERNAL, KERN_MOD_GUARD, (unsigned int)__LINE__);
      return KERN_ERR_INTERNAL;
    }
  }

  return KERN_OK;
}
/*---------------------------------------------------------------------------*/

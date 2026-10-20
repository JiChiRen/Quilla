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
 * Kernel status codes and mandatory error reporting interface.
 *
 * Every kernel entry point reports a detailed status instead of silently
 * returning void. Failures are routed to a single error hook which the
 * application is expected to override to reach a defined safe state.
 */

#ifndef KERN_STATUS_H_
#define KERN_STATUS_H_

#include "contiki-conf.h"

#include <stdint.h>

/**
 * \brief Kernel status codes.
 *
 * Values are small non-negative integers so that callers which still
 * ignore the return value keep compiling and behaving as before.
 */
typedef enum {
  KERN_OK = 0,          /**< Operation completed successfully. */
  KERN_ERR_NULL,        /**< A mandatory pointer argument was NULL. */
  KERN_ERR_ALREADY,     /**< Object is already in the requested state. */
  KERN_ERR_NOT_READY,   /**< Kernel or object not initialised. */
  KERN_ERR_FULL,        /**< Fixed size resource exhausted. */
  KERN_ERR_RANGE,       /**< Argument outside the permitted range. */
  KERN_ERR_CONTEXT,     /**< Called from an illegal context, e.g. an ISR. */
  KERN_ERR_INTERNAL,    /**< Internal data consistency check failed. */
  KERN_ERR_STACK,       /**< Stack overflow: the watermark canary was lost. */
  KERN_ERR_SEQUENCE,    /**< Program flow error: token out of order, skipped
                             or not completed inside its time budget. */
  KERN_ERR_CLOCK        /**< A clock cross-check window landed outside the
                             band admitted around the measured rate. */
} kern_status_t;

/**
 * \brief Module identifiers reported with a fault.
 */
typedef enum {
  KERN_MOD_NONE = 0,
  KERN_MOD_PROCESS,
  KERN_MOD_ETIMER,
  KERN_MOD_CTIMER,
  KERN_MOD_CLOCK,
  KERN_MOD_GUARD,
  KERN_MOD_WATCHDOG,
  KERN_MOD_APP,
  KERN_MOD_STACK,
  KERN_MOD_FLOW,
  KERN_MOD_WDT
} kern_module_t;

/**
 * \brief Persistent fault record.
 *
 * Kept in RAM so that a debugger, or the application after a watchdog
 * reset, can inspect the first fault that occurred.
 */
typedef struct {
  uint16_t signature;   /**< KERN_FAULT_SIGNATURE when the record is valid. */
  uint8_t  status;      /**< kern_status_t of the fault. */
  uint8_t  module;      /**< kern_module_t of the fault. */
  uint16_t line;        /**< Source line that raised the fault. */
  uint16_t sequence;    /**< Increments on every fault. */
} kern_fault_record_t;

#define KERN_FAULT_SIGNATURE 0xA5C3U

/**
 * \brief Default error hook.
 *
 * Records the fault and freezes the kernel so that the independent
 * watchdog performs a reset. Applications should override this with
 * KERN_CONF_ERROR_HOOK to enter a project specific safe state.
 */
void kern_error_hook_default(kern_status_t status,
                             kern_module_t module,
                             unsigned int line);

#ifdef KERN_CONF_ERROR_HOOK
#define kern_raise(status, module, line) \
  KERN_CONF_ERROR_HOOK((status), (module), (line))
#else
#define kern_raise(status, module, line) \
  kern_error_hook_default((status), (module), (line))
#endif

/**
 * \brief Validate a condition, raise a fault and return the status.
 *
 * \param expr   Condition that must hold.
 * \param status Status to return and report when expr is false.
 * \param module Module identifier reported with the fault.
 */
#define kern_return_if_fail(expr, status, module)                       \
  do {                                                                  \
    if(!(expr)) {                                                       \
      kern_raise((status), (module), (unsigned int)__LINE__);           \
      return (status);                                                  \
    }                                                                   \
  } while(0)

#endif /* KERN_STATUS_H_ */

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
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE INSTITUTE AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE INSTITUTE OR CONTRIBUTORS BE LIABLE
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
 * \addtogroup shell
 * @{
 *
 * \defgroup quilla-shell Quilla interactive command shell (hardware-independent)
 *
 * A small, hardware-independent command shell. The shell core knows nothing
 * about UART/DMA/GPIO: it owns a lock-free single-producer/single-consumer
 * ring buffer (ISR writes, process reads), a static command registry, line
 * parsing, dispatch and a built-in "help" command.
 *
 * Input is pushed in by the HAL port through shell_input(); output is emitted
 * through a writer callback registered with shell_set_writer(). Applications
 * register commands with shell_register_cmd() and are never referenced by the
 * shell core, keeping the shell fully decoupled from other applications.
 *
 * @{
 */

#ifndef QUILLA_SHELL_H_
#define QUILLA_SHELL_H_

#include "contiki.h"
#include <stdint.h>
#include <stdarg.h>

#include "sys/quilla-version.h"

/* Quilla firmware version reported by the "kernel" shell command. Derived from
 * the single source of truth in sys/quilla-version.h so it can never drift. */
#define QUILLA_LITE_VERSION QUILLA_VERSION_STRING

/* Callback used by the shell to emit a NUL-terminated string. Supplied by the
 * hardware port (e.g. a blocking USART transmit). The shell core never touches
 * hardware directly. */
typedef void (*shell_write_t)(const char *s);

/* A command handler receives the argument string that followed the command
 * name (may be NULL/empty). Output is produced via shell_output/shell_puts. */
typedef void (*shell_cmd_handler_t)(const char *args);

/* Static command descriptor, registered by applications. */
typedef struct shell_cmd {
  const char *name;                 /* command name, no spaces */
  shell_cmd_handler_t handler;      /* handler, must be non-NULL */
  const char *help;                 /* one-line help text */
} shell_cmd_t;

/* The shell process (defined in quilla-shell.c). Started by the port. */
PROCESS_NAME(shell_process);

/* Initialize the shell core (ring buffer, registry, built-ins). */
int shell_init(void);

/* Set the output writer (called by the hardware port at init time). */
void shell_set_writer(shell_write_t writer);

/* Push received bytes into the shell. Called from interrupt context by the
 * hardware port. Bytes are buffered; an event pokes the shell process. */
void shell_input(const uint8_t *data, uint16_t len);

/* Register a command. Returns 0 on success, -1 if the registry is full or the
 * descriptor is invalid. Decoupled: applications register their own commands;
 * the shell core never references application code except through the handler
 * at dispatch time. */
int shell_register_cmd(const shell_cmd_t *cmd);

/* Formatted output (printf-style). Routed through the writer callback. */
void shell_output(const char *fmt, ...);

/* Raw NUL-terminated string output. */
void shell_puts(const char *s);

/* Set the partition identity reported by the built-in "part" command.
 * Called by each system's start-up (QBOOT -> "qboot", APP -> "app1") so the
 * shell core itself stays agnostic about which image is running. Defaults to
 * "app1". */
void shell_set_partition_tag(const char *tag);

#endif /* QUILLA_SHELL_H_ */
/** @} */
/** @} */

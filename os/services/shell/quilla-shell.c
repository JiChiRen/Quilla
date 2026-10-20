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
 * \addtogroup quilla-shell
 * @{
 */

#include "contiki.h"
#include "quilla-shell.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Configuration (static, no heap -- matches the kernel safety policy)    */
#define SHELL_RING_SIZE   256U    /* must be a power of two */
#define SHELL_LINE_MAX    128U
#define SHELL_MAX_CMDS    16U

/* ------------------------------------------------------------------ */
/* Ring buffer: ISR writes head, shell process reads tail.               */
static uint8_t shell_ring[SHELL_RING_SIZE];
static volatile uint16_t shell_ring_head;   /* next byte to write */
static volatile uint16_t shell_ring_tail;   /* next byte to read  */

/* ------------------------------------------------------------------ */
/* Command registry (static array of pointers, no heap).                */
static const shell_cmd_t *shell_cmds[SHELL_MAX_CMDS];
static uint8_t shell_cmd_count;

/* Output writer (set by the hardware port). */
static shell_write_t shell_writer;

/* Partition identity reported by the "part" command. Set at run-time by the
 * running image (QBOOT -> "qboot", APP -> "app1"); the shell core never decides. */
static const char *shell_part_tag = "app1";

/* ------------------------------------------------------------------ */
/* Forward declarations.                                               */
static void shell_drain(void);
static void shell_exec_line(char *line);
static void help_handler(const char *args);
static void kernel_handler(const char *args);
static void part_handler(const char *args);

/* Provided by the hardware port: pull any new bytes the DMA has written into
 * the shell ring / OTA sink. Called from the shell thread (never an ISR) so the
 * RX consumer index is touched by a single context. */
void shellport_rx_harvest(void);

/* Built-in help command (registered at init, listed by itself). */
static const shell_cmd_t help_cmd = {
  "help", help_handler, "list available commands"
};

/* Built-in kernel command: prints the firmware version. */
static const shell_cmd_t kernel_cmd = {
  "kernel", kernel_handler, "show Quilla lite version"
};

/* Built-in part command: prints the partition identity ("qboot" / "app1"),
 * which is supplied at run-time via shell_set_partition_tag(). */
static const shell_cmd_t part_cmd = {
  "part", part_handler, "show partition identity"
};

/* ------------------------------------------------------------------ */
int
shell_init(void)
{
  shell_ring_head = 0;
  shell_ring_tail = 0;
  shell_cmd_count = 0;
  shell_writer = NULL;
  memset(shell_cmds, 0, sizeof(shell_cmds));
  shell_register_cmd(&help_cmd);
  shell_register_cmd(&kernel_cmd);
  shell_register_cmd(&part_cmd);
  return 0;
}

void
shell_set_writer(shell_write_t writer)
{
  shell_writer = writer;
}

int
shell_register_cmd(const shell_cmd_t *cmd)
{
  if(cmd == NULL || cmd->name == NULL || cmd->handler == NULL) {
    return -1;
  }
  if(shell_cmd_count >= SHELL_MAX_CMDS) {
    return -1;
  }
  shell_cmds[shell_cmd_count++] = cmd;
  return 0;
}

void
shell_puts(const char *s)
{
  if(shell_writer != NULL && s != NULL) {
    shell_writer(s);
  }
}

void
shell_output(const char *fmt, ...)
{
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  shell_puts(buf);
}

/* Push received bytes into the ring buffer (called from interrupt context). */
void
shell_input(const uint8_t *data, uint16_t len)
{
  uint16_t i;
  for(i = 0; i < len; i++) {
    uint16_t next = (shell_ring_head + 1U) & (SHELL_RING_SIZE - 1U);
    if(next == shell_ring_tail) {
      /* Ring full: drop the oldest byte so we never overflow / corrupt. */
      break;
    }
    shell_ring[shell_ring_head] = data[i];
    shell_ring_head = next;   /* publish only after the byte is written */
  }
  /* Wake the shell process. process_poll() is the interrupt-safe primitive:
   * it only sets the per-process poll flag and never touches the event queue,
   * so there is no event-queue race and no lost wakeup if the queue is full.
   * The received bytes already live in the ring buffer, so we only need a
   * "drain now" nudge -- not a data-carrying event. */
  process_poll(&shell_process);
}

/* Drain available bytes from the ring, echo them, and run complete lines. */
static void
shell_drain(void)
{
  static char line[SHELL_LINE_MAX];
  static uint16_t line_len = 0;
  static uint8_t prev_eol = 0;
  char echo[2];
  uint16_t processed = 0U;

  /* The head may advance under us via the ISR. We bound the work per call to
   * one ring worth: a sustained RX burst (noise or a runaway sender) must
   * never pin this cooperative process and starve the idle-loop watchdog feed.
   * We yield after draining at most SHELL_RING_SIZE bytes and let the main
   * loop run again; the next poll continues with whatever is left. */
  while((shell_ring_head != shell_ring_tail) &&
        (processed < SHELL_RING_SIZE)) {
    uint8_t c = shell_ring[shell_ring_tail];
    shell_ring_tail = (shell_ring_tail + 1U) & (SHELL_RING_SIZE - 1U);
    processed++;

    if(c == '\r' || c == '\n') {
      if(prev_eol) {
        /* Second byte of a CRLF / LFCR pair: consume, do not echo/prompt. */
        prev_eol = (c == '\r');
        continue;
      }
      prev_eol = 1;
      shell_puts("\r\n");
      if(line_len > 0) {
        line[line_len] = '\0';
        shell_exec_line(line);
        line_len = 0;
      }
      shell_puts("> ");
      continue;
    }
    prev_eol = 0;

    if(c == '\b' || c == 0x7f) {    /* backspace / DEL */
      if(line_len > 0) {
        line_len--;
        shell_puts("\b \b");
      }
      continue;
    }

    if(c >= 0x20 && c < 0x7f) {     /* printable */
      echo[0] = (char)c;
      echo[1] = '\0';
      shell_puts(echo);
      if(line_len < (SHELL_LINE_MAX - 1)) {
        line[line_len++] = (char)c;
      }
    }
  }
}

/* Tokenize and dispatch a single command line. */
static void
shell_exec_line(char *line)
{
  char *cmd = line;
  char *args = NULL;
  uint8_t i;

  while(*cmd == ' ' || *cmd == '\t') {
    cmd++;
  }
  if(*cmd == '\0') {
    return;    /* blank line */
  }

  /* Split at the first whitespace; trim leading spaces from the args. */
  for(i = 0; cmd[i] != '\0'; i++) {
    if(cmd[i] == ' ' || cmd[i] == '\t') {
      cmd[i] = '\0';
      args = &cmd[i + 1];
      while(*args == ' ' || *args == '\t') {
        args++;
      }
      break;
    }
  }

  for(i = 0; i < shell_cmd_count; i++) {
    if(shell_cmds[i] != NULL && strcmp(shell_cmds[i]->name, cmd) == 0) {
      shell_cmds[i]->handler(args);
      return;
    }
  }
  shell_output("Unknown command: %s\r\n", cmd);
}

static void
help_handler(const char *args)
{
  uint8_t i;
  (void)args;
  shell_output("Commands:\r\n");
  for(i = 0; i < shell_cmd_count; i++) {
    if(shell_cmds[i] != NULL) {
      shell_output("  %-10s %s\r\n", shell_cmds[i]->name,
                   shell_cmds[i]->help != NULL ? shell_cmds[i]->help : "");
    }
  }
}

static void
kernel_handler(const char *args)
{
  (void)args;
  shell_output("Quilla lite version V %s\r\n", QUILLA_LITE_VERSION);
}

static void
part_handler(const char *args)
{
  (void)args;
  shell_output("%s\r\n", shell_part_tag != NULL ? shell_part_tag : "unknown");
}

void
shell_set_partition_tag(const char *tag)
{
  shell_part_tag = tag;
}

/* ------------------------------------------------------------------ */
PROCESS(shell_process, "shell");

PROCESS_THREAD(shell_process, ev, data)
{
  PROCESS_BEGIN();
  /* The initial banner + prompt are emitted by the port once the writer is
   * registered; here we just wait for input and drain the ring. */
  while(1) {
    PROCESS_WAIT_EVENT();
    shellport_rx_harvest();   /* pull any new bytes from the DMA ring */
    shell_drain();
  }
  PROCESS_END();
}
/** @} */

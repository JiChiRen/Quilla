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
 * Stack watermark monitoring. See sys/kern-stack.h for the layout.
 *
 * There is no MPU on this class of part, so the software measures instead
 * of trapping: a canary guards the boundary against the static data and a
 * fill pattern records how deep the stack ever got. Both are checked from
 * the idle loop; the canary check is the one that raises a fault.
 */

#include "sys/kern-stack.h"

#include <stdint.h>

#if KERN_CONF_STACK_SHELL
#include "../services/shell/quilla-shell.h"
#endif

/* ---------------------------------------------------------------------------
 * Region boundaries.
 *
 * armlink (Keil) places an explicit STACK area: Stack_Mem .. Stack_Limit,
 * both exported by startup_py32f040xx.s. GNU ld (EIDE) has no such area and
 * lets the stack run from the end of .bss up to _estack, so the .ld files
 * export _stack_origin. Either side can be overridden numerically.
 * ------------------------------------------------------------------------ */
#if defined(KERN_CONF_STACK_BOTTOM)
#define KERN_STACK_BOTTOM_ADDR  ((uint32_t)(KERN_CONF_STACK_BOTTOM))
#elif defined(__ARMCC_VERSION)
extern uint32_t Stack_Mem;
#define KERN_STACK_BOTTOM_ADDR  ((uint32_t)&Stack_Mem)
#else
extern uint32_t _stack_origin;
#define KERN_STACK_BOTTOM_ADDR  ((uint32_t)&_stack_origin)
#endif

#if defined(KERN_CONF_STACK_TOP)
#define KERN_STACK_TOP_ADDR     ((uint32_t)(KERN_CONF_STACK_TOP))
#elif defined(__ARMCC_VERSION)
extern uint32_t Stack_Limit;
#define KERN_STACK_TOP_ADDR     ((uint32_t)&Stack_Limit)
#else
extern uint32_t _estack;
#define KERN_STACK_TOP_ADDR     ((uint32_t)&_estack)
#endif

/* Smallest region worth monitoring: canary plus a few words of fill. */
#define KERN_STACK_MIN_BYTES    ((KERN_CONF_STACK_CANARY_WORDS + 4U) * 4U)

/* --------------------------------------------------------------------------*/
static uint32_t *canary_base;   /* first canary word                        */
static uint32_t *fill_base;     /* first word of the watermark area         */
static uint32_t *stack_end;     /* one word past the highest usable word    */
static uint32_t *low_water;     /* lowest address ever seen in use          */
static uint8_t armed;
static unsigned int overflows;
/* --------------------------------------------------------------------------*/
/**
 * \brief Current main stack pointer.
 *
 * The fill must stop exactly below the live stack; reading MSP is the only
 * exact way to know where that is. MRS <Rn>, MSP is valid on ARMv6-M.
 */
static uint32_t
kern_stack_current_sp(void)
{
  uint32_t sp;

  __asm__ volatile("mrs %0, msp" : "=r"(sp));
  return sp;
}
/* --------------------------------------------------------------------------*/
kern_status_t
kern_stack_init(void)
{
  uint32_t bottom;
  uint32_t top;
  uint32_t sp;
  uint32_t *p;
  unsigned int i;

  bottom = KERN_STACK_BOTTOM_ADDR;
  top = KERN_STACK_TOP_ADDR;

  /* Word align: Cortex-M0+ faults on unaligned word accesses. The top is
   * rounded down and the bottom up, so the region only ever shrinks. */
  bottom = (bottom + 3U) & ~(uint32_t)3U;
  top = top & ~(uint32_t)3U;

  if(top <= bottom || (top - bottom) < KERN_STACK_MIN_BYTES) {
    return KERN_ERR_RANGE;
  }

  canary_base = (uint32_t *)bottom;
  fill_base = canary_base + KERN_CONF_STACK_CANARY_WORDS;
  stack_end = (uint32_t *)top;

  sp = kern_stack_current_sp();
  if(sp <= bottom || sp > top) {
    /* MSP outside the reserved region: the linker symbols do not describe
     * the stack this image actually runs on. Refuse to paint anything. */
    return KERN_ERR_RANGE;
  }

  for(i = 0U; i < KERN_CONF_STACK_CANARY_WORDS; i++) {
    canary_base[i] = KERN_STACK_CANARY;
  }

  /* Paint only the free part: everything from the canary up to MSP. */
  for(p = fill_base; p < (uint32_t *)sp; p++) {
    *p = KERN_STACK_FILL;
  }

  low_water = (uint32_t *)sp;
  overflows = 0U;
  armed = 1;

  return KERN_OK;
}
/* --------------------------------------------------------------------------*/
kern_status_t
kern_stack_check(void)
{
  unsigned int i;
  uint32_t *p;

  if(armed == 0U) {
    return KERN_ERR_NOT_READY;
  }

  for(i = 0U; i < KERN_CONF_STACK_CANARY_WORDS; i++) {
    if(canary_base[i] != KERN_STACK_CANARY) {
      overflows++;
      kern_raise(KERN_ERR_STACK, KERN_MOD_STACK, (unsigned int)__LINE__);
      return KERN_ERR_STACK;
    }
  }

  /* Descend from the previous low-water mark while the words below it are
   * still dirty, and stop on the first word that still holds the fill: that
   * word is the new deepest point. Only bytes consumed since the last check
   * are visited, so an idle-loop call costs a handful of loads rather than a
   * full region scan. */
  p = low_water;
  while(p > fill_base && p[-1] != KERN_STACK_FILL) {
    p--;
  }
  if(p < low_water) {
    low_water = p;
  }

  return KERN_OK;
}
/* --------------------------------------------------------------------------*/
unsigned int
kern_stack_size_bytes(void)
{
  if(armed == 0U) {
    return 0U;
  }
  return (unsigned int)((uint32_t)stack_end - (uint32_t)canary_base);
}
/* --------------------------------------------------------------------------*/
unsigned int
kern_stack_used_bytes(void)
{
  if(armed == 0U) {
    return 0U;
  }
  return (unsigned int)((uint32_t)stack_end - (uint32_t)low_water);
}
/* --------------------------------------------------------------------------*/
unsigned int
kern_stack_free_bytes(void)
{
  if(armed == 0U) {
    return 0U;
  }
  return (unsigned int)((uint32_t)low_water - (uint32_t)canary_base);
}
/* --------------------------------------------------------------------------*/
unsigned int
kern_stack_overflow_count(void)
{
  return overflows;
}
/* --------------------------------------------------------------------------*/
#if KERN_CONF_STACK_SHELL
/* --------------------------------------------------------------------------*/
static char *
kern_stack_put_dec(char *dst, unsigned int v)
{
  char tmp[12];
  unsigned int n = 0U;

  if(v == 0U) {
    tmp[n++] = '0';
  }
  while(v != 0U) {
    tmp[n++] = (char)('0' + (char)(v % 10U));
    v /= 10U;
  }
  while(n > 0U) {
    *dst++ = tmp[--n];
  }
  return dst;
}
/* --------------------------------------------------------------------------*/
static char *
kern_stack_put_str(char *dst, const char *src)
{
  while(*src != '\0') {
    *dst++ = *src++;
  }
  return dst;
}
/* --------------------------------------------------------------------------*/
/* --------------------------------------------------------------------------*/
/**
 * \brief Print the high-water mark reached so far.
 *
 * Boot-path instrumentation: the interesting stack depth is not the one the
 * "stack" command sees (it deliberately avoids vsnprintf, so it is the
 * shallowest path in the shell) but the one reached on the way to the APP -
 * SHA-256 over the image plus the signature check - and inside the QIAP state
 * machine. Both are one-way paths that end in a jump or a reset, so the peak
 * has to be printed before control leaves, or it is lost.
 *
 * The reported peak includes this function's own frame (about 100 bytes),
 * which only ever makes the figure pessimistic.
 */
void
kern_stack_report(const char *tag)
{
  char buf[80];
  char *p = buf;

  if(armed == 0U) {
    return;
  }

  p = kern_stack_put_str(p, "[stack] ");
  if(tag != NULL) {
    p = kern_stack_put_str(p, tag);
    p = kern_stack_put_str(p, ": ");
  }
  p = kern_stack_put_str(p, "peak ");
  p = kern_stack_put_dec(p, kern_stack_used_bytes());
  p = kern_stack_put_str(p, " / ");
  p = kern_stack_put_dec(p, kern_stack_size_bytes());
  p = kern_stack_put_str(p, " B, canary ");
  p = kern_stack_put_str(p, (overflows == 0U) ? "ok" : "LOST");
  p = kern_stack_put_str(p, "\r\n");
  *p = '\0';

  shell_output(buf);
}
/* --------------------------------------------------------------------------*/
static void
kern_stack_cmd_handler(const char *args)
{
  char buf[96];
  char *p = buf;

  (void)args;

  if(armed == 0U) {
    shell_output("stack: monitor not armed\r\n");
    return;
  }

  p = kern_stack_put_str(p, "stack: total ");
  p = kern_stack_put_dec(p, kern_stack_size_bytes());
  p = kern_stack_put_str(p, " B, peak ");
  p = kern_stack_put_dec(p, kern_stack_used_bytes());
  p = kern_stack_put_str(p, " B, free ");
  p = kern_stack_put_dec(p, kern_stack_free_bytes());
  p = kern_stack_put_str(p, " B, canary ");
  p = kern_stack_put_str(p, (overflows == 0U) ? "ok" : "LOST");
  p = kern_stack_put_str(p, "\r\n");
  *p = '\0';

  shell_output(buf);
}
/* --------------------------------------------------------------------------*/
static const shell_cmd_t kern_stack_cmd = {
  "stack", kern_stack_cmd_handler, "stack watermark: size, peak usage, margin"
};
/* --------------------------------------------------------------------------*/
void
kern_stack_shell_init(void)
{
  shell_register_cmd(&kern_stack_cmd);
}
/* --------------------------------------------------------------------------*/
#endif /* KERN_CONF_STACK_SHELL */
/* --------------------------------------------------------------------------*/

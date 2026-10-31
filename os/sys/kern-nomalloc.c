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
 * \file
 * Dynamic memory is forbidden by design (static allocation only).
 *
 * These strong definitions intercept every call to the standard allocator
 * family and route it to the kernel error hook instead of the C library
 * implementation. Because they are linked directly (not from a library),
 * they satisfy any reference to malloc/calloc/realloc/free before the C
 * library's own versions are pulled in, so the real allocator is never
 * linked and no heap is ever touched.
 *
 * This is the armlink-compatible equivalent of GNU ld's --wrap: the
 * ARM linker does not implement --wrap, so we provide the intercept
 * symbols directly.
 *
 * _sbrk()/_sbrk_r() are intercepted for the same reason: they are the
 * allocator's own back door. newlib (EIDE/GCC) reaches the heap through
 * _sbrk rather than through malloc, and libnosys ships a stub that
 * returns -1 - harmless, but it leaves the "no heap exists" claim resting
 * on a stub nobody controls. Intercepting it makes the claim explicit and
 * identical on both toolchains. armlink/microlib never references either
 * symbol, so they cost nothing on the Keil side.
 */

#include "sys/kern-status.h"

#include <stddef.h>

/*---------------------------------------------------------------------------*/
void *
malloc(size_t size)
{
  (void)size;
  kern_raise(KERN_ERR_INTERNAL, KERN_MOD_APP, __LINE__);
  return NULL;
}
/*---------------------------------------------------------------------------*/
void *
calloc(size_t nmemb, size_t size)
{
  (void)nmemb;
  (void)size;
  kern_raise(KERN_ERR_INTERNAL, KERN_MOD_APP, __LINE__);
  return NULL;
}
/*---------------------------------------------------------------------------*/
void *
realloc(void *ptr, size_t size)
{
  (void)ptr;
  (void)size;
  kern_raise(KERN_ERR_INTERNAL, KERN_MOD_APP, __LINE__);
  return NULL;
}
/*---------------------------------------------------------------------------*/
void
free(void *ptr)
{
  (void)ptr;
  kern_raise(KERN_ERR_INTERNAL, KERN_MOD_APP, __LINE__);
}
/*---------------------------------------------------------------------------*/
/* newlib's heap back door. struct _reent is only ever referenced through an
 * incomplete pointer here, so <sys/reent.h> is not needed - and must not be
 * pulled in, since the ARM Compiler has no such header.
 *
 * Returning (void *)-1 is newlib's "out of memory" convention, so even a
 * caller that ignores the error hook sees a failed allocation rather than a
 * pointer into space this image does not own. */
struct _reent;

/* Prototypes: MISRA C:2012 Rule 8.4 requires both of these to be declared
 * before use, and -Wmissing-prototypes flags them otherwise. A stray third
 * translation unit calling _sbrk() directly would then be caught too. */
void *_sbrk(int incr);
void *_sbrk_r(struct _reent *reent, int incr);

void *
_sbrk(int incr)
{
  (void)incr;
  kern_raise(KERN_ERR_INTERNAL, KERN_MOD_APP, __LINE__);
  return (void *)-1;
}
/*---------------------------------------------------------------------------*/
void *
_sbrk_r(struct _reent *reent, int incr)
{
  (void)reent;
  (void)incr;
  kern_raise(KERN_ERR_INTERNAL, KERN_MOD_APP, __LINE__);
  return (void *)-1;
}
/*---------------------------------------------------------------------------*/

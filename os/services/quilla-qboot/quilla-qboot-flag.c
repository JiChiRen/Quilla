/*
 * Copyright (c) 2026, JiChiRen Info. Tech.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED. IN NO EVENT SHALL THE
 * INSTITUTE OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ----------------------------------------------------------------------------
 * QBOOT/APP interaction flag.
 *
 * The flag is now one field of the wear-levelled META store
 * (quilla-qboot-meta.c): quilla_qboot_flag_* simply delegate to it, so the flag
 * is appended (never erased-in-place) and enjoys the same power-loss safety as
 * the rest of the META log.
 * ----------------------------------------------------------------------------
 */

/**
 * \addtogroup quilla-qboot-flag
 * @{
 */

#include "quilla-qboot-flag.h"

#include "quilla-qboot-meta.h"

void
quilla_qboot_flag_init(void)
{
  /* Bring the META store online; also seeds the cached flag. The clock must
   * already be running (Flash needs HCLK). */
  quilla_qboot_meta_init();
}

uint32_t
quilla_qboot_flag_get(void)
{
  quilla_meta_state_t s;

  quilla_qboot_meta_get(&s);
  if(s.flag == QUILLA_QBOOT_FLAG_QIAP) {
    return QUILLA_QBOOT_FLAG_QIAP;
  }
  if(s.flag == QUILLA_QBOOT_FLAG_QBOOT) {
    return QUILLA_QBOOT_FLAG_QBOOT;
  }
  return QUILLA_QBOOT_FLAG_APP1;   /* absent / unknown -> safe default */
}

int
quilla_qboot_flag_set(uint32_t flag)
{
  return quilla_qboot_meta_set_flag(flag);
}

int
quilla_qboot_flag_erase(void)
{
  /* Explicit wipe of the whole META region; an operator-initiated reset, not a
   * runtime write. Returns 0 (meta_reset cannot fail on erase). */
  quilla_qboot_meta_reset();
  return 0;
}

/** @} */

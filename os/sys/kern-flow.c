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
 * Program-flow sequence monitoring. See sys/kern-flow.h for the contract.
 *
 * Storage is fixed size and the whole module is ROM-resident except for the
 * small state array: no heap, consistent with the rest of the kernel safety
 * layer. An unused slot is identified by required == 0, which is also the
 * value register() rejects, so a zero-filled .bss starts out empty.
 */

#include "sys/kern-flow.h"

#include <stdint.h>


/* All ones, i.e. "nothing arrived yet" in the complemented copy. */
#define KERN_FLOW_TOKEN_NONE        ((kern_flow_token_t)0xFFFFU)

/* Highest checkpoint index a token can carry. */
#define KERN_FLOW_MAX_STEPS         16U

typedef struct {
  const char *name;
  kern_flow_token_t required;      /* every cycle must visit these bits     */
  clock_time_t budget;             /* 0 = no timing check                   */
  clock_time_t started;            /* clock time of the current cycle start  */
  clock_time_t worst_cycle;        /* longest cycle completed so far         */
  kern_flow_token_t arrived;       /* checkpoints reached this cycle         */
  kern_flow_token_t arrived_inv;   /* complement; used to detect bit flips    */
  kern_flow_token_t expect_next;   /* next token that may arrive, or 0        */
  unsigned int cycles;
  unsigned int violations;
} kern_flow_track_t;

static kern_flow_track_t tracks[KERN_FLOW_MAX_TRACKS];

/*---------------------------------------------------------------------------*/
/**
 * \brief First bit of `required` that has not been reached yet.
 *
 * This is what makes the sequence ordered rather than merely complete: a
 * flow may not visit checkpoint 3 while checkpoints 0..2 are still missing.
 */
static kern_flow_token_t
next_expected(kern_flow_token_t required, kern_flow_token_t arrived)
{
  unsigned int i;
  kern_flow_token_t bit;

  for(i = 0U; i < KERN_FLOW_MAX_STEPS; i++) {
    bit = (kern_flow_token_t)(1U << i);
    if(((required & bit) != 0U) && ((arrived & bit) == 0U)) {
      return bit;
    }
  }
  return (kern_flow_token_t)0U;
}
/*---------------------------------------------------------------------------*/
static int
id_in_range(int id)
{
  return (id >= 0) && ((unsigned int)id < KERN_FLOW_MAX_TRACKS);
}
/*---------------------------------------------------------------------------*/
static void
restart_cycle(kern_flow_track_t *t)
{
  t->arrived = (kern_flow_token_t)0U;
  t->arrived_inv = KERN_FLOW_TOKEN_NONE;
  t->expect_next = next_expected(t->required, (kern_flow_token_t)0U);
  t->started = clock_time();
}
/*---------------------------------------------------------------------------*/
int
kern_flow_register(const char *name, kern_flow_token_t required,
                   clock_time_t budget)
{
  unsigned int i;

  if(required == 0U) {
    kern_raise(KERN_ERR_RANGE, KERN_MOD_FLOW, (unsigned int)__LINE__);
    return -1;
  }

  for(i = 0U; i < KERN_FLOW_MAX_TRACKS; i++) {
    if(tracks[i].required == 0U) {
      tracks[i].name = name;
      tracks[i].required = required;
      tracks[i].budget = budget;
      tracks[i].worst_cycle = (clock_time_t)0U;
      tracks[i].cycles = 0U;
      tracks[i].violations = 0U;
      restart_cycle(&tracks[i]);
      return (int)i;
    }
  }

  kern_raise(KERN_ERR_FULL, KERN_MOD_FLOW, (unsigned int)__LINE__);
  return -1;
}
/*---------------------------------------------------------------------------*/
kern_status_t
kern_flow_checkpoint(int id, kern_flow_token_t token)
{
  kern_flow_track_t *t;
  clock_time_t now;
  clock_time_t elapsed;

  if(!id_in_range(id)) {
    return KERN_ERR_RANGE;
  }
  t = &tracks[id];

  if(t->required == 0U) {
    return KERN_ERR_NOT_READY;
  }

  /* A token carries exactly one bit. */
  if((token == 0U) || ((token & (token - 1U)) != 0U) ||
     ((token & t->required) == 0U)) {
    t->violations++;
    restart_cycle(t);
    kern_raise(KERN_ERR_SEQUENCE, KERN_MOD_FLOW, (unsigned int)__LINE__);
    return KERN_ERR_SEQUENCE;
  }

  if(token != t->expect_next) {
    /* Skipped a checkpoint, visited it twice, or came back out of order.
     * Restarting keeps a single disturbance from stalling the flow: what
     * matters for the safety case is that the event is counted and raised,
     * not that the sequence stays broken until someone resets it. */
    t->violations++;
    restart_cycle(t);
    kern_raise(KERN_ERR_SEQUENCE, KERN_MOD_FLOW, (unsigned int)__LINE__);
    return KERN_ERR_SEQUENCE;
  }

  now = clock_time();
  t->arrived = (kern_flow_token_t)(t->arrived | token);
  t->arrived_inv = (kern_flow_token_t)(~t->arrived);

  if(t->arrived == t->required) {
    elapsed = (clock_time_t)(now - t->started);
    if(elapsed > t->worst_cycle) {
      t->worst_cycle = elapsed;
    }
    t->cycles++;
    restart_cycle(t);
  } else {
    t->expect_next = next_expected(t->required, t->arrived);
  }

  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
kern_status_t
kern_flow_reset(int id)
{
  if(!id_in_range(id)) {
    return KERN_ERR_RANGE;
  }
  if(tracks[id].required == 0U) {
    return KERN_ERR_NOT_READY;
  }
  restart_cycle(&tracks[id]);
  return KERN_OK;
}
/*---------------------------------------------------------------------------*/
void
kern_flow_reset_all(void)
{
  unsigned int i;

  for(i = 0U; i < KERN_FLOW_MAX_TRACKS; i++) {
    if(tracks[i].required != 0U) {
      restart_cycle(&tracks[i]);
    }
  }
}
/*---------------------------------------------------------------------------*/
kern_status_t
kern_flow_check(void)
{
  unsigned int i;
  kern_status_t rc = KERN_OK;
  clock_time_t now;
  clock_time_t elapsed;
  kern_flow_track_t *t;

  now = clock_time();

  for(i = 0U; i < KERN_FLOW_MAX_TRACKS; i++) {
    t = &tracks[i];
    if(t->required == 0U) {
      continue;
    }

    /* Complement check. Catches a corrupted arrival set, which would
     * otherwise look like progress. */
    if(t->arrived != (kern_flow_token_t)(~t->arrived_inv)) {
      t->violations++;
      restart_cycle(t);
      kern_raise(KERN_ERR_SEQUENCE, KERN_MOD_FLOW, (unsigned int)__LINE__);
      rc = KERN_ERR_SEQUENCE;
      continue;
    }

    if(t->budget == 0U) {
      continue;
    }

    elapsed = (clock_time_t)(now - t->started);
    if(elapsed > t->budget) {
      t->violations++;
      restart_cycle(t);
      kern_raise(KERN_ERR_SEQUENCE, KERN_MOD_FLOW, (unsigned int)__LINE__);
      rc = KERN_ERR_SEQUENCE;
    }
  }

  return rc;
}
/*---------------------------------------------------------------------------*/
static kern_flow_track_t *
lookup(int id)
{
  if(!id_in_range(id)) {
    return NULL;
  }
  if(tracks[id].required == 0U) {
    return NULL;
  }
  return &tracks[id];
}
/*---------------------------------------------------------------------------*/
unsigned int
kern_flow_track_count(void)
{
  unsigned int i;
  unsigned int n = 0U;

  for(i = 0U; i < KERN_FLOW_MAX_TRACKS; i++) {
    if(tracks[i].required != 0U) {
      n++;
    }
  }
  return n;
}
/*---------------------------------------------------------------------------*/
unsigned int
kern_flow_violation_total(void)
{
  unsigned int i;
  unsigned int n = 0U;

  for(i = 0U; i < KERN_FLOW_MAX_TRACKS; i++) {
    n += tracks[i].violations;
  }
  return n;
}
/*---------------------------------------------------------------------------*/
unsigned int
kern_flow_cycles(int id)
{
  const kern_flow_track_t *t = lookup(id);

  return (t != NULL) ? t->cycles : 0U;
}
/*---------------------------------------------------------------------------*/
unsigned int
kern_flow_violations(int id)
{
  const kern_flow_track_t *t = lookup(id);

  return (t != NULL) ? t->violations : 0U;
}
/*---------------------------------------------------------------------------*/
clock_time_t
kern_flow_worst_cycle(int id)
{
  const kern_flow_track_t *t = lookup(id);

  return (t != NULL) ? t->worst_cycle : (clock_time_t)0U;
}
/*---------------------------------------------------------------------------*/
const char *
kern_flow_name(int id)
{
  const kern_flow_track_t *t = lookup(id);

  return (t != NULL) ? t->name : NULL;
}
/*---------------------------------------------------------------------------*/

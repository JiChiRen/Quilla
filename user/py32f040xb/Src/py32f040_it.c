/**
  ******************************************************************************
  * @file    py32f040_it.c
  * @author  MCU Application Team
  * @brief   Interrupt Service Routines.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2023 Puya Semiconductor Co.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by Puya under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2016 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under BSD 3-Clause license,
  * the "License"; You may not use this file except in compliance with the
  * License. You may obtain a copy of the License at:
  *                        opensource.org/licenses/BSD-3-Clause
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "py32f040_it.h"

/* Private includes ----------------------------------------------------------*/
/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/
/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/
/* Private function prototypes -----------------------------------------------*/
/* Private user code ---------------------------------------------------------*/
/* External variables --------------------------------------------------------*/
/* Available from the shell UART port; used to emit a fault "dying gasp" from
 * the HardFault handler without depending on any interrupt being live. */
extern void shellport_emergency_write(const char *s);

/******************************************************************************/
/*          Cortex-M0+ Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
}

/**
  * @brief This function handles Hard fault interrupt.
  *
  * Naked entry snapshots EXC_RETURN and the fault stack pointer *before* the
  * toolchain prologue can shift SP, then delegates to a normal C helper that
  * emits the register/memory view and resets. This is the only reliable way to
  * read the stacked R0..R3,R12,LR,PC,xPSR on Cortex-M0+ (the helper's own
  * stack must not be confused with the fault frame).
  */
__attribute__((naked))
void HardFault_Handler(void)
{
  __asm volatile (
    "mov r0, lr\n"            /* EXC_RETURN                                 */
    "movs r1, #4\n"           /* bit2 mask of EXC_RETURN (M0+ has no TST #imm) */
    "tst r0, r1\n"            /* bit2: 1 -> fault used PSP, 0 -> MSP        */
    "beq 1f\n"
    "mrs r1, psp\n"
    "b 2f\n"
    "1: mrs r1, msp\n"
    "2: bl HardFault_Dump\n"  /* (lr_val=r0, sp_val=r1) does not return     */
    "b .\n"
  );
}

/* Mark used + noinline so LTO does not drop this symbol: the only caller is
 * the naked HardFault_Handler through inline asm (bl HardFault_Dump), which
 * the cross-module optimizer cannot see as a reference. */
__attribute__((used, noinline))
void
HardFault_Dump(uint32_t lr_val, uint32_t sp_val)
{
  /* A genuine HardFault has no safe recovery on this platform. Issue a
   * software reset so the box does not wedge. The naked HardFault_Handler
   * wrapper ends in "b .", so this reset is the only way out of the handler.
   * lr_val / sp_val are passed by the asm wrapper but intentionally unused:
   * we deliberately do NOT touch the faulting stack frame here. */
  (void)lr_val;
  (void)sp_val;
  NVIC_SystemReset();
  while (1)
  {
  }
}

/**
  * @brief This function handles System service call via SWI instruction.
  */
void SVC_Handler(void)
{
}

/**
  * @brief This function handles Pendable request for system service.
  */
void PendSV_Handler(void)
{
}

#if 0
/**
  * @brief This function handles System tick timer.
  */
void SysTick_Handler(void)
{
  HAL_IncTick();
}
#endif
/******************************************************************************/
/* PY32F040xB Peripheral Interrupt Handlers                                     */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file.                                          */
/******************************************************************************/

/************************ (C) COPYRIGHT Puya *****END OF FILE******************/

/*
******************************************************************************
**  File        : py32f040xb_qboot.ld
**  Abstract    : Linker script for the QIAP (bootloader) image on PY32F040xB.
**                Flash 128K total. The QBOOT occupies the first 24K
**                (0x08000000 - 0x08005FFF). The META region (flag + manifest)
**                and the APP image live above this region; the QIAP must never
**                touch them.
**
**  Derived from py32f040xb.ld (Puya). Only the FLASH memory area is changed.
*****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* _estack (initial MSP) is no longer the top of RAM: it is placed at the top
   of the reserved stack inside ._user_heap_stack, so the GNU ld build gets the
   same fixed partition as armlink does -- statics first, then a stack of
   exactly _Min_Stack_Size, then headroom for .bss/.data to grow into. */
_Min_Heap_Size = 0x0;        /* static allocation only */
/* Reserved main stack. Must match Stack_Size in
   MDK-ARM/startup_py32f040xx.s so both toolchains reserve the same region. */
_Min_Stack_Size = 0x1000;

/* Specify the memory areas */
MEMORY
{
RAM (rw)       : ORIGIN = 0x20000000, LENGTH = 16K
FLASH (rx)      : ORIGIN = 0x8000000, LENGTH = 24K
}

/* Define output sections */
SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab   : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array     :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH
  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH
  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    PROVIDE ( _stack_origin = . );   /* lowest byte of the stack reservation */
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
    PROVIDE ( _estack = . );   /* initial MSP = top of the reserved stack */
  } >RAM

  /* Remove information from the standard libraries.
   * See py32f040xb_app.ld for the full rationale: crt0.o (auto-linked by the
   * GCC driver, but redundant here -- the project startup calls main
   * directly) is discarded because its exit() pulls in the newlib stdio
   * teardown chain (findfp.o/_malloc_r/_sbrk_r), a dynamic-allocation path
   * this no-MPU, no-heap build forbids. The libc_nano.a entries are a hard
   * policy gate: referencing exit()/stdio/malloc fails the build. */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
    *crt0.o (.text .text.*)
    libc_nano.a (libc_a-exit.o libc_a-findfp.o libc_a-mallocr.o libc_a-nano-mallocr.o libc_a-reallocr.o)
  }

  .ARM.attributes : { *(.ARM.attributes) } >FLASH

  /* Link-time guards for the fixed memory partition claim (no MPU on this
     part, so the partition has to be enforced by the linker instead).
     Fail the build rather than ship an image whose stack reservation was
     silently squeezed by .bss/.data growth. */
  ASSERT(_estack - _stack_origin >= _Min_Stack_Size,
         "Reserved main stack is smaller than _Min_Stack_Size")
  ASSERT(_estack <= ORIGIN(RAM) + LENGTH(RAM),
         "Reserved main stack runs past the end of RAM")
  ASSERT(_stack_origin >= _ebss, "Stack reservation overlaps .bss/.data")
}

/*
******************************************************************************
**  File        : gd32f502xE_qboot.ld
**  Abstract    : Linker script for the QBOOT (bootloader) image on GD32F502xE.
**
**                Flash 512K, SRAM 64K. The QBOOT owns the first 24K
**                (0x08000000 - 0x08005FFF); the APP image starts at
**                0x08007000 and the META region (flag + manifest) sits just
**                below it at 0x08006000. The QBOOT must never write above its
**                own region.
**
**                The vendor script (gd32f502xC_flash.ld) only covers the 256K
**                'C' variant, so this file derives the 'E' layout from it and
**                adds the fixed stack reservation the kernel stack watermark
**                monitor needs (_stack_origin / _estack).
**
**  Entry        : Reset_Handler (CMSIS-style, from startup_gd32f50x.S)
*****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* _estack (initial MSP) is not the top of RAM: it is placed at the top of the
   reserved stack inside ._user_heap_stack, so the GNU ld build gets the same
   fixed partition armlink would give -- statics first, then a stack of exactly
   _Min_Stack_Size, then headroom for .bss/.data to grow into. */
_Min_Heap_Size = 0x0;        /* static allocation only, see os/sys/kern-nomalloc.c */
_Min_Stack_Size = 0x2000;    /* 8 KiB main stack */

/* Specify the memory areas */
MEMORY
{
RAM (rw)       : ORIGIN = 0x20000000, LENGTH = 64K
FLASH (rx)     : ORIGIN = 0x08000000, LENGTH = 24K
}

/* Define output sections */
SECTIONS
{
  /* ISR vectors. The GD32 CMSIS startup emits .vectors (not .isr_vector) and
     takes the initial MSP from the symbol _sp. */
  .vectors :
  {
    . = ALIGN(4);
    KEEP(*(.vectors))
    . = ALIGN(4);
    __Vectors_End = .;
    __Vectors_Size = __Vectors_End - __gVectors;
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

  .preinit_array :
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
    PROVIDE ( _estack = . );         /* initial MSP = top of the reserved stack */
  } >RAM

  /* CMSIS startup reads the initial MSP from _sp. */
  PROVIDE ( _sp = _estack );

  .ARM.attributes : { *(.ARM.attributes) } >FLASH

  /* Link-time guards for the fixed memory partition claim. Fail the build
     rather than ship an image whose stack reservation was silently squeezed
     by .bss/.data growth. */
  ASSERT(_estack - _stack_origin >= _Min_Stack_Size,
         "Reserved main stack is smaller than _Min_Stack_Size")
  ASSERT(_estack <= ORIGIN(RAM) + LENGTH(RAM),
         "Reserved main stack runs past the end of RAM")
  ASSERT(_stack_origin >= _ebss, "Stack reservation overlaps .bss/.data")
}

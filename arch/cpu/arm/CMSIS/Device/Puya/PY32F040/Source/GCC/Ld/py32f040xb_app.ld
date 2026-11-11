/*
******************************************************************************
**  File        : py32f040xb_app.ld
**  Abstract    : Linker script for the APP image on PY32F040xB.
**                Flash 128K total. The QBOOT occupies the first 32K
**                (0x08000000 - 0x08007FFF); this APP image starts at the
**                APP base 0x08007000 and runs to the end of flash. The META
**                region (flag page + secure-boot manifest) sits just below it
**                at 0x08006000; both are owned by the QIAP, not the APP.
**
**  Derived from py32f040xb.ld (Puya). Only the FLASH memory area is changed.
**
**  NOTE: Because the vector table is now linked at 0x08007000, the APP must
**  relocate VTOR to APP_START at run time. See doc/qboot-app-architecture.md
**  section 3 (APP_BUILD-guarded SCB->VTOR = APP_START in system_py32f040.c).
*****************************************************************************
*/

/* Entry Point */
ENTRY(Reset_Handler)

/* _estack (initial MSP) is no longer the top of RAM: it is placed at the top
   of the reserved stack inside ._user_heap_stack, so the GNU ld build gets the
   same fixed partition as armlink does -- statics first, then a stack of
   exactly _Min_Stack_Size, then headroom for .bss/.data to grow into. */
_Min_Heap_Size = 0x0;        /* required amount of heap (static allocation only) */
/* Reserved main stack. Must match Stack_Size in
   MDK-ARM/startup_py32f040xx.s so both toolchains reserve the same region. */
_Min_Stack_Size = 0x1000;

/* Specify the memory areas */
MEMORY
{
RAM (rw)       : ORIGIN = 0x20000000, LENGTH = 0x4000
FLASH (rx)      : ORIGIN = 0x8007000, LENGTH = 0x17000
}

/* Define output sections */
SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector)) /* Startup code */
    . = ALIGN(4);
  } >FLASH

  /* Firmware security-version region for the secure-boot anti-rollback gate.
   * QBOOT reads it at the FIXED address QUILLA_QBOOT_APP_START + 0x400
   * (0x08007400) so it does not need the APP's symbol table. The content is
   * emitted by QUILLA_APP_SECVER_PLACE() in the QAPP (see quilla-app-secver.h).
   * Placed explicitly well past the vector table (192 B) and inside the real
   * image; the region is covered by the signed digest. */
  .quilla_secver 0x08007400 :
  {
    KEEP(*(.quilla_secver))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)           /* .text sections (code) */
    *(.text*)          /* .text* sections (code) */
    *(.glue_7)         /* glue arm to thumb code */
    *(.glue_7t)        /* glue thumb to arm code */
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    . = ALIGN(4);
    _etext = .;        /* define a global symbols at end of code */
  } >FLASH

  /* Constant data goes into FLASH */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)         /* .rodata sections (constants, strings, etc.) */
    *(.rodata*)        /* .rodata* sections (constants, strings, etc.) */
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

  /* used by the startup to initialize data */
  _sidata = LOADADDR(.data);

  /* Initialized data sections goes into RAM, load LMA copy after code */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;        /* create a global symbol at data start */
    *(.data)           /* .data sections */
    *(.data*)          /* .data* sections */

    . = ALIGN(4);
    _edata = .;        /* define a global symbol at data end */
  } >RAM AT> FLASH


  /* Uninitialized data section */
  . = ALIGN(4);
  .bss :
  {
    /* This is used by the startup in order to initialize the .bss secion */
    _sbss = .;         /* define a global symbol at bss start */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)

    . = ALIGN(4);
    _ebss = .;         /* define a global symbol at bss end */
    __bss_end__ = _ebss;
  } >RAM

  /* User_heap_stack section, used to check that there is enough RAM left */
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
   * NOTE: with -specs=nano.specs the archive is libc_nano.a, so the "libc.a"
   * pattern below never matched anything (kept for documentation value).
   * crt0.o is discarded explicitly: the GCC driver auto-links it, but this
   * project brings its own startup (Reset_Handler calls SystemInit,
   * __libc_init_array and main directly), so crt0's _start is dead weight and
   * its exit() reference drags in the whole newlib stdio teardown chain
   * (findfp.o/_malloc_r/_sbrk_r) -- a dynamic-allocation path this no-MPU,
   * no-heap build forbids. crti.o/crtn.o stay and still provide _init/_fini
   * for __libc_init_array. The libc_nano.a entries are a hard policy gate:
   * any future code that references exit()/stdio/malloc fails the link
   * outright instead of silently growing an unreachable-teardown chain. */
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

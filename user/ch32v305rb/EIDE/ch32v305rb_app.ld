/*
 * CH32V305VB (RISC-V) APP-image linker script.
 *
 * 128 KiB Flash linked at the 0x00000000 alias (WCH startup convention). The
 * QBOOT owns the first 24 KiB (0x00000000 - 0x00005FFF); the META region sits
 * at 0x00006000; this APP image starts at 0x00007000 and runs to the end of
 * flash (0x0001FFFF). 32 KiB SRAM at 0x20000000.
 *
 * Section order is dictated by startup_ch32v30x_D8C.S:
 *   .init   -> _start ("j handle_reset"), MUST be the first byte at ORIGIN so
 *              the QBOOT branch to APP_START lands on it and the reset PC is 0.
 *   .vector -> _vector_base, 64-byte aligned (mtvec vectored mode).
 *   .quilla_secver -> pinned at APP_START + 0x400 (0x00007400), read by QBOOT.
 *   .text/.rodata/arrays, then .data (with __global_pointer$), .bss, .stack.
 *
 * The startup loads sp from _eusrstack and copies .data via _data_lma/_data_vma/
 * _edata, clears .bss via _sbss/_ebss. kern-stack.c reads _stack_origin/_estack,
 * provided here as aliases of the same stack reservation.
 */

ENTRY(_start)

__stack_size = 0x2000;   /* 8 KiB main stack */

MEMORY
{
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 32K
  FLASH (rx)  : ORIGIN = 0x00007000, LENGTH = 0x18000
}

SECTIONS
{
  .init :
  {
    . = ALIGN(4);
    KEEP(*(SORT_NONE(.init)))
    . = ALIGN(4);
  } >FLASH AT>FLASH

  .vector :
  {
    . = ALIGN(64);
    KEEP(*(.vector))
    . = ALIGN(64);
  } >FLASH AT>FLASH

  /* Firmware security-version region (anti-rollback). QBOOT reads the FIXED
   * address 0x00007400 (QUILLA_QBOOT_APP_START + 0x400) without the APP symbol
   * table. Emitted by QUILLA_APP_SECVER_PLACE() in the QAPP; covered by the
   * signed digest. The ~480-byte vector table fits below 0x400, so this does
   * not collide with it. */
  .quilla_secver 0x00007400 :
  {
    KEEP(*(.quilla_secver))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text.*)
    . = ALIGN(4);
  } >FLASH AT>FLASH

  .fini :
  {
    KEEP(*(SORT_NONE(.fini)))
    . = ALIGN(4);
  } >FLASH AT>FLASH

  PROVIDE(_etext = .);

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } >FLASH AT>FLASH

  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
    KEEP(*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
    PROVIDE_HIDDEN(__init_array_end = .);
  } >FLASH AT>FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
    KEEP(*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .dtors))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } >FLASH AT>FLASH

  .dalign :
  {
    . = ALIGN(4);
    PROVIDE(_data_vma = .);
  } >RAM AT>FLASH

  .dlalign :
  {
    . = ALIGN(4);
    PROVIDE(_data_lma = .);
  } >FLASH AT>FLASH

  .data :
  {
    *(.gnu.linkonce.r.*)
    *(.data .data.*)
    *(.gnu.linkonce.d.*)
    . = ALIGN(8);
    PROVIDE(__global_pointer$ = . + 0x800);
    *(.sdata .sdata.*)
    *(.sdata2.*)
    *(.gnu.linkonce.s.*)
    . = ALIGN(8);
    *(.srodata.cst16)
    *(.srodata.cst8)
    *(.srodata.cst4)
    *(.srodata.cst2)
    *(.srodata .srodata.*)
    . = ALIGN(4);
    PROVIDE(_edata = .);
  } >RAM AT>FLASH

  .bss :
  {
    . = ALIGN(4);
    PROVIDE(_sbss = .);
    *(.sbss*)
    *(.gnu.linkonce.sb.*)
    *(.bss*)
    *(.gnu.linkonce.b.*)
    *(COMMON*)
    . = ALIGN(4);
    PROVIDE(_ebss = .);
  } >RAM AT>FLASH

  PROVIDE(_end = _ebss);
  PROVIDE(end = .);

  .stack ORIGIN(RAM) + LENGTH(RAM) - __stack_size :
  {
    . = ALIGN(8);
    PROVIDE(_heap_end = .);
    PROVIDE(_susrstack = .);
    PROVIDE(_stack_origin = _susrstack);   /* kern-stack.c (GNU) */
    . = . + __stack_size;
    . = ALIGN(8);
    PROVIDE(_eusrstack = .);
    PROVIDE(_estack = _eusrstack);          /* kern-stack.c (GNU) */
  } >RAM

  /* Fail the build rather than ship an image whose stack reservation was
     silently squeezed by .bss/.data growth. */
  ASSERT(_estack - _stack_origin >= __stack_size,
         "Reserved main stack is smaller than __stack_size")
  ASSERT(_estack <= ORIGIN(RAM) + LENGTH(RAM),
         "Reserved main stack runs past the end of RAM")
  ASSERT(_stack_origin >= _ebss, "Stack reservation overlaps .bss/.data")
}

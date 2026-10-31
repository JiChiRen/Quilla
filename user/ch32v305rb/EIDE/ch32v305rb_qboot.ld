/*
 * CH32V305VB (RISC-V) QBOOT-image linker script.
 *
 * 128 KiB Flash linked at the 0x00000000 alias. The QBOOT owns the first
 * 24 KiB (0x00000000 - 0x00005FFF). 32 KiB SRAM at 0x20000000.
 *
 * Section order is dictated by startup_ch32v30x_D8C.S (see the APP script for
 * the symbol contract: _eusrstack / _data_lma / _data_vma / _edata / _sbss /
 * _ebss / __global_pointer$). No .quilla_secver here: that region belongs to
 * the APP image.
 */

ENTRY(_start)

__stack_size = 0x800;    /* 2 KiB main stack (bootloader) */

MEMORY
{
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 32K
  FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 0x6000
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
    PROVIDE(_stack_origin = _susrstack);
    . = . + __stack_size;
    . = ALIGN(8);
    PROVIDE(_eusrstack = .);
    PROVIDE(_estack = _eusrstack);
  } >RAM

  ASSERT(_estack - _stack_origin >= __stack_size,
         "Reserved main stack is smaller than __stack_size")
  ASSERT(_estack <= ORIGIN(RAM) + LENGTH(RAM),
         "Reserved main stack runs past the end of RAM")
  ASSERT(_stack_origin >= _ebss, "Stack reservation overlaps .bss/.data")
}

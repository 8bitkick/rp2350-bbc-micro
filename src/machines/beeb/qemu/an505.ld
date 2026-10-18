/* QEMU mps2-an505 (Cortex-M33, secure state): everything in the 4 MB SSRAM
 * at 0x10000000 (the secure alias, where QEMU's SVTOR points), stack at its
 * top; the 16 MB at 0x80000000 stands in for the Presto's PSRAM (and holds
 * the profiler's samples).
 */
ENTRY(reset_handler)

MEMORY
{
  SSRAM (rwx) : ORIGIN = 0x10000000, LENGTH = 4M
  BIGRAM (rw) : ORIGIN = 0x80000000, LENGTH = 16M
}

SECTIONS
{
  .vectors : { KEEP(*(.vectors)) } > SSRAM
  .text : {
    *(.text*)
    KEEP(*(.init)) KEEP(*(.fini))
    *(.rodata*)
    . = ALIGN(4);
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array))
    PROVIDE_HIDDEN(__preinit_array_end = .);
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE_HIDDEN(__init_array_end = .);
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(.fini_array))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } > SSRAM
  .ARM.exidx : { *(.ARM.exidx*) } > SSRAM
  .data : { *(.data*) . = ALIGN(4); } > SSRAM
  .bss (NOLOAD) : {
    . = ALIGN(4);
    __bss_start__ = .;
    *(.bss*) *(COMMON)
    . = ALIGN(4);
    __bss_end__ = .;
  } > SSRAM
  end = .;
  PROVIDE(_end = .);
  __stack_top = ORIGIN(SSRAM) + LENGTH(SSRAM);

  /* The thumb2 JIT's fixed region (4MB of code, its no-code and stub pages;
   * BEEBJIT_THUMB2_JIT_ADDR=0x80000000), ~8MB of "PSRAM", then 4MB of
   * profile samples.
   */
  .jit (NOLOAD) : {
    . += 0x410000;
  } > BIGRAM
  .big (NOLOAD) : {
    __big_start = .;
    . += 0x7F0000;
    __big_end = .;
  } > BIGRAM
  .samples (NOLOAD) : { *(.samples*) } > BIGRAM
}

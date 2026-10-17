/* A link-only image for library analysis, with separate RX and RW segments.
 * This is not a board memory map or a flashable firmware image. */
ENTRY(main)
PHDRS { code PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS {
  . = 0x10000000;
  .text : { *(.text .text.*) } :code
  .rodata : { *(.rodata .rodata.*) } :code
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } :code
  .ARM.exidx : {
    __exidx_start = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end = .;
  } :code
  . = 0x20000000;
  .data : {
    __global_pointer$ = . + 0x800;
    *(.data .data.* .sdata .sdata.*)
  } :data
  .bss (NOLOAD) : { *(.bss .bss.* .sbss .sbss.*) *(COMMON) } :data
  _end = .;
}

/* keepalive.c in the WLAN core's RAM; MODULE_BASE is given on the command line. Data is loaded, not zeroed. */
ENTRY(keepalive_receive)
SECTIONS
{
  . = MODULE_BASE;
  .module : {
    *(.text.keepalive_receive) *(.text.keepalive_mib) *(.text*) *(.rodata*)
    . = ALIGN(4);
    *(.data*) *(.bss*) *(COMMON)
    . = ALIGN(4);
  }
  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.ARM.exidx*) }
}

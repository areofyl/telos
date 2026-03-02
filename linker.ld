ENTRY(_start)

SECTIONS
{
  . = 0x40080000;

  .text : {
    *(.text._start)
    *(.text*)
    
  }

  .rodata : {
    *(.rodata*)
  }

  .data : {
    *(.data*)
  }

  .bss : {
    __bss_start = .;
    *(.bss*)
    __bss_end = .;
  }

  . = ALIGN(16);
  . = . + 4096;
  _stack_top = .;
}

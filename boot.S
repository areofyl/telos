.section .text._start
.global _start

_start:
  // check current exception level
  mrs x0, CurrentEL
  lsr x0, x0, #2
  cmp x0, #2
  b.ne at_el1

  // we're at EL2 — drop to EL1
  mov x0, #(1 << 31)       // EL1 is AArch64
  msr hcr_el2, x0
  mov x0, #0x3c5           // EL1h with DAIF masked
  msr spsr_el2, x0
  adr x0, at_el1
  msr elr_el2, x0
  eret

at_el1:
  ldr x0, =_stack_top
  mov sp, x0

  ldr x0, =vectors
  msr vbar_el1, x0
  isb

  bl main

halt:
  wfe
  b halt

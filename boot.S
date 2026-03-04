.section .text._start
.global _start

_start:
  // check if we're at EL2
  mrs x0, CurrentEL
  lsr x0, x0, #2
  cmp x0, #2
  b.ne at_el1

  // drop to EL1
  mov x0, #(1 << 31)
  msr hcr_el2, x0
  mov x0, #0x3c5
  msr spsr_el2, x0
  adr x0, at_el1
  msr elr_el2, x0
  eret

at_el1:
  ldr x0, =_stack_top
  mov sp, x0

  // enable fp/simd otherwise the compiler generated code traps
  mov x0, #(3 << 20)
  msr cpacr_el1, x0
  isb

  // set up vector table
  ldr x0, =vectors
  msr vbar_el1, x0
  isb

  bl main

halt:
  wfe
  b halt

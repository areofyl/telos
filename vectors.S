// vector table
// entries are just branches since the full save/restore
// doesnt fit in 0x80 bytes

.section .text

// save everything including sp_el0 (user stack pointer)
// after this, sp points at the bottom of the trap frame
.macro SAVE_ALL
    stp x0,  x1,  [sp, #-16]!
    stp x2,  x3,  [sp, #-16]!
    stp x4,  x5,  [sp, #-16]!
    stp x6,  x7,  [sp, #-16]!
    stp x8,  x9,  [sp, #-16]!
    stp x10, x11, [sp, #-16]!
    stp x12, x13, [sp, #-16]!
    stp x14, x15, [sp, #-16]!
    stp x16, x17, [sp, #-16]!
    stp x18, x19, [sp, #-16]!
    stp x20, x21, [sp, #-16]!
    stp x22, x23, [sp, #-16]!
    stp x24, x25, [sp, #-16]!
    stp x26, x27, [sp, #-16]!
    stp x28, x29, [sp, #-16]!
    mrs x9,  elr_el1
    mrs x10, spsr_el1
    mrs x11, sp_el0
    stp x30, x9,  [sp, #-16]!
    stp x10, x11, [sp, #-16]!
.endm

// restore everything and return
// if the scheduler swapped sp, this restores a different task
.macro RESTORE_ALL
    ldp x10, x11, [sp], #16
    ldp x30, x9,  [sp], #16
    msr elr_el1, x9
    msr spsr_el1, x10
    msr sp_el0, x11
    ldp x28, x29, [sp], #16
    ldp x26, x27, [sp], #16
    ldp x24, x25, [sp], #16
    ldp x22, x23, [sp], #16
    ldp x20, x21, [sp], #16
    ldp x18, x19, [sp], #16
    ldp x16, x17, [sp], #16
    ldp x14, x15, [sp], #16
    ldp x12, x13, [sp], #16
    ldp x10, x11, [sp], #16
    ldp x8,  x9,  [sp], #16
    ldp x6,  x7,  [sp], #16
    ldp x4,  x5,  [sp], #16
    ldp x2,  x3,  [sp], #16
    ldp x0,  x1,  [sp], #16
    eret
.endm

.balign 2048
.global vectors
vectors:
    // current EL, SP_EL0
    .balign 0x80; b exc_entry
    .balign 0x80; b irq_entry
    .balign 0x80; b exc_entry
    .balign 0x80; b exc_entry

    // current EL, SP_ELx (kernel faults/irqs)
    .balign 0x80; b exc_entry
    .balign 0x80; b irq_entry
    .balign 0x80; b exc_entry
    .balign 0x80; b exc_entry

    // lower EL, aarch64 (userspace)
    .balign 0x80; b svc_entry
    .balign 0x80; b irq_entry
    .balign 0x80; b exc_entry
    .balign 0x80; b exc_entry

    // lower EL, aarch32 (not used)
    .balign 0x80; b exc_entry
    .balign 0x80; b irq_entry
    .balign 0x80; b exc_entry
    .balign 0x80; b exc_entry

// sync exception from kernel - save, dump info, hang
exc_entry:
    SAVE_ALL
    mrs x0, esr_el1
    mrs x1, elr_el1
    mrs x2, far_el1
    bl exception_handler
    RESTORE_ALL

// irq - save, handle (may switch tasks), restore
irq_entry:
    SAVE_ALL
    mov x0, sp
    bl irq_handler
    mov sp, x0
    RESTORE_ALL

// sync from EL0 - syscalls land here
// could also be a user fault so the C handler checks ESR
svc_entry:
    SAVE_ALL
    mov x0, sp
    bl syscall_handler
    mov sp, x0
    RESTORE_ALL

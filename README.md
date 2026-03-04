# PebbleOS

minimal aarch64 kernel from scratch in C and arm64 assembly, runs on QEMU virt. no libc, no dependencies, everything written from scratch.

## what it does

- boots on qemu virt (cortex-a72), drops from EL2 to EL1
- uart output
- exception vectors
- timer interrupt via gic + arm generic timer
- physical page allocator (bitmap, 4KB pages)

## build

```
make
make run
make clean
```

needs `gcc`, `ld`, `objcopy`, `qemu-system-aarch64`

exit qemu: ctrl+a then x

## files

- `boot.S` — entry point, EL2 drop, stack, fp/simd enable, vector install
- `vectors.S` — exception vector table
- `main.c` — uart driver, main
- `exception.c` — prints exception info and halts
- `gic.c` — interrupt controller setup
- `timer.c` — arm generic timer, 1 sec tick
- `irq.c` — irq handler
- `pmm.c` — physical memory allocator
- `linker.ld` — memory layout
- `Makefile` — build

## roadmap

- [x] uart hello world
- [x] exception vectors
- [x] timer interrupt
- [x] physical memory allocator
- [ ] mmu / virtual memory
- [ ] scheduler
- [ ] user space (EL0) + syscalls
- [ ] filesystem (ramdisk)
- [ ] shell

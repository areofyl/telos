# telOS

a tiny aarch64 kernel i'm building from scratch for fun. no libc, no dependencies, just C and arm64 assembly running on QEMU virt.

it's a SASOS (Single Address Space OS) — all tasks share one set of page tables, and isolation is done by flipping permission bits on context switch instead of swapping page tables. weird but it works.

## what it can do

- boots on qemu virt (cortex-a72), drops from EL2 to EL1
- uart (PL011), full exception handling with register dumps
- GIC + arm generic timer for preemptive round-robin scheduling
- physical page allocator (bitmap, 4KB pages, 128MB)
- MMU with 4KB granule, identity-mapped kernel, per-task slots
- EL0 userspace with syscalls (svc #0)
- IPC — synchronous message passing (send/recv/call/reply)
- nameserver so tasks can find each other by name
- tag-based filesystem (no directories — files have key:value tags)
- interactive shell to poke around the filesystem
- built-in C compiler (tcc) — JIT compiles a subset of C to aarch64 and runs it

## the tag-based fs

instead of directories, files have tags. you label things and query by tags:

```
telos> create notes.txt
telos> write notes.txt some stuff i wrote
telos> tag notes.txt type text
telos> tag notes.txt topic ideas
telos> query type text
notes.txt
log.txt
telos> tags notes.txt
  type = text
  topic = ideas
```

it's kind of like how you'd search for files if folders didn't exist. still figuring out where to take this but it's a fun model to play with.

## the C compiler

there's a built-in C compiler (`cc` command) that JIT-compiles a subset of C directly to aarch64 machine code and runs it. no assembler, no linker — single-pass recursive descent that emits instructions into a buffer and jumps to it.

```
telos> cat hello.c
int main() {
  int i = 0;
  while (i < 10) {
    putc('0' + i);
    putc('\n');
    i = i + 1;
  }
  return 0;
}
telos> cc hello.c
[cc] running...
0
1
2
3
4
5
6
7
8
9

[cc] exit code: 0
```

supports: `int` variables, `if`/`else`, `while`, arithmetic (`+ - * / %`), comparisons, `&&`/`||`, `putc()`, `getc()`, char/int literals, `return`.

## build

```
make
make run
make clean
```

needs `gcc` (aarch64 cross-compiler), `ld`, `objcopy`, `qemu-system-aarch64`

exit qemu: `ctrl+a` then `x`

## memory map

| range | what |
|-------|------|
| `0x00000000-0x3fffffff` | device memory (UART, GIC, etc) |
| `0x40000000-0x47ffffff` | RAM (identity mapped) |
| `0x40080000` | kernel load address |
| `0x80000000+` | process slots (16MB each, up to 8 tasks) |

## files

| file | what |
|------|------|
| `boot.S` | entry, EL2 drop, stack setup, bss zeroing |
| `vectors.S` | vector table, save/restore macros, syscall + irq entry |
| `main.c` | everything userspace — uart server, nameserver, fs, shell, compiler, tasks |
| `exception.c` | exception handler (ESR/ELR/FAR dump) |
| `gic.c` | GIC distributor + cpu interface |
| `timer.c` | arm generic timer, 1s tick |
| `irq.c` | irq dispatch + scheduler hook |
| `pmm.c` | bitmap page allocator |
| `mmu.c` | page tables, map/unmap, permission toggling, device mapping |
| `proc.c` | process slots, scheduler, context switch |
| `syscall.c` | syscall dispatch (write, yield, exit, send, recv, call, reply, cacheflush) |
| `linker.ld` | memory layout |

## how it works

tasks run at EL0. they `svc #0` to make syscalls — handled at EL1. every timer tick, the scheduler saves the current task's state, picks the next one, flips page permissions so only the active task can touch its memory, and restores.

each task gets a 16MB slot starting at 0x80000000. the kernel maps code + stack pages there and controls access with AP bits — running task gets EL0 rw, everyone else EL1-only.

IPC is synchronous: `sys_call` sends a message and blocks until the server replies. servers loop on `sys_recv` waiting for requests. the nameserver (pid 1) lets tasks register names and look each other up, so nothing is hardcoded.

the fs server stores files in memory with tags instead of a directory tree. the shell talks directly to UART for input and uses IPC to talk to the fs server.

## what's next

not sure yet, just seeing where this goes. maybe pipes, maybe spawning tasks from the shell, maybe porting to a raspberry pi, maybe something else entirely.

# telOS

A tiny aarch64 kernel I'm building from scratch for fun. No libc, no dependencies, just C and ARM64 assembly running on QEMU virt and Raspberry Pi 5.

It's a SASOS (Single Address Space OS) — all tasks share one set of page tables, and isolation is done by flipping permission bits on context switch instead of swapping page tables. Weird but it works.

## What It Can Do

- Boots on QEMU virt (Cortex-A72) and RPi 5 (Cortex-A76), drops from EL2 to EL1
- UART (PL011), full exception handling with register dumps
- GIC + ARM generic timer for preemptive round-robin scheduling
- Physical page allocator (bitmap, 4KB pages, 128MB)
- MMU with 4KB granule, identity-mapped kernel, per-task slots
- EL0 userspace with syscalls (`svc #0`)
- IPC — synchronous message passing (send/recv/call/reply)
- Nameserver so tasks can find each other by name
- Tag-based filesystem (no directories — files have key:value tags)
- Interactive shell with 15 commands
- Built-in C compiler — JIT compiles a subset of C to aarch64 and runs it
- Task spawning — compile C and launch it as a separate process from the shell
- Text editor (teled)
- Snake game, because why not

## The Tag-Based FS

Instead of directories, files have tags. You label things and query by tags:

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

It's kind of like how you'd search for files if folders didn't exist. Still figuring out where to take this but it's a fun model to play with.

## The C Compiler

There's a built-in C compiler (`cc` command) that JIT-compiles a subset of C directly to aarch64 machine code and runs it. No assembler, no linker — single-pass recursive descent that emits instructions into a buffer and jumps to it.

You can also use `run` to compile and spawn the program as a separate task instead of running it inline.

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
telos> run hello.c
[run] spawned as pid 4
```

Supports: `int` variables, `if`/`else`, `while`, arithmetic (`+ - * / %`), comparisons, `&&`/`||`, `putc()`, `getc()`, char/int literals, `return`.

## Shell Commands

| Command | Description |
|---------|-------------|
| `ls` | List all files |
| `cat <file>` | Read file contents |
| `create <name>` | Create empty file |
| `write <name> <data>` | Write text to file |
| `tag <name> <k> <v>` | Add/update a tag |
| `query <k> <v>` | Find files by tag |
| `tags <file>` | Show file's tags |
| `teled <file>` | Text editor |
| `cc <file>` | Compile & run C (inline) |
| `run <file>` | Compile & spawn as task |
| `ps` | List running tasks |
| `top` | Live task monitor |
| `snake` | Play snake |
| `telfetch` | System info |
| `clear` | Clear screen |

## Build

```
make
make run
make clean
```

Needs `gcc` (aarch64 cross-compiler), `ld`, `objcopy`, `qemu-system-aarch64`.

Exit QEMU: `Ctrl+A` then `X`

## Memory Map (QEMU)

| Range | What |
|-------|------|
| `0x00000000-0x3FFFFFFF` | Device memory (UART, GIC, etc) |
| `0x40000000-0x47FFFFFF` | RAM (identity mapped) |
| `0x40080000` | Kernel load address |
| `0x80000000+` | Process slots (16MB each, up to 8 tasks) |

## Files

| File | What |
|------|------|
| `boot.S` | Entry, EL2 drop, stack setup, BSS zeroing |
| `vectors.S` | Vector table, save/restore macros, syscall + IRQ entry |
| `main.c` | Everything userspace — UART server, nameserver, FS, shell, compiler, editor, snake |
| `exception.c` | Exception handler (ESR/ELR/FAR dump) |
| `gic.c` | GIC distributor + CPU interface |
| `timer.c` | ARM generic timer, 1s tick |
| `irq.c` | IRQ dispatch + scheduler hook |
| `pmm.c` | Bitmap page allocator |
| `mmu.c` | Page tables, map/unmap, permission toggling, device mapping |
| `proc.c` | Process slots, scheduler, context switch, task spawning |
| `syscall.c` | Syscall dispatch (write, yield, exit, IPC, spawn, cacheflush, procinfo) |
| `linker.ld` | Memory layout |

## How It Works

Tasks run at EL0. They `svc #0` to make syscalls — handled at EL1. Every timer tick, the scheduler saves the current task's state, picks the next one, flips page permissions so only the active task can touch its memory, and restores.

Each task gets a 16MB slot starting at `0x80000000`. The kernel maps code + stack pages there and controls access with AP bits — running task gets EL0 RW, everyone else EL1-only.

IPC is synchronous: `sys_call` sends a message and blocks until the server replies. Servers loop on `sys_recv` waiting for requests. The nameserver (PID 1) lets tasks register names and look each other up, so nothing is hardcoded.

The FS server stores files in memory with tags instead of a directory tree. The shell talks directly to UART for input and uses IPC to talk to the FS server.

## What's Next

Not sure yet, just seeing where this goes. Maybe pipes, maybe persistent storage, maybe multi-core, maybe something else entirely.

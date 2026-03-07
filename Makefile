CC = gcc
AS = gcc
LD = ld
OBJCOPY = objcopy

CFLAGS = -ffreestanding -nostdlib -O2 -Wall
ASFLAGS = -c

all: kernel.bin

boot.o: boot.S
	$(AS) $(ASFLAGS) boot.S -o boot.o

vectors.o: vectors.S
	$(AS) $(ASFLAGS) vectors.S -o vectors.o

main.o: main.c
	$(CC) $(CFLAGS) -c main.c -o main.o

exception.o: exception.c
	$(CC) $(CFLAGS) -c exception.c -o exception.o

gic.o: gic.c
	$(CC) $(CFLAGS) -c gic.c -o gic.o

timer.o: timer.c
	$(CC) $(CFLAGS) -c timer.c -o timer.o

irq.o: irq.c
	$(CC) $(CFLAGS) -c irq.c -o irq.o

pmm.o: pmm.c
	$(CC) $(CFLAGS) -c pmm.c -o pmm.o

mmu.o: mmu.c
	$(CC) $(CFLAGS) -mgeneral-regs-only -c mmu.c -o mmu.o

proc.o: proc.c
	$(CC) $(CFLAGS) -c proc.c -o proc.o

syscall.o: syscall.c
	$(CC) $(CFLAGS) -c syscall.c -o syscall.o

kernel.elf: boot.o vectors.o main.o exception.o gic.o timer.o irq.o pmm.o mmu.o proc.o syscall.o linker.ld
	$(LD) -T linker.ld boot.o vectors.o main.o exception.o gic.o timer.o irq.o pmm.o mmu.o proc.o syscall.o -o kernel.elf

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary kernel.elf kernel.bin

run: kernel.bin
	qemu-system-aarch64 -M virt -cpu cortex-a72 -nographic -kernel kernel.bin

clean:
	rm -f *.o kernel.elf kernel.bin

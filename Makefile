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

kernel.elf: boot.o vectors.o main.o exception.o linker.ld
	$(LD) -T linker.ld boot.o vectors.o main.o exception.o -o kernel.elf

kernel.bin: kernel.elf
	$(OBJCOPY) -O binary kernel.elf kernel.bin

run: kernel.bin
	qemu-system-aarch64 -M virt -cpu cortex-a72 -nographic -kernel kernel.bin

clean:
	rm -f *.o kernel.elf kernel.bin

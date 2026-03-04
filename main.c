#include <stdint.h>

// uart
volatile uint8_t *uart = (volatile uint8_t *)0x09000000;

void putchar(char c) { *uart = c; }

void print(const char *s) {
  while (*s) {
    putchar(*s);
    s++;
  }
}

void print_hex(unsigned long val) {
  char buf[17];
  char hex[] = "0123456789abcdef";
  buf[16] = '\0';
  for (int i = 15; i >= 0; i--) {
    buf[i] = hex[val & 0xf];
    val >>= 4;
  }
  print("0x");
  print(buf);
}

void gic_init(void);
void timer_init(void);
void pmm_init(void);
uint64_t alloc_page(void);
void free_page(uint64_t addr);

void main() {
  print("Hello from PebbleOS\n\n");

  print("setting up interrupts\n");
  gic_init();
  print("\n");
  timer_init();
  print("\n");

  print("setting up memory\n");
  pmm_init();
  print("\n");

  // quick test to make sure alloc/free works
  print("testing allocator\n");
  uint64_t p1 = alloc_page();
  print("[test] alloc page 1: ");
  print_hex(p1);
  print("\n");

  uint64_t p2 = alloc_page();
  print("[test] alloc page 2: ");
  print_hex(p2);
  print("\n");

  free_page(p1);
  print("[test] freed page 1\n");

  uint64_t p3 = alloc_page();
  print("[test] alloc page 3: ");
  print_hex(p3);
  print(" (should be same as page 1)\n\n");

  // unmask irqs - clear the I bit in DAIF
  asm volatile("msr daifclr, #2");
  print("[cpu] IRQs unmasked -- waiting for timer...\n\n");

  // sit here forever, irqs will interrupt us
  for (;;) {
    asm volatile("wfe");
  }
}

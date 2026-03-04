#include <stdint.h>

// GIC - interrupt controller
// has a distributor (routes interrupts) and cpu interface (where cpu picks them up)
// both mmio like uart

// distributor
#define GICD_BASE 0x08000000
#define GICD_CTLR (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER0 (*(volatile uint32_t *)(GICD_BASE + 0x100))
#define GICD_IPRIORITY7 (*(volatile uint32_t *)(GICD_BASE + 0x41C))

// cpu interface
#define GICC_BASE 0x08010000
#define GICC_CTLR (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR (*(volatile uint32_t *)(GICC_BASE + 0x010))

void print(const char *s);

void gic_init(void) {
  // enable distributor
  GICD_CTLR = 1;
  print("[gic] distributor enabled\n");

  // enable cpu interface
  GICC_CTLR = 1;

  // priority mask, 0xff = accept all priorities
  // (lower number = higher priority which is kinda confusing)
  GICC_PMR = 0xFF;
  print("[gic] cpu interface enabled\n");

  // enable irq 30 (thats the timer)
  // bit 30 in isenabler0 which covers irqs 0-31
  GICD_ISENABLER0 = (1 << 30);

  // set irq 30 priority to 0 (highest)
  // irq 30 is in the 3rd byte of IPRIORITY7
  uint32_t prio = GICD_IPRIORITY7;
  prio &= ~(0xFF << 16);
  GICD_IPRIORITY7 = prio;

  print("[gic] IRQ 30 (timer) enabled\n");
}

// read GICC_IAR to find out which interrupt fired
// also acknowledges it. 1023 = spurious
uint32_t gic_ack(void) { return GICC_IAR; }

// write back to EOIR when done handling
// forgetting this = no more interrupts lol
void gic_eoi(uint32_t iar) { GICC_EOIR = iar; }

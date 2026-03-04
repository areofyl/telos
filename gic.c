#include <stdint.h>

/*
 * gic (generic interrupt controller) — sits between hardware and the cpu.
 *
 * two parts:
 *   1. distributor (gicd) — receives signals from all devices, routes them
 *   2. cpu interface (gicc) — per-cpu inbox where the cpu picks up interrupts
 *
 * both are memory-mapped, just like uart.
 */

/* distributor registers */
#define GICD_BASE 0x08000000
#define GICD_CTLR (*(volatile uint32_t *)(GICD_BASE + 0x000))
#define GICD_ISENABLER0 (*(volatile uint32_t *)(GICD_BASE + 0x100))
#define GICD_IPRIORITY7 (*(volatile uint32_t *)(GICD_BASE + 0x41C))

/* cpu interface registers */
#define GICC_BASE 0x08010000
#define GICC_CTLR (*(volatile uint32_t *)(GICC_BASE + 0x000))
#define GICC_PMR (*(volatile uint32_t *)(GICC_BASE + 0x004))
#define GICC_IAR (*(volatile uint32_t *)(GICC_BASE + 0x00C))
#define GICC_EOIR (*(volatile uint32_t *)(GICC_BASE + 0x010))

void print(const char *s);

void gic_init(void) {
  /* turn on the distributor — without this it ignores everything */
  GICD_CTLR = 1;
  print("[gic] distributor enabled\n");

  /* turn on the cpu interface — without this the cpu can't see interrupts */
  GICC_CTLR = 1;

  /*
   * priority mask — the cpu only accepts interrupts with priority
   * lower than this number (confusingly, lower number = higher priority).
   * 0xff means "let everything through"
   */
  GICC_PMR = 0xFF;
  print("[gic] cpu interface enabled\n");

  /*
   * enable interrupt #30 (the timer).
   * arm hardcodes the timer to intid 30, it's a PPI (private per-cpu interrupt).
   * isenabler0 covers interrupts 0-31, one bit each. writing a 1 enables,
   * writing a 0 does nothing (there's a separate register to disable).
   */
  GICD_ISENABLER0 = (1 << 30);

  /*
   * set priority for irq 30 to 0 (highest).
   * priorities are packed 4 per register (one byte each).
   * irq 30 is the 3rd byte (bits 23:16) in IPRIORITY7.
   * we clear that byte — 0 = highest priority.
   */
  uint32_t prio = GICD_IPRIORITY7;
  prio &= ~(0xFF << 16);
  GICD_IPRIORITY7 = prio;

  print("[gic] IRQ 30 (timer) enabled\n");
}

/*
 * ask the gic "which interrupt fired?"
 * reading iar also tells the gic we're handling it.
 * low 10 bits = interrupt number. 1023 = spurious (nothing there).
 */
uint32_t gic_ack(void) { return GICC_IAR; }

/*
 * tell the gic we're done handling this interrupt.
 * if you forget this it won't send you any more!
 */
void gic_eoi(uint32_t iar) { GICC_EOIR = iar; }

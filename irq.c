#include <stdint.h>

/*
 * irq handler — called by the vector table when an irq fires.
 *
 * the cpu was doing something, got interrupted, jumped here.
 * we figure out which interrupt it was, handle it, and return.
 * when we return the cpu goes right back to what it was doing.
 */

uint32_t gic_ack(void);
void gic_eoi(uint32_t iar);
void timer_reset(void);
extern volatile uint64_t tick_count;
void print(const char *s);
void print_hex(unsigned long val);

void irq_handler(unsigned long esr, unsigned long elr, unsigned long far) {
    /* the vector macro passes these but irqs don't use them — they're
     * for sync exceptions like page faults. just suppress the warnings */
    (void)esr;
    (void)elr;
    (void)far;

    /* ask the gic which interrupt fired. also tells it we're on it */
    uint32_t iar = gic_ack();
    uint32_t intid = iar & 0x3FF;

    /* 1023 = spurious, the interrupt vanished before we got here.
     * harmless, just bail. don't call eoi for these */
    if (intid == 1023) {
        return;
    }

    if (intid == 30) {
        /* timer went off — one tick has passed */
        tick_count++;
        timer_reset();

        print("[tick] #");
        print_hex(tick_count);
        print("\n");
    } else {
        /* something we didn't expect. shouldn't happen since we
         * only enabled irq 30, but print it just in case */
        print("[irq] unexpected INTID: ");
        print_hex(intid);
        print("\n");
    }

    /* tell the gic we're done. skip this and it stops sending interrupts */
    gic_eoi(iar);
}

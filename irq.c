#include <stdint.h>

// irq handler
// returns a stack pointer - if the scheduler switches tasks,
// this will be different from what we got

uint32_t gic_ack(void);
void gic_eoi(uint32_t iar);
void timer_reset(void);
extern volatile uint64_t tick_count;
uint64_t *schedule(uint64_t *frame);
void print(const char *s);
void print_hex(unsigned long val);

uint64_t irq_handler(uint64_t frame) {
    uint64_t *sp = (uint64_t *)frame;

    uint32_t iar = gic_ack();
    uint32_t intid = iar & 0x3FF;

    if (intid == 1023)
        return frame;

    if (intid == 30) {
        tick_count++;
        timer_reset();

        // let the scheduler decide if we should switch
        sp = schedule(sp);
    } else {
        // silently acknowledge unknown interrupts (e.g. RP1/PCIe spurious)
    }

    gic_eoi(iar);
    return (uint64_t)sp;
}

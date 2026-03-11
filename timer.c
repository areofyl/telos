#include <stdint.h>

// arm generic timer
// cant access through memory like gic/uart, need mrs/msr instructions
// using EL1 physical timer (CNTP), interrupt is always #30

void print(const char *s);
void print_hex(unsigned long val);

static uint64_t timer_interval;

// volatile bc it gets changed in the irq handler
volatile uint64_t tick_count = 0;

// helpers for reading/writing system registers
static inline uint64_t read_cntfrq(void) {
    uint64_t val;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(val));
    return val;
}

static inline void write_cntp_tval(uint64_t val) {
    asm volatile("msr cntp_tval_el0, %0" : : "r"(val));
}

static inline void write_cntp_ctl(uint64_t val) {
    asm volatile("msr cntp_ctl_el0, %0" : : "r"(val));
}

void timer_init(void) {
    uint64_t freq = read_cntfrq();
    print("[timer] frequency: ");
    print_hex(freq);
    print(" Hz\n");

    // set it to 1 second (freq ticks = 1 sec)
    timer_interval = freq;
    write_cntp_tval(timer_interval);

    // allow EL0 to read the physical counter (cntpct_el0)
    uint64_t cntkctl;
    asm volatile("mrs %0, cntkctl_el1" : "=r"(cntkctl));
    cntkctl |= 1;  // EL0PCTEN
    asm volatile("msr cntkctl_el1, %0" : : "r"(cntkctl));

    // bit 0 = enable, bit 1 = mask (0 means not masked)
    write_cntp_ctl(1);

    print("[timer] armed for 1 second\n");
}

// per-core timer setup — CNTP registers are per-core (system regs, not MMIO)
// each core has its own countdown that fires IRQ 30 independently
void timer_init_percpu(void) {
    // allow EL0 to read the counter on this core too
    uint64_t cntkctl;
    asm volatile("mrs %0, cntkctl_el1" : "=r"(cntkctl));
    cntkctl |= 1;
    asm volatile("msr cntkctl_el1, %0" : : "r"(cntkctl));

    write_cntp_tval(timer_interval);
    write_cntp_ctl(1);
}

// reset the countdown after each tick
void timer_reset(void) {
    write_cntp_tval(timer_interval);
}

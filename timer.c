#include <stdint.h>

/*
 * arm generic timer — every arm cpu has a built-in clock.
 *
 * unlike uart/gic, you can't access it through memory. you have to use
 * special assembly instructions (mrs to read, msr to write) because these
 * are "system registers" baked into the cpu itself.
 *
 * we're using the EL1 physical timer (CNTP). its interrupt is always #30.
 *
 * the registers we care about:
 *   cntfrq_el0    — how many ticks per second (read-only)
 *   cntp_tval_el0 — countdown value, fires interrupt when it hits 0
 *   cntp_ctl_el0  — bit 0 = enable, bit 1 = mask the interrupt
 */

void print(const char *s);
void print_hex(unsigned long val);

/* saved so timer_reset() can reload without recalculating */
static uint64_t timer_interval;

/*
 * tick counter — bumped by the irq handler every time the timer fires.
 * volatile because it changes in an interrupt (compiler needs to know
 * it can't just cache this in a register).
 */
volatile uint64_t tick_count = 0;

/*
 * inline asm helpers for system registers.
 * mrs = read system register, msr = write system register.
 * "=r" means "put result in any register", "r" means "input is in any register"
 */

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
    /* read how fast the timer ticks — on qemu cortex-a72 it's 62.5 MHz */
    uint64_t freq = read_cntfrq();
    print("[timer] frequency: ");
    print_hex(freq);
    print(" Hz\n");

    /* set countdown to 1 second worth of ticks.
     * freq ticks = 1 second. for 100ms you'd do freq/10, etc. */
    timer_interval = freq;
    write_cntp_tval(timer_interval);

    /* enable the timer: bit 0 = enable, bit 1 = imask (0 = not masked).
     * so writing 1 means "start counting, and yes fire the interrupt" */
    write_cntp_ctl(1);

    print("[timer] armed for 1 second\n");
}

/* reload the countdown so the timer fires again.
 * called by the irq handler after each tick. */
void timer_reset(void) {
    write_cntp_tval(timer_interval);
}

#include <stdint.h>

// syscall handler
// called when EL0 does svc, or on any sync exception from EL0
// checks ESR to tell syscalls from faults

void print(const char *s);
void print_hex(unsigned long val);
void putchar(char c);
void exception_handler(unsigned long esr, unsigned long elr, unsigned long far);
uint64_t *schedule(uint64_t *frame);
uint64_t *ipc_send(uint64_t *frame, int target, uint64_t msg_ptr, uint32_t len);
uint64_t *ipc_recv(uint64_t *frame, uint64_t buf_ptr, uint32_t max_len);
uint64_t *ipc_call(uint64_t *frame, int target, uint64_t msg_ptr, uint32_t len,
                   uint64_t reply_buf, uint32_t reply_max);
uint64_t *ipc_reply(uint64_t *frame, int target, uint64_t msg_ptr, uint32_t len);

// trap frame layout (from SAVE_ALL)
// the regs are pushed in order: x0,x1 first (highest addr), spsr,sp_el0 last (lowest)
#define FRAME_SPSR  0
#define FRAME_SP_EL0 1
#define FRAME_X30   2
#define FRAME_ELR   3
#define FRAME_X28   4
#define FRAME_X29   5
// ... x26-x9 in between ...
#define FRAME_X8    24
#define FRAME_X2    30
#define FRAME_X3    31
#define FRAME_X4    28
#define FRAME_X1    33
#define FRAME_X0    32

#define SYS_WRITE  0
#define SYS_YIELD  1
#define SYS_EXIT   2
#define SYS_SEND   3
#define SYS_RECV   4
#define SYS_CALL   5
#define SYS_REPLY  6
#define SYS_PROCINFO 7
#define SYS_CACHEFLUSH 8
#define SYS_SPAWN  9

// sys_write(buf, len) - print len bytes from buf, or until \0 if len=0
static void do_write(uint64_t *frame) {
    const char *buf = (const char *)frame[FRAME_X0];
    uint64_t len = frame[FRAME_X1];

    if (len == 0) {
        // null-terminated string
        while (*buf) {
            putchar(*buf);
            buf++;
        }
    } else {
        for (uint64_t i = 0; i < len; i++)
            putchar(buf[i]);
    }
}

// sys_yield() - give up the rest of our time slice
static uint64_t *do_yield(uint64_t *frame) {
    return schedule(frame);
}

// sys_exit() - kill this task, switch to something else
static uint64_t *do_exit(uint64_t *frame);

// forward decl for proc functions we need
void proc_exit_current(void);
uint64_t proc_get_info(char *buf, uint64_t max);
int proc_spawn(void *code_buf, uint32_t code_len, const char *name, uint64_t device_pa);

static uint64_t *do_exit(uint64_t *frame) {
    proc_exit_current();
    // current task is dead, schedule will pick someone else
    return schedule(frame);
}

uint64_t syscall_handler(uint64_t frame_sp) {
    uint64_t *frame = (uint64_t *)frame_sp;

    // check ESR - is this actually an SVC or a real fault?
    uint64_t esr;
    asm volatile("mrs %0, esr_el1" : "=r"(esr));
    uint32_t ec = (esr >> 26) & 0x3F;

    if (ec != 0x15) {
        // not an SVC, its a real exception from userspace
        uint64_t elr, far;
        asm volatile("mrs %0, elr_el1" : "=r"(elr));
        asm volatile("mrs %0, far_el1" : "=r"(far));
        print("[fault] user exception EC=");
        print_hex(ec);
        print(" ESR=");
        print_hex(esr);
        print(" ELR=");
        print_hex(elr);
        print(" FAR=");
        print_hex(far);
        print("\n");
        // kill the task
        proc_exit_current();
        return (uint64_t)schedule(frame);
    }

    uint64_t syscall_num = frame[FRAME_X8];

    switch (syscall_num) {
    case SYS_WRITE:
        do_write(frame);
        frame[FRAME_X0] = 0;
        return (uint64_t)frame;

    case SYS_YIELD:
        return (uint64_t)do_yield(frame);

    case SYS_EXIT:
        return (uint64_t)do_exit(frame);

    case SYS_SEND:
        return (uint64_t)ipc_send(frame,
            (int)frame[FRAME_X0],       // target pid
            frame[FRAME_X1],            // msg pointer
            (uint32_t)frame[FRAME_X2]); // length

    case SYS_RECV:
        return (uint64_t)ipc_recv(frame,
            frame[FRAME_X0],            // buf pointer
            (uint32_t)frame[FRAME_X1]); // max length

    case SYS_CALL:
        return (uint64_t)ipc_call(frame,
            (int)frame[FRAME_X0],       // target pid
            frame[FRAME_X1],            // msg pointer
            (uint32_t)frame[FRAME_X2],  // msg length
            frame[FRAME_X3],            // reply buffer
            (uint32_t)frame[FRAME_X4]); // reply max

    case SYS_REPLY:
        return (uint64_t)ipc_reply(frame,
            (int)frame[FRAME_X0],       // target pid
            frame[FRAME_X1],            // msg pointer
            (uint32_t)frame[FRAME_X2]); // length

    case SYS_CACHEFLUSH: {
        // flush dcache + invalidate icache for a VA range
        // x0 = start address, x1 = length
        uint64_t addr = frame[FRAME_X0];
        uint64_t len = frame[FRAME_X1];
        for (uint64_t i = 0; i < len; i += 4) {
            uint64_t va = addr + i;
            asm volatile("dc cvau, %0" : : "r"(va));
        }
        asm volatile("dsb ish");
        for (uint64_t i = 0; i < len; i += 4) {
            uint64_t va = addr + i;
            asm volatile("ic ivau, %0" : : "r"(va));
        }
        asm volatile("dsb ish");
        asm volatile("isb");
        frame[FRAME_X0] = 0;
        return (uint64_t)frame;
    }

    case SYS_SPAWN: {
        // spawn a new task from JIT code buffer
        // x0 = code buffer, x1 = code length (bytes), x2 = name string
        int new_pid = proc_spawn(
            (void *)frame[FRAME_X0],
            (uint32_t)frame[FRAME_X1],
            (const char *)frame[FRAME_X2],
            0x107D001000);  // RPi 5 UART PA — grant to spawned task
        frame[FRAME_X0] = (uint64_t)new_pid;
        return (uint64_t)frame;
    }

    case SYS_PROCINFO: {
        uint64_t bytes = proc_get_info((char *)frame[FRAME_X0],
                                       frame[FRAME_X1]);
        frame[FRAME_X0] = bytes;
        return (uint64_t)frame;
    }

    default:
        print("[syscall] unknown: ");
        print_hex(syscall_num);
        print("\n");
        frame[FRAME_X0] = (uint64_t)-1;
        return (uint64_t)frame;
    }
}

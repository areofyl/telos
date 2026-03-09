#include <stdint.h>

// process management + scheduler for SASOS
// every process gets a fixed VA region in the single address space
// isolation = flipping AP bits on context switch
// scheduling = round robin on timer tick, stack swap

void print(const char *s);
void print_hex(unsigned long val);
uint64_t alloc_page(void);
void free_page(uint64_t addr);
void map_page(uint64_t va, uint64_t pa, uint64_t flags);
void unmap_page(uint64_t va);
void set_page_flags(uint64_t va, uint64_t flags);

#define PAGE_SIZE    4096
#define MAX_PROCS    8
#define MAX_PAGES    64         // max pages per process
#define SLOT_SIZE    0x1000000  // 16MB per slot

// process slots start at 2GB, outside the kernel's identity map
#define SLOT_BASE    0x80000000

// page flags
#define FLAGS_NORMAL ((3UL << 8) | (1UL << 2))  // inner shareable + normal attr
#define AP_KERN_ONLY (0UL << 6)  // EL1 rw, EL0 nothing
#define AP_USER_RW   (1UL << 6)  // EL1 rw, EL0 rw
#define UXN          (1UL << 54)

// trap frame = 17 pairs of 8 bytes = 272 bytes
// layout from SAVE_ALL (lowest address first):
//   [0] = spsr    [1] = sp_el0
//   [2] = x30     [3] = elr
//   [4] = x28     [5] = x29
//   ...
//   [32] = x0     [33] = x1
#define FRAME_SIZE   272

typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED_SEND,
    PROC_BLOCKED_RECV,
    PROC_BLOCKED_REPLY,
} proc_state_t;

typedef struct {
    proc_state_t state;
    uint64_t slot_base;
    uint32_t num_pages;
    uint64_t phys_pages[MAX_PAGES];
    uint64_t kstack;     // physical page for kernel stack (identity mapped)
    uint64_t saved_sp;   // points at the trap frame on the kernel stack
    // IPC fields
    int ipc_target;      // who we're trying to send to (-1 = nobody)
    uint64_t ipc_msg;    // pointer to message buffer (user VA)
    uint32_t ipc_len;    // message length / max length
    uint8_t  wants_reply;  // set by sys_call, controls post-delivery transition
    uint64_t reply_buf;    // user VA for reply buffer
    uint32_t reply_max;    // max reply length
    // device mapping
    uint64_t device_va;  // VA of mapped device page (0 = none)
    // name
    char name[16];
} proc_t;

static proc_t procs[MAX_PROCS];
static int current_pid = -1;
static uint64_t idle_sp;

void proc_init(void) {
    for (int i = 0; i < MAX_PROCS; i++)
        procs[i].state = PROC_UNUSED;
    current_pid = -1;
    print("[proc] initialized\n");
}

// create a process that will run at EL0
// num_pages = user memory in the slot (last page is used as user stack)
// entry = function to start executing
int proc_create(uint32_t num_pages, void (*entry)(void)) {
    if (num_pages < 2 || num_pages > MAX_PAGES) {
        print("[proc] bad page count\n");
        return -1;
    }

    int pid = -1;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == PROC_UNUSED) {
            pid = i;
            break;
        }
    }
    if (pid < 0) {
        print("[proc] no free slots\n");
        return -1;
    }

    proc_t *p = &procs[pid];
    p->slot_base = SLOT_BASE + (uint64_t)pid * SLOT_SIZE;
    p->num_pages = num_pages;

    // allocate user memory in the slot
    for (uint32_t i = 0; i < num_pages; i++) {
        uint64_t pa = alloc_page();
        if (!pa) {
            print("[proc] out of memory\n");
            for (uint32_t j = 0; j < i; j++) {
                unmap_page(p->slot_base + j * PAGE_SIZE);
                free_page(p->phys_pages[j]);
            }
            p->state = PROC_UNUSED;
            return -1;
        }
        p->phys_pages[i] = pa;
        map_page(p->slot_base + i * PAGE_SIZE, pa, FLAGS_NORMAL | AP_KERN_ONLY);
    }

    // kernel stack - separate page, always accessible (identity mapped in RAM)
    p->kstack = alloc_page();
    if (!p->kstack) {
        print("[proc] cant alloc kernel stack\n");
        return -1;
    }

    // build fake trap frame on the kernel stack
    // RESTORE_ALL will pop this and eret to the entry point at EL0
    uint64_t kstack_top = p->kstack + PAGE_SIZE;
    uint64_t *frame = (uint64_t *)(kstack_top - FRAME_SIZE);

    for (int i = 0; i < FRAME_SIZE / 8; i++)
        frame[i] = 0;

    // user stack = top of the slot (last page, grows down)
    uint64_t user_stack_top = p->slot_base + (uint64_t)num_pages * PAGE_SIZE;

    frame[0] = 0x000;                  // SPSR = EL0t (drop to userspace!)
    frame[1] = user_stack_top;         // SP_EL0 = user stack
    frame[3] = (uint64_t)entry;        // ELR = entry point

    p->saved_sp = (uint64_t)frame;
    p->ipc_target = -1;
    p->name[0] = '\0';
    p->state = PROC_READY;

    print("[proc] created pid ");
    print_hex(pid);
    print(" at ");
    print_hex(p->slot_base);
    print(" (EL0)\n");

    return pid;
}

// trap frame indices (must match syscall.c)
#define FRAME_X0 32
#define FRAME_X1 33
#define FRAME_X3 31
#define FRAME_X4 28

// forward declarations
void proc_set_name(int pid, const char *name);

// device memory attributes: attr index 0 (device-nGnRnE), no shareability
#define FLAGS_DEVICE 0

// grant a device MMIO page to a task
// maps device_pa into the task's slot right after its user pages
// passes the mapped VA to the task via x0
void proc_grant_device(int pid, uint64_t device_pa) {
    if (pid < 0 || pid >= MAX_PROCS || procs[pid].state == PROC_UNUSED)
        return;

    proc_t *p = &procs[pid];
    uint64_t va = p->slot_base + (uint64_t)p->num_pages * PAGE_SIZE;
    map_page(va, device_pa, FLAGS_DEVICE | AP_USER_RW);
    p->device_va = va;

    // pass the VA to the task as its first argument (x0)
    uint64_t *frame = (uint64_t *)p->saved_sp;
    frame[FRAME_X0] = va;

    print("[proc] granted device ");
    print_hex(device_pa);
    print(" -> ");
    print_hex(va);
    print(" to pid ");
    print_hex(pid);
    print("\n");
}

void proc_unprotect(int pid) {
    if (pid < 0 || pid >= MAX_PROCS || procs[pid].state == PROC_UNUSED)
        return;
    proc_t *p = &procs[pid];
    for (uint32_t i = 0; i < p->num_pages; i++)
        set_page_flags(p->slot_base + i * PAGE_SIZE, FLAGS_NORMAL | AP_USER_RW);
    if (p->device_va)
        set_page_flags(p->device_va, FLAGS_DEVICE | AP_USER_RW);
}

void proc_protect(int pid) {
    if (pid < 0 || pid >= MAX_PROCS || procs[pid].state == PROC_UNUSED)
        return;
    proc_t *p = &procs[pid];
    for (uint32_t i = 0; i < p->num_pages; i++)
        set_page_flags(p->slot_base + i * PAGE_SIZE, FLAGS_NORMAL | AP_KERN_ONLY | UXN);
    if (p->device_va)
        set_page_flags(p->device_va, FLAGS_DEVICE | AP_KERN_ONLY | UXN);
}

void proc_destroy(int pid) {
    if (pid < 0 || pid >= MAX_PROCS || procs[pid].state == PROC_UNUSED)
        return;

    proc_t *p = &procs[pid];
    for (uint32_t i = 0; i < p->num_pages; i++) {
        unmap_page(p->slot_base + i * PAGE_SIZE);
        free_page(p->phys_pages[i]);
    }
    if (p->device_va) {
        unmap_page(p->device_va);
        p->device_va = 0;
    }
    if (p->kstack)
        free_page(p->kstack);

    p->state = PROC_UNUSED;
    print("[proc] destroyed pid ");
    print_hex(pid);
    print("\n");
}

// spawn a new task from a JIT code buffer
// copies a trampoline + code into the task's slot, sets entry to trampoline
// trampoline: bl code; mov x8,#2; svc #0  (auto sys_exit on return)
// device_pa: if non-zero, grants this device MMIO page (e.g. UART)
int proc_spawn(void *code_buf, uint32_t code_len, const char *name, uint64_t device_pa) {
    // need at least 1 page for code + trampoline, plus stack page
    int pid = proc_create(4, (void (*)(void))0);
    if (pid < 0) return -1;

    proc_t *p = &procs[pid];

    // pages are mapped KERN_ONLY, so kernel can write into the slot
    uint32_t *base = (uint32_t *)p->slot_base;

    // trampoline (3 instructions):
    //   bl +3        -> jump to compiled code at base[3]
    //   mov x8, #2   -> SYS_EXIT
    //   svc #0       -> trap to kernel
    // when compiled code does 'ret', x30 = base+4 (after bl), so it hits mov+svc
    base[0] = 0x94000003;  // bl +3
    base[1] = 0xD2800048;  // mov x8, #2
    base[2] = 0xD4000001;  // svc #0

    // copy JIT code after trampoline
    uint32_t *src = (uint32_t *)code_buf;
    uint32_t num_insns = code_len / 4;
    for (uint32_t i = 0; i < num_insns; i++)
        base[3 + i] = src[i];

    // fix entry point: ELR = slot_base (trampoline start)
    uint64_t *frame = (uint64_t *)p->saved_sp;
    frame[3] = p->slot_base;  // FRAME_ELR

    // flush dcache + invalidate icache so CPU sees the new code
    uint32_t total_bytes = (3 + num_insns) * 4;
    for (uint32_t i = 0; i < total_bytes; i += 4) {
        uint64_t va = p->slot_base + i;
        asm volatile("dc cvau, %0" : : "r"(va));
    }
    asm volatile("dsb ish");
    for (uint32_t i = 0; i < total_bytes; i += 4) {
        uint64_t va = p->slot_base + i;
        asm volatile("ic ivau, %0" : : "r"(va));
    }
    asm volatile("dsb ish");
    asm volatile("isb");

    if (device_pa)
        proc_grant_device(pid, device_pa);
    proc_set_name(pid, name);

    return pid;
}

// kill the currently running task
// called from syscall handler on sys_exit or user fault
void proc_exit_current(void) {
    if (current_pid < 0) return;
    proc_destroy(current_pid);
    // dont update current_pid here, schedule will handle it
}

uint64_t proc_get_base(int pid) {
    if (pid < 0 || pid >= MAX_PROCS)
        return 0;
    return procs[pid].slot_base;
}

// round robin scheduler
// saves current task's frame, picks next, returns that task's frame
uint64_t *schedule(uint64_t *frame) {
    if (current_pid >= 0) {
        if (procs[current_pid].state == PROC_RUNNING) {
            // still alive, just preempted
            procs[current_pid].saved_sp = (uint64_t)frame;
            procs[current_pid].state = PROC_READY;
            proc_protect(current_pid);
        }
        // if state is PROC_UNUSED, it was killed (sys_exit), dont save
    } else {
        idle_sp = (uint64_t)frame;
    }

    // find next ready task
    int next = -1;
    int start = (current_pid < 0) ? 0 : current_pid + 1;
    for (int i = 0; i < MAX_PROCS; i++) {
        int idx = (start + i) % MAX_PROCS;
        if (procs[idx].state == PROC_READY) {
            next = idx;
            break;
        }
    }

    if (next < 0) {
        current_pid = -1;
        return (uint64_t *)idle_sp;
    }

    proc_unprotect(next);
    procs[next].state = PROC_RUNNING;
    current_pid = next;

    return (uint64_t *)procs[next].saved_sp;
}

int proc_current_pid_get(void) {
    return current_pid;
}

// find a task that's BLOCKED_SEND targeting this pid
static int find_sender(int target_pid) {
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == PROC_BLOCKED_SEND && procs[i].ipc_target == target_pid)
            return i;
    }
    return -1;
}

// byte-by-byte copy between user VAs (kernel at EL1 can access all mapped pages)
static void ipc_copy(uint64_t dst, uint64_t src, uint32_t len) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    for (uint32_t i = 0; i < len; i++)
        d[i] = s[i];
}

// send a message to target — blocks if target isn't waiting
// returns new frame pointer (may reschedule)
uint64_t *ipc_send(uint64_t *frame, int target, uint64_t msg_ptr, uint32_t len) {
    int sender = current_pid;

    // validate target
    if (target < 0 || target >= MAX_PROCS ||
        procs[target].state == PROC_UNUSED || target == sender) {
        frame[FRAME_X0] = (uint64_t)-1;
        return frame;
    }

    if (procs[target].state == PROC_BLOCKED_RECV) {
        // rendezvous! target is waiting — copy message now
        uint32_t copy_len = len;
        if (copy_len > procs[target].ipc_len)
            copy_len = procs[target].ipc_len;

        ipc_copy(procs[target].ipc_msg, msg_ptr, copy_len);

        // set receiver's return values: x0 = sender pid, x1 = bytes copied
        uint64_t *recv_frame = (uint64_t *)procs[target].saved_sp;
        recv_frame[FRAME_X0] = (uint64_t)sender;
        recv_frame[FRAME_X1] = (uint64_t)copy_len;
        procs[target].state = PROC_READY;

        if (procs[sender].wants_reply) {
            // caller wants a reply — block until server replies
            procs[sender].wants_reply = 0;
            procs[sender].ipc_msg = procs[sender].reply_buf;
            procs[sender].ipc_len = procs[sender].reply_max;
            procs[sender].saved_sp = (uint64_t)frame;
            procs[sender].state = PROC_BLOCKED_REPLY;
            proc_protect(sender);
            return schedule(frame);
        }

        // sender continues immediately
        frame[FRAME_X0] = 0;
        return frame;
    }

    // target not ready — block sender
    procs[sender].ipc_target = target;
    procs[sender].ipc_msg = msg_ptr;
    procs[sender].ipc_len = len;
    procs[sender].saved_sp = (uint64_t)frame;
    procs[sender].state = PROC_BLOCKED_SEND;
    proc_protect(sender);

    return schedule(frame);
}

// receive a message — blocks if nobody is sending to us
// returns new frame pointer (may reschedule)
uint64_t *ipc_recv(uint64_t *frame, uint64_t buf_ptr, uint32_t max_len) {
    int receiver = current_pid;

    // check if anyone is already trying to send to us
    int sender = find_sender(receiver);
    if (sender >= 0) {
        // rendezvous! copy message from blocked sender
        uint32_t copy_len = procs[sender].ipc_len;
        if (copy_len > max_len)
            copy_len = max_len;

        ipc_copy(buf_ptr, procs[sender].ipc_msg, copy_len);

        if (procs[sender].wants_reply) {
            // caller wants a reply — transition to BLOCKED_REPLY
            procs[sender].wants_reply = 0;
            procs[sender].ipc_msg = procs[sender].reply_buf;
            procs[sender].ipc_len = procs[sender].reply_max;
            procs[sender].state = PROC_BLOCKED_REPLY;
        } else {
            // unblock sender: set x0 = 0 (success)
            uint64_t *send_frame = (uint64_t *)procs[sender].saved_sp;
            send_frame[FRAME_X0] = 0;
            procs[sender].state = PROC_READY;
        }

        // receiver continues: x0 = sender pid, x1 = bytes copied
        frame[FRAME_X0] = (uint64_t)sender;
        frame[FRAME_X1] = (uint64_t)copy_len;
        return frame;
    }

    // nobody sending — block receiver
    procs[receiver].ipc_msg = buf_ptr;
    procs[receiver].ipc_len = max_len;
    procs[receiver].saved_sp = (uint64_t)frame;
    procs[receiver].state = PROC_BLOCKED_RECV;
    proc_protect(receiver);

    return schedule(frame);
}

// send a message and block waiting for a reply
uint64_t *ipc_call(uint64_t *frame, int target, uint64_t msg_ptr, uint32_t len,
                   uint64_t reply_buf, uint32_t reply_max) {
    int sender = current_pid;
    procs[sender].wants_reply = 1;
    procs[sender].reply_buf = reply_buf;
    procs[sender].reply_max = reply_max;
    return ipc_send(frame, target, msg_ptr, len);
}

void proc_set_name(int pid, const char *name) {
    if (pid < 0 || pid >= MAX_PROCS)
        return;
    int i;
    for (i = 0; i < 15 && name[i]; i++)
        procs[pid].name[i] = name[i];
    procs[pid].name[i] = '\0';
}

// pack process info into user buffer
// returns bytes written
uint64_t proc_get_info(char *buf, uint64_t max) {
    uint64_t pos = 0;
    for (int i = 0; i < MAX_PROCS; i++) {
        if (procs[i].state == PROC_UNUSED)
            continue;
        if (pos + 20 > max)
            break;
        buf[pos++] = (char)(uint8_t)i;                     // pid
        buf[pos++] = (char)(uint8_t)procs[i].state;        // state
        buf[pos++] = (char)(uint8_t)procs[i].num_pages;    // num_pages
        buf[pos++] = 0;                                     // reserved
        for (int j = 0; j < 16; j++)
            buf[pos++] = procs[i].name[j];
    }
    return pos;
}

// reply to a caller blocked in BLOCKED_REPLY
uint64_t *ipc_reply(uint64_t *frame, int target, uint64_t msg_ptr, uint32_t len) {
    if (target < 0 || target >= MAX_PROCS ||
        procs[target].state != PROC_BLOCKED_REPLY) {
        frame[FRAME_X0] = (uint64_t)-1;
        return frame;
    }

    uint32_t copy_len = len;
    if (copy_len > procs[target].ipc_len)
        copy_len = procs[target].ipc_len;

    // need write access to target's pages to copy reply
    proc_unprotect(target);
    ipc_copy(procs[target].ipc_msg, msg_ptr, copy_len);
    proc_protect(target);

    // set caller's return value: x0 = bytes copied (reply length)
    uint64_t *caller_frame = (uint64_t *)procs[target].saved_sp;
    caller_frame[FRAME_X0] = (uint64_t)copy_len;
    procs[target].state = PROC_READY;

    // replier continues immediately
    frame[FRAME_X0] = 0;
    return frame;
}

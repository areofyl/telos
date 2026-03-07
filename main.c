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
void mmu_init(void);
void proc_init(void);
int proc_create(uint32_t num_pages, void (*entry)(void));
void proc_grant_device(int pid, uint64_t device_pa);

// ---- syscall wrappers for userspace ----
// these use svc to trap into the kernel
// must be static inline so they get compiled into the task functions
// (task code needs to be self-contained since it runs at EL0)

static inline void sys_write(const char *buf) {
    register uint64_t x0 asm("x0") = (uint64_t)buf;
    register uint64_t x1 asm("x1") = 0;
    register uint64_t x8 asm("x8") = 0;  // SYS_WRITE
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
}

static inline void sys_yield(void) {
    register uint64_t x8 asm("x8") = 1;  // SYS_YIELD
    asm volatile("svc #0" : : "r"(x8) : "memory");
}

static inline void sys_exit(void) {
    register uint64_t x8 asm("x8") = 2;  // SYS_EXIT
    asm volatile("svc #0" : : "r"(x8) : "memory");
    for (;;);  // never returns
}

static inline int sys_send(int pid, const char *msg, uint64_t len) {
    register uint64_t x0 asm("x0") = (uint64_t)pid;
    register uint64_t x1 asm("x1") = (uint64_t)msg;
    register uint64_t x2 asm("x2") = len;
    register uint64_t x8 asm("x8") = 3;  // SYS_SEND
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return (int)x0;
}

static inline int sys_recv(char *buf, uint64_t max_len) {
    register uint64_t x0 asm("x0") = (uint64_t)buf;
    register uint64_t x1 asm("x1") = max_len;
    register uint64_t x8 asm("x8") = 4;  // SYS_RECV
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return (int)x0;  // sender pid
}

// sys_recv2 — like sys_recv but also captures x1 (bytes copied)
static inline int sys_recv2(char *buf, uint64_t max_len, uint64_t *out_len) {
    register uint64_t x0 asm("x0") = (uint64_t)buf;
    register uint64_t x1 asm("x1") = max_len;
    register uint64_t x8 asm("x8") = 4;  // SYS_RECV
    asm volatile("svc #0" : "+r"(x0), "+r"(x1) : "r"(x8) : "memory");
    *out_len = x1;
    return (int)x0;  // sender pid
}

static inline int sys_call(int pid, const char *msg, uint64_t len,
                           char *reply_buf, uint64_t reply_max) {
    register uint64_t x0 asm("x0") = (uint64_t)pid;
    register uint64_t x1 asm("x1") = (uint64_t)msg;
    register uint64_t x2 asm("x2") = len;
    register uint64_t x3 asm("x3") = (uint64_t)reply_buf;
    register uint64_t x4 asm("x4") = reply_max;
    register uint64_t x8 asm("x8") = 5;  // SYS_CALL
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8) : "memory");
    return (int)x0;  // reply length
}

static inline int sys_reply(int pid, const char *msg, uint64_t len) {
    register uint64_t x0 asm("x0") = (uint64_t)pid;
    register uint64_t x1 asm("x1") = (uint64_t)msg;
    register uint64_t x2 asm("x2") = len;
    register uint64_t x8 asm("x8") = 6;  // SYS_REPLY
    asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
    return (int)x0;
}

// ---- string utilities ----
// static inline so they compile into EL0 task code

static inline uint64_t u_strlen(const char *s) {
    uint64_t n = 0;
    while (s[n]) n++;
    return n;
}

static inline int u_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static inline void u_strcpy(char *dst, const char *src) {
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static inline void u_memcpy(void *dst, const void *src, uint64_t n) {
    char *d = (char *)dst;
    const char *s = (const char *)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

// ---- nameserver (pid 1) ----

#define NS_PID     1
#define NS_MAX     8
#define NS_NAMELEN 16

void nameserver(void) {
    char names[NS_MAX][NS_NAMELEN];
    int  pids[NS_MAX];
    int  count = 0;

    // zero names
    for (int i = 0; i < NS_MAX; i++) {
        for (int j = 0; j < NS_NAMELEN; j++) names[i][j] = 0;
        pids[i] = -1;
    }

    char buf[64];
    for (;;) {
        uint64_t len;
        int from = sys_recv2(buf, 63, &len);
        buf[len] = '\0';  // ensure last field is null-terminated

        char op = buf[0];
        char *name = &buf[1];

        if (op == 'R') {
            // register
            if (count < NS_MAX) {
                u_strcpy(names[count], name);
                pids[count] = from;
                count++;
            }
            char ok = '\x00';
            sys_reply(from, &ok, 1);
        } else if (op == 'L') {
            // lookup
            int found = -1;
            for (int i = 0; i < count; i++) {
                if (u_streq(names[i], name)) {
                    found = pids[i];
                    break;
                }
            }
            if (found >= 0) {
                char r = (char)found;
                sys_reply(from, &r, 1);
            } else {
                sys_reply(from, "", 0);
            }
        } else {
            char err = '\x01';
            sys_reply(from, &err, 1);
        }
    }
}

// ---- nameserver client wrappers ----

static inline void ns_register(const char *name) {
    char buf[64];
    char reply[4];
    buf[0] = 'R';
    u_strcpy(&buf[1], name);
    sys_call(NS_PID, buf, 1 + u_strlen(name) + 1, reply, 4);
}

static inline int ns_lookup(const char *name) {
    char buf[64];
    char reply[4];
    buf[0] = 'L';
    u_strcpy(&buf[1], name);
    int rlen = sys_call(NS_PID, buf, 1 + u_strlen(name) + 1, reply, 4);
    if (rlen == 0) return -1;
    return (int)(unsigned char)reply[0];
}

static inline int ns_lookup_wait(const char *name) {
    int pid;
    for (;;) {
        pid = ns_lookup(name);
        if (pid >= 0) return pid;
        sys_yield();
    }
}

// ---- FS server (pid 2) ----

#define FS_MAX_FILES 8
#define FS_NAMELEN   16
#define FS_DATALEN   256
#define FS_MAX_TAGS  4
#define FS_KEYLEN    12
#define FS_VALLEN    12

typedef struct {
    char name[FS_NAMELEN];
    char data[FS_DATALEN];
    uint64_t data_len;
    char tag_keys[FS_MAX_TAGS][FS_KEYLEN];
    char tag_vals[FS_MAX_TAGS][FS_VALLEN];
    int  num_tags;
    int  used;
} fs_file_t;

void fs_server(void) {
    fs_file_t files[FS_MAX_FILES];
    for (int i = 0; i < FS_MAX_FILES; i++) {
        files[i].used = 0;
        files[i].data_len = 0;
        files[i].num_tags = 0;
    }

    // register with nameserver
    ns_register("fs");

    char buf[512];
    for (;;) {
        uint64_t len;
        int from = sys_recv2(buf, 511, &len);
        buf[len] = '\0';  // ensure last field is null-terminated

        char op = buf[0];

        if (op == 'C') {
            // Create: C<name>\0<k1>\0<v1>\0<k2>\0<v2>\0...
            char *name = &buf[1];

            // find free slot
            int slot = -1;
            for (int i = 0; i < FS_MAX_FILES; i++) {
                if (!files[i].used) { slot = i; break; }
            }
            if (slot < 0) {
                char err = '\x01';
                sys_reply(from, &err, 1);
                continue;
            }

            fs_file_t *f = &files[slot];
            f->used = 1;
            f->data_len = 0;
            f->num_tags = 0;
            u_strcpy(f->name, name);
            for (int i = 0; i < FS_DATALEN; i++) f->data[i] = 0;

            // parse tags: skip past name null, then key\0val\0 pairs
            uint64_t pos = 1 + u_strlen(name) + 1; // skip 'C', name, null
            while (pos < len && f->num_tags < FS_MAX_TAGS) {
                char *key = &buf[pos];
                uint64_t klen = u_strlen(key);
                if (klen == 0) break;
                pos += klen + 1;
                if (pos >= len) break;
                char *val = &buf[pos];
                uint64_t vlen = u_strlen(val);
                pos += vlen + 1;

                u_strcpy(f->tag_keys[f->num_tags], key);
                u_strcpy(f->tag_vals[f->num_tags], val);
                f->num_tags++;
            }

            char ok = '\x00';
            sys_reply(from, &ok, 1);

        } else if (op == 'W') {
            // Write: W<name>\0<data bytes>
            char *name = &buf[1];
            uint64_t nlen = u_strlen(name);
            uint64_t data_start = 1 + nlen + 1;
            uint64_t data_len = len - data_start;

            int found = -1;
            for (int i = 0; i < FS_MAX_FILES; i++) {
                if (files[i].used && u_streq(files[i].name, name)) {
                    found = i; break;
                }
            }
            if (found < 0) {
                char err = '\x01';
                sys_reply(from, &err, 1);
                continue;
            }

            if (data_len > FS_DATALEN) data_len = FS_DATALEN;
            u_memcpy(files[found].data, &buf[data_start], data_len);
            files[found].data_len = data_len;

            char ok = '\x00';
            sys_reply(from, &ok, 1);

        } else if (op == 'R') {
            // Read: R<name>
            char *name = &buf[1];

            int found = -1;
            for (int i = 0; i < FS_MAX_FILES; i++) {
                if (files[i].used && u_streq(files[i].name, name)) {
                    found = i; break;
                }
            }
            if (found < 0) {
                sys_reply(from, "", 0);
            } else {
                sys_reply(from, files[found].data, files[found].data_len);
            }

        } else if (op == 'T') {
            // Tag: T<name>\0<key>\0<val>
            char *name = &buf[1];
            uint64_t nlen = u_strlen(name);
            uint64_t pos = 1 + nlen + 1;
            char *key = &buf[pos];
            uint64_t klen = u_strlen(key);
            pos += klen + 1;
            char *val = &buf[pos];

            int found = -1;
            for (int i = 0; i < FS_MAX_FILES; i++) {
                if (files[i].used && u_streq(files[i].name, name)) {
                    found = i; break;
                }
            }
            if (found < 0) {
                char err = '\x01';
                sys_reply(from, &err, 1);
                continue;
            }

            fs_file_t *f = &files[found];
            // update existing key or add new
            int tag_idx = -1;
            for (int i = 0; i < f->num_tags; i++) {
                if (u_streq(f->tag_keys[i], key)) {
                    tag_idx = i; break;
                }
            }
            if (tag_idx >= 0) {
                u_strcpy(f->tag_vals[tag_idx], val);
            } else if (f->num_tags < FS_MAX_TAGS) {
                u_strcpy(f->tag_keys[f->num_tags], key);
                u_strcpy(f->tag_vals[f->num_tags], val);
                f->num_tags++;
            }

            char ok = '\x00';
            sys_reply(from, &ok, 1);

        } else if (op == 'Q') {
            // Query: Q<key>\0<val>
            char *key = &buf[1];
            uint64_t klen = u_strlen(key);
            char *val = &buf[1 + klen + 1];

            char reply[256];
            uint64_t rpos = 0;

            for (int i = 0; i < FS_MAX_FILES; i++) {
                if (!files[i].used) continue;
                for (int t = 0; t < files[i].num_tags; t++) {
                    if (u_streq(files[i].tag_keys[t], key) &&
                        u_streq(files[i].tag_vals[t], val)) {
                        // append name + null
                        uint64_t nlen = u_strlen(files[i].name);
                        if (rpos + nlen + 1 <= 256) {
                            u_memcpy(&reply[rpos], files[i].name, nlen);
                            rpos += nlen;
                            reply[rpos++] = '\0';
                        }
                        break;
                    }
                }
            }

            sys_reply(from, reply, rpos);

        } else if (op == 'L') {
            // List all files: L
            char reply[256];
            uint64_t rpos = 0;

            for (int i = 0; i < FS_MAX_FILES; i++) {
                if (!files[i].used) continue;
                uint64_t nlen = u_strlen(files[i].name);
                if (rpos + nlen + 1 <= 256) {
                    u_memcpy(&reply[rpos], files[i].name, nlen);
                    rpos += nlen;
                    reply[rpos++] = '\0';
                }
            }

            sys_reply(from, reply, rpos);

        } else if (op == 'G') {
            // Get tags: G<name>\0
            char *name = &buf[1];

            int found = -1;
            for (int i = 0; i < FS_MAX_FILES; i++) {
                if (files[i].used && u_streq(files[i].name, name)) {
                    found = i; break;
                }
            }
            if (found < 0) {
                sys_reply(from, "", 0);
            } else {
                char reply[256];
                uint64_t rpos = 0;
                fs_file_t *f = &files[found];
                for (int t = 0; t < f->num_tags; t++) {
                    uint64_t klen = u_strlen(f->tag_keys[t]);
                    uint64_t vlen = u_strlen(f->tag_vals[t]);
                    if (rpos + klen + 1 + vlen + 1 <= 256) {
                        u_memcpy(&reply[rpos], f->tag_keys[t], klen);
                        rpos += klen;
                        reply[rpos++] = '\0';
                        u_memcpy(&reply[rpos], f->tag_vals[t], vlen);
                        rpos += vlen;
                        reply[rpos++] = '\0';
                    }
                }
                sys_reply(from, reply, rpos);
            }

        } else {
            char err = '\x01';
            sys_reply(from, &err, 1);
        }
    }
}

// ---- FS client wrappers ----

static inline void fs_write(int fs_pid, const char *name, const char *data, uint64_t data_len) {
    char buf[512];
    char reply[4];
    buf[0] = 'W';
    u_strcpy(&buf[1], name);
    uint64_t nlen = u_strlen(name);
    uint64_t off = 1 + nlen + 1; // 'W' + name + null
    u_memcpy(&buf[off], data, data_len);
    sys_call(fs_pid, buf, off + data_len, reply, 4);
}

static inline int fs_read(int fs_pid, const char *name, char *out, uint64_t max) {
    char buf[64];
    buf[0] = 'R';
    u_strcpy(&buf[1], name);
    return sys_call(fs_pid, buf, 1 + u_strlen(name) + 1, out, max);
}

static inline void fs_tag(int fs_pid, const char *name, const char *key, const char *val) {
    char buf[128];
    char reply[4];
    buf[0] = 'T';
    u_strcpy(&buf[1], name);
    uint64_t pos = 1 + u_strlen(name) + 1;
    u_strcpy(&buf[pos], key);
    pos += u_strlen(key) + 1;
    u_strcpy(&buf[pos], val);
    pos += u_strlen(val) + 1;
    sys_call(fs_pid, buf, pos, reply, 4);
}

static inline int fs_query(int fs_pid, const char *key, const char *val, char *out, uint64_t max) {
    char buf[64];
    buf[0] = 'Q';
    u_strcpy(&buf[1], key);
    uint64_t pos = 1 + u_strlen(key) + 1;
    u_strcpy(&buf[pos], val);
    pos += u_strlen(val) + 1;
    return sys_call(fs_pid, buf, pos, out, max);
}

static inline int fs_list(int fs_pid, char *out, uint64_t max) {
    char buf[4];
    buf[0] = 'L';
    return sys_call(fs_pid, buf, 1, out, max);
}

static inline int fs_tags(int fs_pid, const char *name, char *out, uint64_t max) {
    char buf[64];
    buf[0] = 'G';
    u_strcpy(&buf[1], name);
    return sys_call(fs_pid, buf, 1 + u_strlen(name) + 1, out, max);
}

// ---- string builder + IPC print helpers ----

static inline void u_strcat(char *dst, const char *src) {
    while (*dst) dst++;
    while (*src) *dst++ = *src++;
    *dst = '\0';
}

static inline void ipc_print(int uart_pid, const char *msg) {
    char reply[4];
    sys_call(uart_pid, msg, u_strlen(msg) + 1, reply, 4);
}

// ---- UART server (pid 0) ----

void uart_server(uint64_t uart_base) {
    volatile uint8_t *uart = (volatile uint8_t *)uart_base;

    // register with nameserver
    ns_register("uart");

    char buf[256];
    for (;;) {
        uint64_t len;
        int from = sys_recv2(buf, 256, &len);
        for (uint64_t i = 0; i < len; i++) {
            if (buf[i] == '\0') break;
            *uart = buf[i];
        }
        sys_reply(from, "ok", 3);
    }
}

// ---- demo tasks ----

void task_a(void) {
    int uart_pid = ns_lookup_wait("uart");
    int fs_pid   = ns_lookup_wait("fs");

    ipc_print(uart_pid, "[A] started, found uart and fs\n");

    // create notes.txt with tags type=text, owner=taskA
    {
        char buf[128];
        buf[0] = 'C';
        u_strcpy(&buf[1], "notes.txt");
        uint64_t pos = 1 + 9 + 1; // 'C' + "notes.txt" + null
        u_strcpy(&buf[pos], "type");  pos += 5;
        u_strcpy(&buf[pos], "text");  pos += 5;
        u_strcpy(&buf[pos], "owner"); pos += 6;
        u_strcpy(&buf[pos], "taskA"); pos += 5;
        char reply[4];
        sys_call(fs_pid, buf, pos, reply, 4);
    }
    ipc_print(uart_pid, "[A] created notes.txt (type=text, owner=taskA)\n");

    // write data
    fs_write(fs_pid, "notes.txt", "hello from task A!", 18);
    ipc_print(uart_pid, "[A] wrote data to notes.txt\n");

    // read back
    {
        char data[256];
        char line[256];
        int rlen = fs_read(fs_pid, "notes.txt", data, 255);
        data[rlen] = '\0';
        u_strcpy(line, "[A] read back: ");
        u_strcat(line, data);
        u_strcat(line, "\n");
        ipc_print(uart_pid, line);
    }

    // query type=text
    {
        char results[256];
        char line[256];
        int rlen = fs_query(fs_pid, "type", "text", results, 256);
        u_strcpy(line, "[A] query type=text: ");
        uint64_t pos = 0;
        while (pos < (uint64_t)rlen) {
            u_strcat(line, &results[pos]);
            pos += u_strlen(&results[pos]) + 1;
            if (pos < (uint64_t)rlen) u_strcat(line, " ");
        }
        u_strcat(line, "\n");
        ipc_print(uart_pid, line);
    }

    ipc_print(uart_pid, "[A] done!\n");
    sys_exit();
}

void task_b(void) {
    int uart_pid = ns_lookup_wait("uart");
    int fs_pid   = ns_lookup_wait("fs");

    // delay so task_a finishes first
    for (volatile int i = 0; i < 5000000; i++);

    ipc_print(uart_pid, "[B] started, found uart and fs\n");

    // create log.txt with tags type=text, owner=taskB
    {
        char buf[128];
        buf[0] = 'C';
        u_strcpy(&buf[1], "log.txt");
        uint64_t pos = 1 + 7 + 1; // 'C' + "log.txt" + null
        u_strcpy(&buf[pos], "type");  pos += 5;
        u_strcpy(&buf[pos], "text");  pos += 5;
        u_strcpy(&buf[pos], "owner"); pos += 6;
        u_strcpy(&buf[pos], "taskB"); pos += 5;
        char reply[4];
        sys_call(fs_pid, buf, pos, reply, 4);
    }
    ipc_print(uart_pid, "[B] created log.txt (type=text, owner=taskB)\n");

    // write data
    fs_write(fs_pid, "log.txt", "log entry from B", 16);
    ipc_print(uart_pid, "[B] wrote data to log.txt\n");

    // query type=text — should find both
    {
        char results[256];
        char line[256];
        int rlen = fs_query(fs_pid, "type", "text", results, 256);
        u_strcpy(line, "[B] query type=text: ");
        uint64_t pos = 0;
        while (pos < (uint64_t)rlen) {
            u_strcat(line, &results[pos]);
            pos += u_strlen(&results[pos]) + 1;
            if (pos < (uint64_t)rlen) u_strcat(line, " ");
        }
        u_strcat(line, "\n");
        ipc_print(uart_pid, line);
    }

    // query owner=taskA — should find only notes.txt
    {
        char results[256];
        char line[256];
        int rlen = fs_query(fs_pid, "owner", "taskA", results, 256);
        u_strcpy(line, "[B] query owner=taskA: ");
        uint64_t pos = 0;
        while (pos < (uint64_t)rlen) {
            u_strcat(line, &results[pos]);
            pos += u_strlen(&results[pos]) + 1;
            if (pos < (uint64_t)rlen) u_strcat(line, " ");
        }
        u_strcat(line, "\n");
        ipc_print(uart_pid, line);
    }

    // read task A's file (cross-task access)
    {
        char data[256];
        char line[256];
        int rlen = fs_read(fs_pid, "notes.txt", data, 255);
        data[rlen] = '\0';
        u_strcpy(line, "[B] read A's file: ");
        u_strcat(line, data);
        u_strcat(line, "\n");
        ipc_print(uart_pid, line);
    }

    ipc_print(uart_pid, "[B] done!\n");
    sys_exit();
}

// ---- FS create helper (simple, no initial tags) ----

static inline void fs_create(int fs_pid, const char *name) {
    char buf[64];
    char reply[4];
    buf[0] = 'C';
    u_strcpy(&buf[1], name);
    sys_call(fs_pid, buf, 1 + u_strlen(name) + 1, reply, 4);
}

// ---- shell task ----

static inline char shell_getc(volatile uint8_t *u) {
    while (u[0x18] & (1 << 4));  // poll RXFE until data ready
    return u[0x00];
}

static inline void shell_putc(volatile uint8_t *u, char c) {
    while (u[0x18] & (1 << 5));  // poll TXFF until space
    u[0x00] = c;
}

static inline void shell_puts(volatile uint8_t *u, const char *s) {
    while (*s) shell_putc(u, *s++);
}

static inline void shell_put_int(volatile uint8_t *u, int n) {
    if (n == 0) { shell_putc(u, '0'); return; }
    char buf[12];
    int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) shell_putc(u, buf[--i]);
}

void shell_task(uint64_t uart_base) {
    volatile uint8_t *u = (volatile uint8_t *)uart_base;

    int fs_pid = ns_lookup_wait("fs");

    // wait for demo tasks to finish
    for (volatile int i = 0; i < 10000000; i++);

    // tutorial
    shell_puts(u, "\n");
    shell_puts(u, "========================================\n");
    shell_puts(u, "  Welcome to the telOS shell!\n");
    shell_puts(u, "========================================\n");
    shell_puts(u, "\n");
    shell_puts(u, "telOS uses a tag-based filesystem:\n");
    shell_puts(u, "  - Files have key:value tags instead of directories\n");
    shell_puts(u, "  - Use 'tag <file> <key> <val>' to label files\n");
    shell_puts(u, "  - Use 'query <key> <val>' to find matching files\n");
    shell_puts(u, "\n");
    shell_puts(u, "Example workflow:\n");
    shell_puts(u, "  telos> create hello.txt\n");
    shell_puts(u, "  telos> write hello.txt Hello World!\n");
    shell_puts(u, "  telos> tag hello.txt type text\n");
    shell_puts(u, "  telos> tag hello.txt topic greeting\n");
    shell_puts(u, "  telos> query type text\n");
    shell_puts(u, "  telos> tags hello.txt\n");
    shell_puts(u, "  telos> cat hello.txt\n");
    shell_puts(u, "\n");
    shell_puts(u, "Type 'help' for available commands.\n\n");

    char cmd_buf[128];
    for (;;) {
        shell_puts(u, "telos> ");

        // read line
        int pos = 0;
        for (;;) {
            char c = shell_getc(u);
            if (c == '\r' || c == '\n') {
                shell_puts(u, "\n");
                break;
            }
            if (c == 0x7f || c == 0x08) {
                // backspace
                if (pos > 0) {
                    pos--;
                    shell_puts(u, "\b \b");
                }
                continue;
            }
            if (pos < 127) {
                cmd_buf[pos++] = c;
                shell_putc(u, c);  // echo
            }
        }
        cmd_buf[pos] = '\0';

        if (pos == 0) continue;

        // parse: split into argv[0..argc-1]
        char *argv[8];
        int argc = 0;
        char *p = cmd_buf;
        while (*p && argc < 8) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
        if (argc == 0) continue;

        if (u_streq(argv[0], "help")) {
            shell_puts(u, "Commands:\n");
            shell_puts(u, "  ls              - list all files\n");
            shell_puts(u, "  cat <file>      - read file contents\n");
            shell_puts(u, "  create <name>   - create empty file\n");
            shell_puts(u, "  write <n> <data> - write text to file\n");
            shell_puts(u, "  tag <n> <k> <v> - add/update tag\n");
            shell_puts(u, "  query <k> <v>   - find files by tag\n");
            shell_puts(u, "  tags <file>     - show file's tags\n");
            shell_puts(u, "  help            - this message\n");

        } else if (u_streq(argv[0], "ls")) {
            char out[256];
            int rlen = fs_list(fs_pid, out, 256);
            if (rlen == 0) {
                shell_puts(u, "(no files)\n");
            } else {
                uint64_t p = 0;
                while (p < (uint64_t)rlen) {
                    shell_puts(u, &out[p]);
                    shell_puts(u, "\n");
                    p += u_strlen(&out[p]) + 1;
                }
            }

        } else if (u_streq(argv[0], "cat")) {
            if (argc < 2) {
                shell_puts(u, "usage: cat <file>\n");
            } else {
                char data[256];
                int rlen = fs_read(fs_pid, argv[1], data, 255);
                if (rlen == 0) {
                    shell_puts(u, "file not found or empty\n");
                } else {
                    data[rlen] = '\0';
                    shell_puts(u, data);
                    shell_puts(u, "\n");
                }
            }

        } else if (u_streq(argv[0], "create")) {
            if (argc < 2) {
                shell_puts(u, "usage: create <name>\n");
            } else {
                fs_create(fs_pid, argv[1]);
                shell_puts(u, "created ");
                shell_puts(u, argv[1]);
                shell_puts(u, "\n");
            }

        } else if (u_streq(argv[0], "write")) {
            if (argc < 3) {
                shell_puts(u, "usage: write <name> <data...>\n");
            } else {
                // reconstruct data from argv[2..] with spaces
                char data[256];
                data[0] = '\0';
                for (int i = 2; i < argc; i++) {
                    u_strcat(data, argv[i]);
                    if (i < argc - 1) u_strcat(data, " ");
                }
                fs_write(fs_pid, argv[1], data, u_strlen(data));
                shell_puts(u, "wrote ");
                shell_put_int(u, (int)u_strlen(data));
                shell_puts(u, " bytes to ");
                shell_puts(u, argv[1]);
                shell_puts(u, "\n");
            }

        } else if (u_streq(argv[0], "tag")) {
            if (argc < 4) {
                shell_puts(u, "usage: tag <name> <key> <val>\n");
            } else {
                fs_tag(fs_pid, argv[1], argv[2], argv[3]);
                shell_puts(u, "tagged ");
                shell_puts(u, argv[1]);
                shell_puts(u, " ");
                shell_puts(u, argv[2]);
                shell_puts(u, "=");
                shell_puts(u, argv[3]);
                shell_puts(u, "\n");
            }

        } else if (u_streq(argv[0], "query")) {
            if (argc < 3) {
                shell_puts(u, "usage: query <key> <val>\n");
            } else {
                char out[256];
                int rlen = fs_query(fs_pid, argv[1], argv[2], out, 256);
                if (rlen == 0) {
                    shell_puts(u, "(no matches)\n");
                } else {
                    uint64_t p = 0;
                    while (p < (uint64_t)rlen) {
                        shell_puts(u, &out[p]);
                        shell_puts(u, "\n");
                        p += u_strlen(&out[p]) + 1;
                    }
                }
            }

        } else if (u_streq(argv[0], "tags")) {
            if (argc < 2) {
                shell_puts(u, "usage: tags <file>\n");
            } else {
                char out[256];
                int rlen = fs_tags(fs_pid, argv[1], out, 256);
                if (rlen == 0) {
                    shell_puts(u, "file not found or no tags\n");
                } else {
                    uint64_t p = 0;
                    while (p < (uint64_t)rlen) {
                        char *key = &out[p];
                        p += u_strlen(key) + 1;
                        char *val = &out[p];
                        p += u_strlen(val) + 1;
                        shell_puts(u, "  ");
                        shell_puts(u, key);
                        shell_puts(u, " = ");
                        shell_puts(u, val);
                        shell_puts(u, "\n");
                    }
                }
            }

        } else {
            shell_puts(u, "unknown command: ");
            shell_puts(u, argv[0]);
            shell_puts(u, "\ntype 'help' for commands\n");
        }
    }
}

// ---- entry point ----

void main() {
  print("hello from telOS\n\n");

  print("setting up interrupts\n");
  gic_init();
  print("\n");
  timer_init();
  print("\n");

  print("setting up memory\n");
  pmm_init();
  print("\n");

  print("setting up mmu\n");
  mmu_init();
  print("\n");

  proc_init();
  print("\n");

  print("creating tasks\n");
  int uart_pid = proc_create(4, (void(*)(void))uart_server);
  proc_grant_device(uart_pid, 0x09000000);
  proc_create(4, (void(*)(void))nameserver);
  proc_create(4, (void(*)(void))fs_server);
  proc_create(4, task_a);
  proc_create(4, task_b);
  int shell_pid = proc_create(4, (void(*)(void))shell_task);
  proc_grant_device(shell_pid, 0x09000000);
  print("\n");

  asm volatile("msr daifclr, #2");
  print("[cpu] IRQs unmasked, tasks will start on first tick\n\n");

  for (;;) {
    asm volatile("wfe");
  }
}

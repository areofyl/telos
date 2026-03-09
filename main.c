#include <stdint.h>

// uart
// RPi 5 RP1 UART0 (GPIO 14/15 on 40-pin header, via PCIe)
#define UART_BASE       0x1F00030000UL
#define UART_DR         (*(volatile uint32_t *)(UART_BASE + 0x00))
#define UART_FR         (*(volatile uint32_t *)(UART_BASE + 0x18))
#define UART_IBRD       (*(volatile uint32_t *)(UART_BASE + 0x24))
#define UART_FBRD       (*(volatile uint32_t *)(UART_BASE + 0x28))
#define UART_LCRH       (*(volatile uint32_t *)(UART_BASE + 0x2C))
#define UART_CR         (*(volatile uint32_t *)(UART_BASE + 0x30))
#define UART_IMSC       (*(volatile uint32_t *)(UART_BASE + 0x38))
#define UART_ICR        (*(volatile uint32_t *)(UART_BASE + 0x44))

#define FR_TXFF         (1 << 5)

// keep old pointer for compatibility with rest of code
volatile uint8_t *uart = (volatile uint8_t *)UART_BASE;

void uart_init(void) {
  // disable UART
  UART_CR = 0;

  // clear pending interrupts
  UART_ICR = 0x7FF;

  // set baud rate: 57600 with 50MHz clock
  // IBRD = 50000000 / (16 * 57600) = 54
  // FBRD = round(0.253 * 64) = 16
  UART_IBRD = 54;
  UART_FBRD = 16;

  // 8 bits, no parity, 1 stop bit, enable FIFOs
  UART_LCRH = (3 << 5) | (1 << 4);  // WLEN8 | FEN

  // disable interrupts
  UART_IMSC = 0;

  // enable UART, TX, RX
  UART_CR = (1 << 0) | (1 << 8) | (1 << 9);  // UARTEN | TXE | RXE
}

void putchar(char c) {
  // wait until TX FIFO is not full
  while (UART_FR & FR_TXFF);
  UART_DR = c;
}

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
void proc_set_name(int pid, const char *name);

// ---- syscall wrappers for userspace ----
// these use svc to trap into the kernel
// must be static inline so they get compiled into the task functions
// (task code needs to be self-contained since it runs at EL0)

static inline void sys_write(const char *buf) {
  register uint64_t x0 asm("x0") = (uint64_t)buf;
  register uint64_t x1 asm("x1") = 0;
  register uint64_t x8 asm("x8") = 0; // SYS_WRITE
  asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
}

static inline void sys_yield(void) {
  register uint64_t x8 asm("x8") = 1; // SYS_YIELD
  asm volatile("svc #0" : : "r"(x8) : "memory");
}

static inline void sys_exit(void) {
  register uint64_t x8 asm("x8") = 2; // SYS_EXIT
  asm volatile("svc #0" : : "r"(x8) : "memory");
  for (;;)
    ; // never returns
}

static inline int sys_send(int pid, const char *msg, uint64_t len) {
  register uint64_t x0 asm("x0") = (uint64_t)pid;
  register uint64_t x1 asm("x1") = (uint64_t)msg;
  register uint64_t x2 asm("x2") = len;
  register uint64_t x8 asm("x8") = 3; // SYS_SEND
  asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return (int)x0;
}

static inline int sys_recv(char *buf, uint64_t max_len) {
  register uint64_t x0 asm("x0") = (uint64_t)buf;
  register uint64_t x1 asm("x1") = max_len;
  register uint64_t x8 asm("x8") = 4; // SYS_RECV
  asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  return (int)x0; // sender pid
}

// sys_recv2 — like sys_recv but also captures x1 (bytes copied)
static inline int sys_recv2(char *buf, uint64_t max_len, uint64_t *out_len) {
  register uint64_t x0 asm("x0") = (uint64_t)buf;
  register uint64_t x1 asm("x1") = max_len;
  register uint64_t x8 asm("x8") = 4; // SYS_RECV
  asm volatile("svc #0" : "+r"(x0), "+r"(x1) : "r"(x8) : "memory");
  *out_len = x1;
  return (int)x0; // sender pid
}

static inline int sys_call(int pid, const char *msg, uint64_t len,
                           char *reply_buf, uint64_t reply_max) {
  register uint64_t x0 asm("x0") = (uint64_t)pid;
  register uint64_t x1 asm("x1") = (uint64_t)msg;
  register uint64_t x2 asm("x2") = len;
  register uint64_t x3 asm("x3") = (uint64_t)reply_buf;
  register uint64_t x4 asm("x4") = reply_max;
  register uint64_t x8 asm("x8") = 5; // SYS_CALL
  asm volatile("svc #0"
               : "+r"(x0)
               : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x8)
               : "memory");
  return (int)x0; // reply length
}

static inline void sys_cacheflush(void *addr, uint64_t len) {
  register uint64_t x0 asm("x0") = (uint64_t)addr;
  register uint64_t x1 asm("x1") = len;
  register uint64_t x8 asm("x8") = 8; // SYS_CACHEFLUSH
  asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
}

static inline int sys_procinfo(char *buf, uint64_t max) {
  register uint64_t x0 asm("x0") = (uint64_t)buf;
  register uint64_t x1 asm("x1") = max;
  register uint64_t x8 asm("x8") = 7; // SYS_PROCINFO
  asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  return (int)x0;
}

static inline int sys_reply(int pid, const char *msg, uint64_t len) {
  register uint64_t x0 asm("x0") = (uint64_t)pid;
  register uint64_t x1 asm("x1") = (uint64_t)msg;
  register uint64_t x2 asm("x2") = len;
  register uint64_t x8 asm("x8") = 6; // SYS_REPLY
  asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return (int)x0;
}

static inline int sys_spawn(void *code_buf, uint32_t code_len, const char *name) {
  register uint64_t x0 asm("x0") = (uint64_t)code_buf;
  register uint64_t x1 asm("x1") = (uint64_t)code_len;
  register uint64_t x2 asm("x2") = (uint64_t)name;
  register uint64_t x8 asm("x8") = 9; // SYS_SPAWN
  asm volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return (int)x0;
}

// ---- string utilities ----
// static inline so they compile into EL0 task code

static inline uint64_t u_strlen(const char *s) {
  uint64_t n = 0;
  while (s[n])
    n++;
  return n;
}

static inline int u_streq(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b)
      return 0;
    a++;
    b++;
  }
  return *a == *b;
}

static inline void u_strcpy(char *dst, const char *src) {
  while (*src)
    *dst++ = *src++;
  *dst = '\0';
}

static inline void u_memcpy(void *dst, const void *src, uint64_t n) {
  char *d = (char *)dst;
  const char *s = (const char *)src;
  for (uint64_t i = 0; i < n; i++)
    d[i] = s[i];
}

// ---- nameserver (pid 1) ----

#define NS_PID 1
#define NS_MAX 8
#define NS_NAMELEN 16

void nameserver(void) {
  char names[NS_MAX][NS_NAMELEN];
  int pids[NS_MAX];
  int count = 0;

  // zero names
  for (int i = 0; i < NS_MAX; i++) {
    for (int j = 0; j < NS_NAMELEN; j++)
      names[i][j] = 0;
    pids[i] = -1;
  }

  char buf[64];
  for (;;) {
    uint64_t len;
    int from = sys_recv2(buf, 63, &len);
    buf[len] = '\0'; // ensure last field is null-terminated

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
  if (rlen == 0)
    return -1;
  return (int)(unsigned char)reply[0];
}

static inline int ns_lookup_wait(const char *name) {
  int pid;
  for (;;) {
    pid = ns_lookup(name);
    if (pid >= 0)
      return pid;
    sys_yield();
  }
}

// ---- FS server (pid 2) ----

#define FS_MAX_FILES 8
#define FS_NAMELEN 16
#define FS_DATALEN 1024
#define FS_MAX_TAGS 4
#define FS_KEYLEN 12
#define FS_VALLEN 12

typedef struct {
  char name[FS_NAMELEN];
  char data[FS_DATALEN];
  uint64_t data_len;
  char tag_keys[FS_MAX_TAGS][FS_KEYLEN];
  char tag_vals[FS_MAX_TAGS][FS_VALLEN];
  int num_tags;
  int used;
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
    buf[len] = '\0'; // ensure last field is null-terminated

    char op = buf[0];

    if (op == 'C') {
      // Create: C<name>\0<k1>\0<v1>\0<k2>\0<v2>\0...
      char *name = &buf[1];

      // find free slot
      int slot = -1;
      for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!files[i].used) {
          slot = i;
          break;
        }
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
      for (int i = 0; i < FS_DATALEN; i++)
        f->data[i] = 0;

      // parse tags: skip past name null, then key\0val\0 pairs
      uint64_t pos = 1 + u_strlen(name) + 1; // skip 'C', name, null
      while (pos < len && f->num_tags < FS_MAX_TAGS) {
        char *key = &buf[pos];
        uint64_t klen = u_strlen(key);
        if (klen == 0)
          break;
        pos += klen + 1;
        if (pos >= len)
          break;
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
          found = i;
          break;
        }
      }
      if (found < 0) {
        char err = '\x01';
        sys_reply(from, &err, 1);
        continue;
      }

      if (data_len > FS_DATALEN)
        data_len = FS_DATALEN;
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
          found = i;
          break;
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
          found = i;
          break;
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
          tag_idx = i;
          break;
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
        if (!files[i].used)
          continue;
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
        if (!files[i].used)
          continue;
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
          found = i;
          break;
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

static inline void fs_write(int fs_pid, const char *name, const char *data,
                            uint64_t data_len) {
  char buf[512];
  char reply[4];
  buf[0] = 'W';
  u_strcpy(&buf[1], name);
  uint64_t nlen = u_strlen(name);
  uint64_t off = 1 + nlen + 1; // 'W' + name + null
  u_memcpy(&buf[off], data, data_len);
  sys_call(fs_pid, buf, off + data_len, reply, 4);
}

static inline int fs_read(int fs_pid, const char *name, char *out,
                          uint64_t max) {
  char buf[64];
  buf[0] = 'R';
  u_strcpy(&buf[1], name);
  return sys_call(fs_pid, buf, 1 + u_strlen(name) + 1, out, max);
}

static inline void fs_tag(int fs_pid, const char *name, const char *key,
                          const char *val) {
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

static inline int fs_query(int fs_pid, const char *key, const char *val,
                           char *out, uint64_t max) {
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

static inline int fs_tags(int fs_pid, const char *name, char *out,
                          uint64_t max) {
  char buf[64];
  buf[0] = 'G';
  u_strcpy(&buf[1], name);
  return sys_call(fs_pid, buf, 1 + u_strlen(name) + 1, out, max);
}

// ---- string builder + IPC print helpers ----

static inline void u_strcat(char *dst, const char *src) {
  while (*dst)
    dst++;
  while (*src)
    *dst++ = *src++;
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
      if (buf[i] == '\0')
        break;
      *uart = buf[i];
    }
    sys_reply(from, "ok", 3);
  }
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
  while (u[0x18] & (1 << 4))
    ; // poll RXFE until data ready
  return u[0x00];
}

static inline void shell_putc(volatile uint8_t *u, char c) {
  while (u[0x18] & (1 << 5))
    ; // poll TXFF until space
  u[0x00] = c;
}

static inline void shell_puts(volatile uint8_t *u, const char *s) {
  while (*s)
    shell_putc(u, *s++);
}

static inline void shell_put_int(volatile uint8_t *u, int n) {
  if (n == 0) {
    shell_putc(u, '0');
    return;
  }
  char buf[12];
  int i = 0;
  while (n > 0) {
    buf[i++] = '0' + (n % 10);
    n /= 10;
  }
  while (i > 0)
    shell_putc(u, buf[--i]);
}

// ---- teled: simple text editor ----

static void teled_redraw(volatile uint8_t *u, const char *name, char *buf,
                         int len) {
  shell_puts(u, "\033[2J\033[H"); // clear screen
  shell_puts(u, "--- teled: ");
  shell_puts(u, name);
  shell_puts(u, " --- Ctrl+S save | Ctrl+Q quit ---\n");
  shell_puts(u, "----------------------------------------------\n");
  // print buffer contents
  for (int i = 0; i < len; i++)
    shell_putc(u, buf[i]);
}

static void teled(volatile uint8_t *u, int fs_pid, const char *name) {
  char buf[1024];
  int len = 0;

  // try to load existing file contents
  int rlen = fs_read(fs_pid, name, buf, 1023);
  if (rlen > 0)
    len = rlen;

  teled_redraw(u, name, buf, len);

  for (;;) {
    char c = shell_getc(u);

    if (c == 0x13) { // Ctrl+S — save and exit
      // create file if it doesn't exist, then write
      fs_create(fs_pid, name);
      fs_write(fs_pid, name, buf, len);
      shell_puts(u, "\033[2J\033[H");
      shell_puts(u, "saved ");
      shell_puts(u, name);
      shell_puts(u, " (");
      shell_put_int(u, len);
      shell_puts(u, " bytes)\n");
      return;
    }

    if (c == 0x11) { // Ctrl+Q — quit without saving
      shell_puts(u, "\033[2J\033[H");
      shell_puts(u, "quit without saving\n");
      return;
    }

    if (c == 0x7f || c == 0x08) { // backspace
      if (len > 0) {
        len--;
        if (buf[len] == '\n') {
          // redraw needed when deleting a newline
          teled_redraw(u, name, buf, len);
        } else {
          shell_puts(u, "\b \b");
        }
      }
      continue;
    }

    if (c == '\r' || c == '\n') {
      if (len < 1023) {
        buf[len++] = '\n';
        shell_puts(u, "\n");
      }
      continue;
    }

    // printable characters
    if (c >= 0x20 && len < 1023) {
      buf[len++] = c;
      shell_putc(u, c);
    }
  }
}

// ---- tcc: tiny C compiler (JIT) ----
// compiles a subset of C to aarch64 machine code and runs it
//
// supported:
//   int variables (local only), if/else, while, return
//   putc(expr), getc(), arithmetic, comparisons, char/int literals
//   functions: only main() is compiled and executed
//
// codegen: single-pass recursive descent, emits into a buffer
//   x19 = UART base (preserved), expression result always in x0
//   locals on stack addressed via [x29, #-offset]

// tokens
#define T_NUM    1
#define T_CHAR   2
#define T_IDENT  3
#define T_INT    10
#define T_IF     11
#define T_ELSE   12
#define T_WHILE  13
#define T_RETURN 14
#define T_VOID   15
#define T_EQ     20  // ==
#define T_NE     21  // !=
#define T_LE     22  // <=
#define T_GE     23  // >=
#define T_AND    24  // &&
#define T_OR     25  // ||
#define T_EOF    99

typedef struct {
  const char *src;
  int pos;
  int tok;
  int num_val;
  char ident[32];
  // codegen
  uint32_t *code;
  int code_len;
  int code_max;
  // locals: name -> stack offset
  char locals[16][16];
  int local_offs[16]; // offset from x29 (negative)
  int num_locals;
  int stack_size; // current stack allocation for locals
  // error
  int error;
  char errmsg[64];
} cc_state_t;

static void cc_emit(cc_state_t *cc, uint32_t insn) {
  if (cc->code_len < cc->code_max)
    cc->code[cc->code_len++] = insn;
}

static void cc_error(cc_state_t *cc, const char *msg) {
  if (!cc->error) {
    cc->error = 1;
    int i = 0;
    while (msg[i] && i < 63) {
      cc->errmsg[i] = msg[i];
      i++;
    }
    cc->errmsg[i] = '\0';
  }
}

// lexer
static void cc_skip_ws(cc_state_t *cc) {
  while (cc->src[cc->pos] == ' ' || cc->src[cc->pos] == '\n' ||
         cc->src[cc->pos] == '\r' || cc->src[cc->pos] == '\t')
    cc->pos++;
  // skip // comments
  if (cc->src[cc->pos] == '/' && cc->src[cc->pos + 1] == '/') {
    while (cc->src[cc->pos] && cc->src[cc->pos] != '\n')
      cc->pos++;
    cc_skip_ws(cc);
  }
}

static int cc_is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int cc_is_digit(char c) { return c >= '0' && c <= '9'; }

static int cc_kw(const char *a, const char *b) {
  while (*a && *b && *a == *b) { a++; b++; }
  return *a == *b;
}

static void cc_next(cc_state_t *cc) {
  cc_skip_ws(cc);
  char c = cc->src[cc->pos];

  if (c == '\0') { cc->tok = T_EOF; return; }

  // character literal
  if (c == '\'') {
    cc->pos++;
    if (cc->src[cc->pos] == '\\') {
      cc->pos++;
      char e = cc->src[cc->pos++];
      if (e == 'n') cc->num_val = '\n';
      else if (e == 'r') cc->num_val = '\r';
      else if (e == 't') cc->num_val = '\t';
      else if (e == '0') cc->num_val = '\0';
      else if (e == '\\') cc->num_val = '\\';
      else if (e == '\'') cc->num_val = '\'';
      else cc->num_val = e;
    } else {
      cc->num_val = cc->src[cc->pos++];
    }
    cc->pos++; // closing quote
    cc->tok = T_NUM;
    return;
  }

  // number
  if (cc_is_digit(c)) {
    cc->num_val = 0;
    while (cc_is_digit(cc->src[cc->pos]))
      cc->num_val = cc->num_val * 10 + (cc->src[cc->pos++] - '0');
    cc->tok = T_NUM;
    return;
  }

  // identifier or keyword
  if (cc_is_alpha(c)) {
    int i = 0;
    while (cc_is_alpha(cc->src[cc->pos]) || cc_is_digit(cc->src[cc->pos])) {
      if (i < 31) cc->ident[i++] = cc->src[cc->pos];
      cc->pos++;
    }
    cc->ident[i] = '\0';
    if (cc_kw(cc->ident, "int")) cc->tok = T_INT;
    else if (cc_kw(cc->ident, "if")) cc->tok = T_IF;
    else if (cc_kw(cc->ident, "else")) cc->tok = T_ELSE;
    else if (cc_kw(cc->ident, "while")) cc->tok = T_WHILE;
    else if (cc_kw(cc->ident, "return")) cc->tok = T_RETURN;
    else if (cc_kw(cc->ident, "void")) cc->tok = T_VOID;
    else cc->tok = T_IDENT;
    return;
  }

  // two-char operators
  if (c == '=' && cc->src[cc->pos + 1] == '=') { cc->pos += 2; cc->tok = T_EQ; return; }
  if (c == '!' && cc->src[cc->pos + 1] == '=') { cc->pos += 2; cc->tok = T_NE; return; }
  if (c == '<' && cc->src[cc->pos + 1] == '=') { cc->pos += 2; cc->tok = T_LE; return; }
  if (c == '>' && cc->src[cc->pos + 1] == '=') { cc->pos += 2; cc->tok = T_GE; return; }
  if (c == '&' && cc->src[cc->pos + 1] == '&') { cc->pos += 2; cc->tok = T_AND; return; }
  if (c == '|' && cc->src[cc->pos + 1] == '|') { cc->pos += 2; cc->tok = T_OR; return; }

  // single char token
  cc->tok = c;
  cc->pos++;
}

static void cc_expect(cc_state_t *cc, int tok) {
  if (cc->tok != tok) {
    cc_error(cc, "syntax error");
    return;
  }
  cc_next(cc);
}

// find local variable, return stack offset (negative from x29)
static int cc_find_local(cc_state_t *cc, const char *name) {
  for (int i = 0; i < cc->num_locals; i++) {
    if (cc_kw(cc->locals[i], name))
      return cc->local_offs[i];
  }
  return 0; // not found
}

// add local variable, returns stack offset
static int cc_add_local(cc_state_t *cc, const char *name) {
  if (cc->num_locals >= 16) {
    cc_error(cc, "too many locals");
    return -8;
  }
  cc->stack_size += 8;
  int off = cc->stack_size;
  int idx = cc->num_locals++;
  int i = 0;
  while (name[i] && i < 15) { cc->locals[idx][i] = name[i]; i++; }
  cc->locals[idx][i] = '\0';
  cc->local_offs[idx] = off;
  return off;
}

// aarch64 instruction encoders

// movz xD, #imm16, lsl #shift
static uint32_t a64_movz(int rd, uint32_t imm16, int shift) {
  return 0xD2800000 | ((shift / 16) << 21) | ((imm16 & 0xFFFF) << 5) | rd;
}
// movk xD, #imm16, lsl #shift
static uint32_t a64_movk(int rd, uint32_t imm16, int shift) {
  return 0xF2800000 | ((shift / 16) << 21) | ((imm16 & 0xFFFF) << 5) | rd;
}
// load 64-bit immediate into register
static void cc_load_imm(cc_state_t *cc, int rd, uint64_t val) {
  cc_emit(cc, a64_movz(rd, val & 0xFFFF, 0));
  if (val > 0xFFFF)
    cc_emit(cc, a64_movk(rd, (val >> 16) & 0xFFFF, 16));
  if (val > 0xFFFFFFFF)
    cc_emit(cc, a64_movk(rd, (val >> 32) & 0xFFFF, 32));
  if (val > 0xFFFFFFFFFFFF)
    cc_emit(cc, a64_movk(rd, (val >> 48) & 0xFFFF, 48));
}

// stur x_src, [x_base, #simm9]  (store with signed offset)
static uint32_t a64_stur(int src, int base, int simm9) {
  return 0xF8000000 | ((simm9 & 0x1FF) << 12) | (base << 5) | src;
}
// ldur x_dst, [x_base, #simm9]
static uint32_t a64_ldur(int dst, int base, int simm9) {
  return 0xF8400000 | ((simm9 & 0x1FF) << 12) | (base << 5) | dst;
}

// sub xd, xn, #imm12
static uint32_t a64_sub_imm(int rd, int rn, uint32_t imm12) {
  return 0xD1000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd;
}
// add xd, xn, #imm12
static uint32_t a64_add_imm(int rd, int rn, uint32_t imm12) {
  return 0x91000000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd;
}

// sub xd, xn, xm
static uint32_t a64_sub_reg(int rd, int rn, int rm) {
  return 0xCB000000 | (rm << 16) | (rn << 5) | rd;
}
// add xd, xn, xm
static uint32_t a64_add_reg(int rd, int rn, int rm) {
  return 0x8B000000 | (rm << 16) | (rn << 5) | rd;
}
// mul xd, xn, xm
static uint32_t a64_mul(int rd, int rn, int rm) {
  return 0x9B007C00 | (rm << 16) | (rn << 5) | rd;
}
// sdiv xd, xn, xm
static uint32_t a64_sdiv(int rd, int rn, int rm) {
  return 0x9AC00C00 | (rm << 16) | (rn << 5) | rd;
}

// cmp xn, xm
static uint32_t a64_cmp(int rn, int rm) {
  return 0xEB000000 | (rm << 16) | (rn << 5) | 0x1F; // rd=xzr
}

// cset xd, cond
static uint32_t a64_cset(int rd, int cond) {
  // cset is csinc rd, xzr, xzr, invert(cond)
  int inv = cond ^ 1;
  return 0x9A9F07E0 | (inv << 12) | rd;
}

// b.cond offset (offset in instructions, signed)
static uint32_t a64_bcond(int cond, int off_insns) {
  return 0x54000000 | ((off_insns & 0x7FFFF) << 5) | cond;
}

// b offset
static uint32_t a64_b(int off_insns) {
  return 0x14000000 | (off_insns & 0x3FFFFFF);
}

// cbz x_reg, offset
static uint32_t a64_cbz(int reg, int off_insns) {
  return 0xB4000000 | ((off_insns & 0x7FFFF) << 5) | reg;
}

// ret
static uint32_t a64_ret(void) {
  return 0xD65F03C0;
}

// stp x_a, x_b, [sp, #imm7*8]!  (pre-index)
static uint32_t a64_stp_pre(int a, int b, int imm7) {
  return 0xA9800000 | ((imm7 & 0x7F) << 15) | (b << 10) | (31 << 5) | a;
}
// ldp x_a, x_b, [sp], #imm7*8  (post-index)
static uint32_t a64_ldp_post(int a, int b, int imm7) {
  return 0xA8C00000 | ((imm7 & 0x7F) << 15) | (b << 10) | (31 << 5) | a;
}

// mov xd, xn (alias for orr xd, xzr, xn)
static uint32_t a64_mov(int rd, int rn) {
  return 0xAA0003E0 | (rn << 16) | rd;
}

// strb w_src, [x_base]
static uint32_t a64_strb(int src, int base) {
  return 0x39000000 | (base << 5) | src;
}
// ldrb w_dst, [x_base]
static uint32_t a64_ldrb(int dst, int base) {
  return 0x39400000 | (base << 5) | dst;
}
// ldrb w_dst, [x_base, #imm12]
static uint32_t a64_ldrb_off(int dst, int base, int off) {
  return 0x39400000 | ((off & 0xFFF) << 10) | (base << 5) | dst;
}

// tst bits: ands xzr, xn, xm
// we won't need this, using cmp instead

// forward declarations for recursive descent
static void cc_expr(cc_state_t *cc);
static void cc_block(cc_state_t *cc);

// ---- expression parser / codegen ----
// result always in x0

static void cc_primary(cc_state_t *cc) {
  if (cc->error) return;

  if (cc->tok == T_NUM) {
    cc_load_imm(cc, 0, (uint64_t)(uint32_t)cc->num_val);
    cc_next(cc);
  } else if (cc->tok == T_IDENT) {
    char name[32];
    int i = 0;
    while (cc->ident[i] && i < 31) { name[i] = cc->ident[i]; i++; }
    name[i] = '\0';
    cc_next(cc);

    if (cc->tok == '(') {
      // function call
      cc_next(cc);
      if (cc_kw(name, "putc")) {
        // putc(expr) -> write char to UART
        cc_expr(cc);
        // store byte at [x19] (UART data register)
        cc_emit(cc, a64_strb(0, 19));
      } else if (cc_kw(name, "getc")) {
        // getc() -> poll UART, return char in x0
        // poll: ldrb w1, [x19, #0x18]; tst w1, #(1<<4); b.ne poll; ldrb w0, [x19]
        int poll_pc = cc->code_len;
        cc_emit(cc, a64_ldrb_off(1, 19, 0x18)); // ldrb w1, [x19, #0x18]
        // tst w1, #16 -> ands wzr, w1, #16
        // encoding for ands wzr, w1, #16 (imm bitmask for bit 4)
        // simpler: and w2, w1, #16; cbnz w2, poll
        cc_emit(cc, 0x12100021); // and w1, w1, #0x10
        int branch_pc = cc->code_len;
        cc_emit(cc, 0); // placeholder for cbnz
        cc_emit(cc, a64_ldrb(0, 19)); // ldrb w0, [x19]
        int after_pc = cc->code_len;
        // patch: cbnz w1, poll_pc (go back if RXFE set)
        int back = poll_pc - branch_pc;
        cc->code[branch_pc] = 0x35000000 | ((back & 0x7FFFF) << 5) | 1; // cbnz w1
        (void)after_pc;
      } else {
        cc_error(cc, "unknown function");
      }
      cc_expect(cc, ')');
    } else {
      // variable load
      int off = cc_find_local(cc, name);
      if (!off) {
        cc_error(cc, "undefined variable");
        return;
      }
      cc_emit(cc, a64_ldur(0, 29, -off));
    }
  } else if (cc->tok == '(') {
    cc_next(cc);
    cc_expr(cc);
    cc_expect(cc, ')');
  } else if (cc->tok == '-') {
    // unary minus
    cc_next(cc);
    cc_primary(cc);
    // neg x0, x0 -> sub x0, xzr, x0
    cc_emit(cc, a64_sub_reg(0, 31, 0));
  } else if (cc->tok == '!') {
    cc_next(cc);
    cc_primary(cc);
    // cmp x0, #0; cset x0, eq
    cc_emit(cc, 0xF100001F); // cmp x0, #0
    cc_emit(cc, a64_cset(0, 0)); // cset x0, eq (cond 0 = EQ)
  } else {
    cc_error(cc, "unexpected token");
  }
}

static void cc_mul_expr(cc_state_t *cc) {
  cc_primary(cc);
  while (cc->tok == '*' || cc->tok == '/' || cc->tok == '%') {
    int op = cc->tok;
    cc_next(cc);
    // push x0
    cc_emit(cc, a64_stp_pre(0, 30, -2)); // stp x0, x30, [sp, #-16]!
    cc_primary(cc);
    // pop left into x1
    cc_emit(cc, a64_ldp_post(1, 30, 2)); // ldp x1, x30, [sp], #16
    // x1 = left, x0 = right
    if (op == '*')
      cc_emit(cc, a64_mul(0, 1, 0));
    else if (op == '/')
      cc_emit(cc, a64_sdiv(0, 1, 0));
    else {
      // modulo: x0 = x1 - (x1/x0)*x0
      cc_emit(cc, a64_sdiv(2, 1, 0));  // x2 = x1 / x0
      cc_emit(cc, 0x9B008040 | (0 << 16) | (2 << 5) | 0); // msub x0, x2, x0, x1
      // msub xd, xn, xm, xa = xa - xn*xm
      // encoding: 0x9B00_8000 | rm<<16 | ra<<10 | rn<<5 | rd
      cc->code[cc->code_len - 1] = 0x9B008000 | (0 << 16) | (1 << 10) | (2 << 5) | 0;
    }
  }
}

static void cc_add_expr(cc_state_t *cc) {
  cc_mul_expr(cc);
  while (cc->tok == '+' || cc->tok == '-') {
    int op = cc->tok;
    cc_next(cc);
    cc_emit(cc, a64_stp_pre(0, 30, -2));
    cc_mul_expr(cc);
    cc_emit(cc, a64_ldp_post(1, 30, 2));
    if (op == '+')
      cc_emit(cc, a64_add_reg(0, 1, 0));
    else
      cc_emit(cc, a64_sub_reg(0, 1, 0));
  }
}

static void cc_cmp_expr(cc_state_t *cc) {
  cc_add_expr(cc);
  while (cc->tok == '<' || cc->tok == '>' || cc->tok == T_LE || cc->tok == T_GE) {
    int op = cc->tok;
    cc_next(cc);
    cc_emit(cc, a64_stp_pre(0, 30, -2));
    cc_add_expr(cc);
    cc_emit(cc, a64_ldp_post(1, 30, 2));
    cc_emit(cc, a64_cmp(1, 0)); // cmp left, right
    if (op == '<') cc_emit(cc, a64_cset(0, 11));       // lt
    else if (op == '>') cc_emit(cc, a64_cset(0, 12));   // gt
    else if (op == T_LE) cc_emit(cc, a64_cset(0, 13));  // le
    else cc_emit(cc, a64_cset(0, 10));                   // ge
  }
}

static void cc_eq_expr(cc_state_t *cc) {
  cc_cmp_expr(cc);
  while (cc->tok == T_EQ || cc->tok == T_NE) {
    int op = cc->tok;
    cc_next(cc);
    cc_emit(cc, a64_stp_pre(0, 30, -2));
    cc_cmp_expr(cc);
    cc_emit(cc, a64_ldp_post(1, 30, 2));
    cc_emit(cc, a64_cmp(1, 0));
    if (op == T_EQ) cc_emit(cc, a64_cset(0, 0));  // eq
    else cc_emit(cc, a64_cset(0, 1));               // ne
  }
}

static void cc_and_expr(cc_state_t *cc) {
  cc_eq_expr(cc);
  while (cc->tok == T_AND) {
    cc_next(cc);
    // short circuit: if x0 == 0, skip right side
    cc_emit(cc, 0xF100001F); // cmp x0, #0
    int patch = cc->code_len;
    cc_emit(cc, 0); // placeholder for b.eq (skip)
    cc_eq_expr(cc);
    // patch: b.eq to here
    int off = cc->code_len - patch;
    cc->code[patch] = a64_bcond(0, off); // b.eq
  }
}

static void cc_or_expr(cc_state_t *cc) {
  cc_and_expr(cc);
  while (cc->tok == T_OR) {
    cc_next(cc);
    // short circuit: if x0 != 0, skip right side
    cc_emit(cc, 0xF100001F); // cmp x0, #0
    int patch = cc->code_len;
    cc_emit(cc, 0); // placeholder for b.ne (skip)
    cc_and_expr(cc);
    int off = cc->code_len - patch;
    cc->code[patch] = a64_bcond(1, off); // b.ne
  }
}

static void cc_expr(cc_state_t *cc) {
  if (cc->error) return;

  // check for assignment: ident = expr
  if (cc->tok == T_IDENT) {
    // peek ahead for '='
    char name[32];
    int i = 0;
    while (cc->ident[i] && i < 31) { name[i] = cc->ident[i]; i++; }
    name[i] = '\0';
    int save_pos = cc->pos;
    int save_tok = cc->tok;
    cc_next(cc);
    if (cc->tok == '=' ) {
      // check it's not ==
      // (we already consumed the token, if it's '=' it's assignment since == is a separate token)
      cc_next(cc);
      cc_expr(cc);
      int off = cc_find_local(cc, name);
      if (!off) {
        cc_error(cc, "undefined variable");
        return;
      }
      cc_emit(cc, a64_stur(0, 29, -off));
      return;
    }
    // not assignment, backtrack
    cc->pos = save_pos;
    cc->tok = save_tok;
    // restore ident
    for (int j = 0; j < 32; j++) cc->ident[j] = name[j];
  }

  cc_or_expr(cc);
}

// ---- statement parser ----

static void cc_stmt(cc_state_t *cc) {
  if (cc->error) return;

  if (cc->tok == T_INT) {
    // int x; or int x = expr;
    cc_next(cc);
    if (cc->tok != T_IDENT) { cc_error(cc, "expected name"); return; }
    char name[32];
    int i = 0;
    while (cc->ident[i] && i < 31) { name[i] = cc->ident[i]; i++; }
    name[i] = '\0';
    cc_next(cc);

    int off = cc_add_local(cc, name);

    if (cc->tok == '=') {
      cc_next(cc);
      cc_expr(cc);
      cc_emit(cc, a64_stur(0, 29, -off));
    } else {
      // zero-init
      cc_emit(cc, a64_stur(31, 29, -off)); // store xzr
    }
    cc_expect(cc, ';');

  } else if (cc->tok == T_IF) {
    cc_next(cc);
    cc_expect(cc, '(');
    cc_expr(cc);
    cc_expect(cc, ')');
    // cbz x0, else_or_end
    int patch_else = cc->code_len;
    cc_emit(cc, 0); // placeholder
    cc_block(cc);
    if (cc->tok == T_ELSE) {
      cc_next(cc);
      int patch_end = cc->code_len;
      cc_emit(cc, 0); // placeholder for b end
      // patch else branch to here
      cc->code[patch_else] = a64_cbz(0, cc->code_len - patch_else);
      cc_block(cc);
      // patch end branch
      cc->code[patch_end] = a64_b(cc->code_len - patch_end);
    } else {
      cc->code[patch_else] = a64_cbz(0, cc->code_len - patch_else);
    }

  } else if (cc->tok == T_WHILE) {
    cc_next(cc);
    int loop_top = cc->code_len;
    cc_expect(cc, '(');
    cc_expr(cc);
    cc_expect(cc, ')');
    int patch_exit = cc->code_len;
    cc_emit(cc, 0); // placeholder cbz
    cc_block(cc);
    // b loop_top
    cc_emit(cc, a64_b(loop_top - cc->code_len));
    // patch exit
    cc->code[patch_exit] = a64_cbz(0, cc->code_len - patch_exit);

  } else if (cc->tok == T_RETURN) {
    cc_next(cc);
    if (cc->tok != ';') {
      cc_expr(cc);
    }
    // epilogue: restore and ret
    cc_emit(cc, a64_add_imm(31, 29, 0)); // mov sp, x29
    cc_emit(cc, a64_ldp_post(19, 20, 2)); // ldp x19, x20, [sp], #16
    cc_emit(cc, a64_ldp_post(29, 30, 2)); // ldp x29, x30, [sp], #16
    cc_emit(cc, a64_ret());
    cc_expect(cc, ';');

  } else {
    // expression statement
    cc_expr(cc);
    cc_expect(cc, ';');
  }
}

static void cc_block(cc_state_t *cc) {
  if (cc->error) return;
  cc_expect(cc, '{');
  while (cc->tok != '}' && cc->tok != T_EOF && !cc->error) {
    cc_stmt(cc);
  }
  cc_expect(cc, '}');
}

// compile source file into code_buf, return code length in instructions (-1 on error)
static int cc_compile(volatile uint8_t *u, int fs_pid, const char *filename,
                      uint32_t *code_buf, int code_max) {
  char src[1024];
  int slen = fs_read(fs_pid, filename, src, 1023);
  if (slen <= 0) {
    shell_puts(u, "error: can't read file\n");
    return -1;
  }
  src[slen] = '\0';

  cc_state_t cc;
  cc.src = src;
  cc.pos = 0;
  cc.tok = 0;
  cc.code = code_buf;
  cc.code_len = 0;
  cc.code_max = code_max;
  cc.num_locals = 0;
  cc.stack_size = 0;
  cc.error = 0;
  cc.errmsg[0] = '\0';

  // lex first token
  cc_next(&cc);

  // expect: int/void main() { ... }
  if (cc.tok == T_INT || cc.tok == T_VOID) cc_next(&cc);
  if (cc.tok != T_IDENT || !cc_kw(cc.ident, "main")) {
    shell_puts(u, "error: expected main()\n");
    return -1;
  }
  cc_next(&cc);
  cc_expect(&cc, '(');
  cc_expect(&cc, ')');

  // emit prologue
  cc_emit(&cc, a64_stp_pre(29, 30, -2));   // stp x29, x30, [sp, #-16]!
  cc_emit(&cc, a64_stp_pre(19, 20, -2));   // stp x19, x20, [sp, #-16]! (save callee-saved)
  cc_emit(&cc, a64_add_imm(29, 31, 0));      // mov x29, sp
  // reserve stack space for locals — we'll patch this
  int stack_patch = cc.code_len;
  cc_emit(&cc, 0); // placeholder: sub sp, sp, #N
  // save uart base (x19)
  // caller will put it in x0, move to x19
  cc_emit(&cc, a64_mov(19, 0));

  // parse body
  cc_block(&cc);

  if (cc.error) {
    shell_puts(u, "compile error: ");
    shell_puts(u, cc.errmsg);
    shell_puts(u, "\n");
    return -1;
  }

  // emit epilogue (for implicit return)
  cc_load_imm(&cc, 0, 0);
  cc_emit(&cc, a64_add_imm(31, 29, 0)); // mov sp, x29
  cc_emit(&cc, a64_ldp_post(19, 20, 2)); // restore x19, x20
  cc_emit(&cc, a64_ldp_post(29, 30, 2)); // restore x29, x30
  cc_emit(&cc, a64_ret());

  // patch stack reservation (align to 16)
  int frame_size = (cc.stack_size + 15) & ~15;
  if (frame_size == 0) frame_size = 16;
  cc.code[stack_patch] = a64_sub_imm(31, 31, frame_size);

  return cc.code_len;
}

// compile and run source code inline (same process)
static void cc_run(volatile uint8_t *u, int fs_pid, const char *filename) {
  uint32_t code_buf[512]; // 2KB
  int code_len = cc_compile(u, fs_pid, filename, code_buf, 512);
  if (code_len < 0) return;

  // flush icache
  sys_cacheflush(code_buf, code_len * 4);

  // run it! pass UART base as argument
  shell_puts(u, "[cc] running...\n");
  typedef int (*jit_fn)(uint64_t);
  jit_fn fn = (jit_fn)(void *)code_buf;
  int result = fn((uint64_t)u);

  shell_puts(u, "\n[cc] exit code: ");
  shell_put_int(u, result);
  shell_puts(u, "\n");
}

// compile and spawn as a separate task
static void cc_spawn(volatile uint8_t *u, int fs_pid, const char *filename) {
  uint32_t code_buf[512]; // 2KB
  int code_len = cc_compile(u, fs_pid, filename, code_buf, 512);
  if (code_len < 0) return;

  int pid = sys_spawn(code_buf, code_len * 4, filename);
  if (pid < 0) {
    shell_puts(u, "error: spawn failed (no free slots?)\n");
    return;
  }
  shell_puts(u, "[run] spawned as pid ");
  shell_put_int(u, pid);
  shell_puts(u, "\n");
}

// ---- snake game ----

#define SNAKE_W 30
#define SNAKE_H 15
#define SNAKE_MAX 200

static void snake_game(volatile uint8_t *u) {
  // snake body stored as ring buffer of (x,y) pairs
  uint8_t sx[SNAKE_MAX], sy[SNAKE_MAX];
  int head = 0, tail = 0, len = 3;
  int dx = 1, dy = 0; // moving right
  int score = 0;
  int alive = 1;

  // init snake in the middle
  for (int i = 0; i < len; i++) {
    int idx = (head + i) % SNAKE_MAX;
    sx[idx] = SNAKE_W / 2 - len + 1 + i;
    sy[idx] = SNAKE_H / 2;
  }
  tail = 0;
  head = len - 1;

  // pseudo-random seed from timer
  uint64_t seed;
  asm volatile("mrs %0, cntpct_el0" : "=r"(seed));

  // place food
  int fx = (seed >> 4) % (SNAKE_W - 2) + 1;
  int fy = (seed >> 12) % (SNAKE_H - 2) + 1;

  shell_puts(u, "\033[?25l");   // hide cursor
  shell_puts(u, "\033[2J\033[H"); // clear

  while (alive) {
    // ---- draw ----
    shell_puts(u, "\033[H"); // cursor home

    // score line
    shell_puts(u, "\033[1;33m score: "); // bold yellow
    shell_put_int(u, score);
    shell_puts(u, "  \033[0m\n");

    for (int y = 0; y < SNAKE_H; y++) {
      for (int x = 0; x < SNAKE_W; x++) {
        // border
        if (y == 0 || y == SNAKE_H - 1 || x == 0 || x == SNAKE_W - 1) {
          shell_puts(u, "\033[90m#\033[0m"); // dark gray
          continue;
        }

        // check if snake body
        int is_snake = 0;
        int is_head = 0;
        // walk from tail to head
        int count = len;
        int idx = tail;
        while (count > 0) {
          if (sx[idx] == (uint8_t)x && sy[idx] == (uint8_t)y) {
            is_snake = 1;
            if (idx == head) is_head = 1;
            break;
          }
          idx = (idx + 1) % SNAKE_MAX;
          count--;
        }

        if (is_head) {
          shell_puts(u, "\033[1;32m@\033[0m"); // bright green
        } else if (is_snake) {
          shell_puts(u, "\033[32mo\033[0m"); // green
        } else if (x == fx && y == fy) {
          shell_puts(u, "\033[1;31m*\033[0m"); // bright red
        } else {
          shell_putc(u, ' ');
        }
      }
      shell_putc(u, '\n');
    }
    shell_puts(u, " arrows/wasd to move | q to quit\n");

    // ---- delay + input ----
    // poll for input during the delay so keypresses aren't missed
    // only check UART every 10000 iterations (MMIO reads are slow)
    for (volatile int d = 0; d < 8000000; d++) {
      if ((d % 10000) == 0 && !(u[0x18] & (1 << 4))) {
        char c = u[0x00];
        if (c == 'q') { alive = 0; break; }
        if (c == 'w' && dy != 1)  { dx = 0; dy = -1; }
        if (c == 's' && dy != -1) { dx = 0; dy = 1; }
        if (c == 'a' && dx != 1)  { dx = -1; dy = 0; }
        if (c == 'd' && dx != -1) { dx = 1; dy = 0; }
        if (c == 0x1b) {
          // arrow key: ESC [ A/B/C/D
          for (volatile int w = 0; w < 100000; w++) ;
          if (!(u[0x18] & (1 << 4))) {
            char c2 = u[0x00];
            if (c2 == '[' && !(u[0x18] & (1 << 4))) {
              char c3 = u[0x00];
              if (c3 == 'A' && dy != 1)  { dx = 0; dy = -1; }
              if (c3 == 'B' && dy != -1) { dx = 0; dy = 1; }
              if (c3 == 'C' && dx != -1) { dx = 1; dy = 0; }
              if (c3 == 'D' && dx != 1)  { dx = -1; dy = 0; }
            }
          }
        }
      }
    }

    if (!alive) break;

    // ---- move ----
    int nx = (int)sx[head] + dx;
    int ny = (int)sy[head] + dy;

    // wall collision
    if (nx <= 0 || nx >= SNAKE_W - 1 || ny <= 0 || ny >= SNAKE_H - 1) {
      alive = 0;
      break;
    }

    // self collision
    int count = len;
    int idx = tail;
    while (count > 0) {
      if (sx[idx] == (uint8_t)nx && sy[idx] == (uint8_t)ny) {
        alive = 0;
        break;
      }
      idx = (idx + 1) % SNAKE_MAX;
      count--;
    }
    if (!alive) break;

    // advance head
    head = (head + 1) % SNAKE_MAX;
    sx[head] = nx;
    sy[head] = ny;

    // check food
    if (nx == fx && ny == fy) {
      score++;
      len++;
      // new food position
      seed = seed * 6364136223846793005ULL + 1;
      fx = (seed >> 16) % (SNAKE_W - 2) + 1;
      fy = (seed >> 24) % (SNAKE_H - 2) + 1;
    } else {
      // remove tail
      tail = (tail + 1) % SNAKE_MAX;
    }
  }

  // game over
  shell_puts(u, "\033[H");
  for (int i = 0; i < SNAKE_H / 2 + 1; i++) shell_putc(u, '\n');
  shell_puts(u, "        \033[1;31m  GAME OVER\033[0m\n");
  shell_puts(u, "        \033[1;33m  score: ");
  shell_put_int(u, score);
  shell_puts(u, "\033[0m\n\n");
  shell_puts(u, "        press any key...\n");
  shell_puts(u, "\033[?25h"); // show cursor
  shell_getc(u); // wait for keypress
  shell_puts(u, "\033[2J\033[H"); // clear
}

void shell_task(uint64_t uart_base) {
  volatile uint8_t *u = (volatile uint8_t *)uart_base;

  int fs_pid = ns_lookup_wait("fs");

  // ---- boot splash ----
  shell_puts(u, "\033[2J\033[H"); // clear screen
  shell_puts(u, "\n");
  shell_puts(u, "  _       _  ___  ____\n");
  shell_puts(u, " | |_ ___| |/ _ \\/ ___|\n");
  shell_puts(u, " | __/ _ \\ | | | \\___ \\\n");
  shell_puts(u, " | ||  __/ | |_| |___) |\n");
  shell_puts(u, "  \\__\\___|_|\\___/|____/\n");
  shell_puts(u, "\n");
  shell_puts(u, "Type 'help' for commands.\n");
  shell_puts(u, "Try 'cat hello.c' then 'run hello.c'\n\n");

  // pre-create a sample hello.c
  fs_create(fs_pid, "hello.c");
  {
    const char *sample =
      "int main() {\n"
      "  int i = 0;\n"
      "  while (i < 10) {\n"
      "    putc('0' + i);\n"
      "    putc('\\n');\n"
      "    i = i + 1;\n"
      "  }\n"
      "  return 0;\n"
      "}\n";
    fs_write(fs_pid, "hello.c", sample, u_strlen(sample));
  }

  char cmd_buf[128];

  // ---- command loop ----
  for (;;) {
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
          if (pos > 0) {
            pos--;
            shell_puts(u, "\b \b");
          }
          continue;
        }
        if (pos < 127) {
          cmd_buf[pos++] = c;
          shell_putc(u, c); // echo
        }
      }
      cmd_buf[pos] = '\0';

      if (pos == 0)
        continue;

      // parse: split into argv[0..argc-1]
      char *argv[8];
      int argc = 0;
      char *p = cmd_buf;
      while (*p && argc < 8) {
        while (*p == ' ')
          p++;
        if (!*p)
          break;
        argv[argc++] = p;
        while (*p && *p != ' ')
          p++;
        if (*p)
          *p++ = '\0';
      }
      if (argc == 0)
        continue;

      if (u_streq(argv[0], "clear")) {
        shell_puts(u, "\033[2J\033[H");

      } else if (u_streq(argv[0], "telfetch")) {
        shell_puts(u, "\n");
        shell_puts(u, "  _       _  ___  ____     telos\n");
        shell_puts(u, " | |_ ___| |/ _ \\/ ___|    --------\n");
        shell_puts(u, " | __/ _ \\ | | | \\___ \\    OS: telOS v0.1\n");
        shell_puts(u, " | ||  __/ | |_| |___) |   Kernel: telos-arm64\n");
        shell_puts(u, "  \\__\\___|_|\\___/|____/    Shell: tsh\n");
        shell_puts(u, "                           Editor: teled\n");
        shell_puts(u, "                           CC: tcc (JIT)\n");
        shell_puts(u, "                           CPU: ARM Cortex-A (aarch64)\n");
        shell_puts(u, "                           Memory: 128MB\n");
        shell_puts(u, "\n");

      } else if (u_streq(argv[0], "cc")) {
        if (argc < 2) {
          shell_puts(u, "usage: cc <file>\n");
        } else {
          cc_run(u, fs_pid, argv[1]);
        }

      } else if (u_streq(argv[0], "run")) {
        if (argc < 2) {
          shell_puts(u, "usage: run <file>\n");
        } else {
          cc_spawn(u, fs_pid, argv[1]);
        }

      } else if (u_streq(argv[0], "snake")) {
        snake_game(u);

      } else if (u_streq(argv[0], "teled")) {
        if (argc < 2) {
          shell_puts(u, "usage: teled <file>\n");
        } else {
          teled(u, fs_pid, argv[1]);
        }

      } else if (u_streq(argv[0], "help")) {
        shell_puts(u, "Commands:\n");
        shell_puts(u, "  ls              - list all files\n");
        shell_puts(u, "  cat <file>      - read file contents\n");
        shell_puts(u, "  create <name>   - create empty file\n");
        shell_puts(u, "  write <n> <data> - write text to file\n");
        shell_puts(u, "  tag <n> <k> <v> - add/update tag\n");
        shell_puts(u, "  query <k> <v>   - find files by tag\n");
        shell_puts(u, "  tags <file>     - show file's tags\n");
        shell_puts(u, "  teled <file>    - text editor\n");
        shell_puts(u, "  cc <file>       - compile & run C (inline)\n");
        shell_puts(u, "  run <file>      - compile & spawn as task\n");
        shell_puts(u, "  ps              - list running tasks\n");
        shell_puts(u, "  top             - live task monitor\n");
        shell_puts(u, "  telfetch        - system info\n");
        shell_puts(u, "  snake           - play snake!\n");
        shell_puts(u, "  clear           - clear screen\n");
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
            if (i < argc - 1)
              u_strcat(data, " ");
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

      } else if (u_streq(argv[0], "ps")) {
        char info[160]; // 8 procs * 20 bytes
        int bytes = sys_procinfo(info, 160);
        shell_puts(u, "PID  STATE    MEM     NAME\n");
        for (int i = 0; i < bytes; i += 20) {
          int pid = (uint8_t)info[i];
          int state = (uint8_t)info[i + 1];
          int pages = (uint8_t)info[i + 2];
          char *name = &info[i + 4];
          shell_put_int(u, pid);
          shell_puts(u, "    ");
          if (state == 1)
            shell_puts(u, "ready   ");
          else if (state == 2)
            shell_puts(u, "running ");
          else
            shell_puts(u, "blocked ");
          shell_put_int(u, pages * 4);
          shell_puts(u, "K     ");
          shell_puts(u, name);
          shell_puts(u, "\n");
        }

      } else if (u_streq(argv[0], "top")) {
        for (;;) {
          shell_puts(u, "\033[2J\033[H"); // clear screen
          shell_puts(u, "top - press any key to exit\n\n");
          char info[160];
          int bytes = sys_procinfo(info, 160);
          shell_puts(u, "PID  STATE    MEM     NAME\n");
          for (int i = 0; i < bytes; i += 20) {
            int pid = (uint8_t)info[i];
            int state = (uint8_t)info[i + 1];
            int pages = (uint8_t)info[i + 2];
            char *name = &info[i + 4];
            shell_put_int(u, pid);
            shell_puts(u, "    ");
            if (state == 1)
              shell_puts(u, "ready   ");
            else if (state == 2)
              shell_puts(u, "running ");
            else
              shell_puts(u, "blocked ");
            shell_put_int(u, pages * 4);
            shell_puts(u, "K     ");
            shell_puts(u, name);
            shell_puts(u, "\n");
          }
          // spin-delay ~1s
          for (volatile int d = 0; d < 5000000; d++)
            ;
          // check for keypress (non-blocking)
          if (!(u[0x18] & (1 << 4))) {
            (void)u[0x00]; // consume the key
            break;
          }
        }
        shell_puts(u, "\033[2J\033[H");

      } else {
        shell_puts(u, "unknown command: ");
        shell_puts(u, argv[0]);
        shell_puts(u, "\ntype 'help' for commands\n");
      }
    } // command loop
  }
}

// ---- entry point ----

void main() {
  uart_init();
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
  int uart_pid = proc_create(4, (void (*)(void))uart_server);
  proc_grant_device(uart_pid, 0x1F00030000);
  proc_set_name(uart_pid, "uart");
  int ns_pid = proc_create(4, (void (*)(void))nameserver);
  proc_set_name(ns_pid, "nameserver");
  int fs_pid = proc_create(4, (void (*)(void))fs_server);
  proc_set_name(fs_pid, "fs");
  int shell_pid = proc_create(4, (void (*)(void))shell_task);
  proc_grant_device(shell_pid, 0x1F00030000);
  proc_set_name(shell_pid, "shell");
  print("\n");

  asm volatile("msr daifclr, #2");
  print("[cpu] IRQs unmasked, tasks will start on first tick\n\n");

  for (;;) {
    asm volatile("wfe");
  }
}

#include <stdint.h>

// physical memory manager
// bitmap allocator, each bit = one 4KB page
// 1 = used, 0 = free

void print(const char *s);
void print_hex(unsigned long val);

#define PAGE_SIZE 4096

// RPi 5 ram range (first 128MB, rest available but unused for now)
#define RAM_START 0x00000000
#define RAM_END   0x08000000
#define TOTAL_PAGES ((RAM_END - RAM_START) / PAGE_SIZE)

static uint8_t bitmap[TOTAL_PAGES / 8];
static uint64_t first_free_page;

extern char _kernel_end[];

void pmm_init(void) {
    print("[pmm] ram: ");
    print_hex(RAM_START);
    print(" - ");
    print_hex(RAM_END);
    print("\n");

    uint64_t kernel_end = (uint64_t)_kernel_end;
    print("[pmm] kernel ends at: ");
    print_hex(kernel_end);
    print("\n");

    // zero out bitmap (everything free)
    for (uint64_t i = 0; i < TOTAL_PAGES / 8; i++)
        bitmap[i] = 0;

    // mark kernel pages as used so we dont hand them out
    uint64_t kernel_pages = (kernel_end - RAM_START) / PAGE_SIZE;
    for (uint64_t i = 0; i < kernel_pages; i++)
        bitmap[i / 8] |= (1 << (i % 8));

    first_free_page = kernel_pages;

    // count free pages
    uint64_t free_count = 0;
    for (uint64_t i = 0; i < TOTAL_PAGES; i++) {
        if (!(bitmap[i / 8] & (1 << (i % 8))))
            free_count++;
    }

    print("[pmm] total pages: ");
    print_hex(TOTAL_PAGES);
    print("\n");
    print("[pmm] kernel uses: ");
    print_hex(kernel_pages);
    print(" pages\n");
    print("[pmm] free: ");
    print_hex(free_count);
    print(" pages (");
    print_hex(free_count * PAGE_SIZE);
    print(" bytes)\n");
}

// find a free page and return its address, 0 if none left
uint64_t alloc_page(void) {
    for (uint64_t i = first_free_page; i < TOTAL_PAGES; i++) {
        if (!(bitmap[i / 8] & (1 << (i % 8)))) {
            bitmap[i / 8] |= (1 << (i % 8));
            return RAM_START + i * PAGE_SIZE;
        }
    }
    print("[pmm] out of memory!\n");
    return 0;
}

// free a page
void free_page(uint64_t addr) {
    uint64_t page = (addr - RAM_START) / PAGE_SIZE;

    if (page >= TOTAL_PAGES) {
        print("[pmm] bad free: ");
        print_hex(addr);
        print("\n");
        return;
    }

    bitmap[page / 8] &= ~(1 << (page % 8));

    // so alloc_page can find it again
    if (page < first_free_page)
        first_free_page = page;
}

#include <stdint.h>

// mmu setup
// identity maps everything (VA == PA) using 1GB block mappings
// 4KB granule, 39-bit VA space (T0SZ=25), so we start at L1

void print(const char *s);
void print_hex(unsigned long val);

// memory attribute indices (into MAIR_EL1)
#define MT_DEVICE 0  // device-nGnRnE
#define MT_NORMAL 1  // normal write-back cacheable

// page table entry bits
#define PTE_VALID    (1UL << 0)
#define PTE_BLOCK    (0UL << 1)  // L1/L2 block mapping (bit 1 = 0)
#define PTE_AF       (1UL << 10) // access flag, must set or instant fault
#define PTE_SH_INNER (3UL << 8)  // inner shareable
#define PTE_ATTR(n)  ((uint64_t)(n) << 2)

// L1 table, 512 entries, page-aligned
// with T0SZ=25, TTBR0 points straight to this
static uint64_t l1_table[512] __attribute__((aligned(4096)));

void mmu_init(void) {
    print("[mmu] setting up page tables\n");

    // zero out
    for (int i = 0; i < 512; i++)
        l1_table[i] = 0;

    // 0x00000000 - 0x3FFFFFFF (1GB) device memory
    // covers GIC (0x08000000) and UART (0x09000000)
    l1_table[0] = 0x00000000UL | PTE_VALID | PTE_BLOCK
                 | PTE_AF | PTE_ATTR(MT_DEVICE);

    // 0x40000000 - 0x7FFFFFFF (1GB) normal RAM
    // covers kernel + all usable memory
    l1_table[1] = 0x40000000UL | PTE_VALID | PTE_BLOCK
                 | PTE_AF | PTE_SH_INNER | PTE_ATTR(MT_NORMAL);

    print("[mmu] L1 at: ");
    print_hex((uint64_t)l1_table);
    print("\n");
    print("[mmu] 0x00000000-0x3fffffff -> device\n");
    print("[mmu] 0x40000000-0x7fffffff -> normal\n");

    // MAIR_EL1: what our attribute indices actually mean
    // attr0 = 0x00 -> device-nGnRnE (no gathering, no reordering, no early ack)
    // attr1 = 0xFF -> normal, write-back read+write allocate
    uint64_t mair = (0x00UL << (MT_DEVICE * 8))
                  | (0xFFUL << (MT_NORMAL * 8));
    asm volatile("msr mair_el1, %0" : : "r"(mair));

    // TCR_EL1: translation control register
    // T0SZ  = 25 -> 39-bit VA (512GB, way more than we need)
    // TG0   = 0b00 -> 4KB granule
    // SH0   = 0b11 -> inner shareable
    // ORGN0 = 0b01 -> outer write-back cacheable
    // IRGN0 = 0b01 -> inner write-back cacheable
    uint64_t tcr = (25UL << 0)      // T0SZ
                 | (0b00UL << 14)   // TG0
                 | (0b11UL << 12)   // SH0
                 | (0b01UL << 10)   // ORGN0
                 | (0b01UL << 8);   // IRGN0
    asm volatile("msr tcr_el1, %0" : : "r"(tcr));

    // point TTBR0 at our L1 table
    asm volatile("msr ttbr0_el1, %0" : : "r"((uint64_t)l1_table));

    // invalidate TLB and make sure everything above is visible
    asm volatile("tlbi vmalle1");
    asm volatile("dsb sy");
    asm volatile("isb");

    // flip the switch: enable MMU + caches
    uint64_t sctlr;
    asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1 << 0);   // M  - MMU on
    sctlr |= (1 << 2);   // C  - data cache
    sctlr |= (1 << 12);  // I  - instruction cache
    asm volatile("msr sctlr_el1, %0" : : "r"(sctlr));
    asm volatile("isb");

    print("[mmu] enabled\n");
}

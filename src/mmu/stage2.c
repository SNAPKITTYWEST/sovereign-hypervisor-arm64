#include "sov_hypervisor.h"

// =============================================================================
// Stage 2 Translation Tables — Memory Isolation
// =============================================================================
// IPA → PA mapping with per-page attributes.
// 3-level table walk: L1 (1GB blocks) → L2 (2MB blocks) → L3 (4KB pages)
// Loaded into VTTBR_EL2 with VMID for TLB tagging.
// =============================================================================

#define L1_SHIFT    30
#define L2_SHIFT    21
#define L3_SHIFT    12
#define TABLE_ENTRIES 512
#define BLOCK_1G    (1UL << 30)
#define BLOCK_2M    (1UL << 21)
#define PAGE_4K     (1UL << 12)

// Page table pool (statically allocated for bare-metal)
static uint64_t s2_page_pool[SOV_MAX_GUESTS][4096][512] __attribute__((aligned(4096)));
static uint32_t s2_pool_idx[SOV_MAX_GUESTS];

static uint64_t *alloc_page_table(sov_guest_t *guest) {
    uint32_t idx = s2_pool_idx[guest->guest_id]++;
    return s2_page_pool[guest->guest_id][idx];
}

static inline uint64_t ipa_l1_index(uint64_t ipa) {
    return (ipa >> L1_SHIFT) & 0x1FF;
}

static inline uint64_t ipa_l2_index(uint64_t ipa) {
    return (ipa >> L2_SHIFT) & 0x1FF;
}

static inline uint64_t ipa_l3_index(uint64_t ipa) {
    return (ipa >> L3_SHIFT) & 0x1FF;
}

int sov_stage2_map(sov_guest_t *guest, uint64_t ipa, uint64_t pa,
                   uint64_t size, uint64_t attrs) {
    uint64_t *l1 = guest->stage2_root;
    if (!l1) return -1;

    uint64_t end = ipa + size;

    while (ipa < end) {
        uint64_t l1_idx = ipa_l1_index(ipa);

        // Can we use a 1GB block mapping?
        if ((ipa & (BLOCK_1G - 1)) == 0 &&
            (pa & (BLOCK_1G - 1)) == 0 &&
            (end - ipa) >= BLOCK_1G) {
            l1[l1_idx] = pa | S2_VALID | S2_AF | attrs;
            ipa += BLOCK_1G;
            pa  += BLOCK_1G;
            continue;
        }

        // Need L2 table
        uint64_t *l2;
        if (l1[l1_idx] & S2_VALID) {
            if (!(l1[l1_idx] & S2_TABLE)) return -1; // Block in the way
            l2 = (uint64_t *)(l1[l1_idx] & 0x0000FFFFFFFFF000UL);
        } else {
            l2 = alloc_page_table(guest);
            for (int i = 0; i < TABLE_ENTRIES; i++) l2[i] = 0;
            l1[l1_idx] = (uint64_t)l2 | S2_TABLE | S2_VALID;
        }

        uint64_t l2_idx = ipa_l2_index(ipa);

        // Can we use a 2MB block mapping?
        if ((ipa & (BLOCK_2M - 1)) == 0 &&
            (pa & (BLOCK_2M - 1)) == 0 &&
            (end - ipa) >= BLOCK_2M) {
            l2[l2_idx] = pa | S2_VALID | S2_AF | attrs;
            ipa += BLOCK_2M;
            pa  += BLOCK_2M;
            continue;
        }

        // Need L3 table
        uint64_t *l3;
        if (l2[l2_idx] & S2_VALID) {
            if (!(l2[l2_idx] & S2_TABLE)) return -1;
            l3 = (uint64_t *)(l2[l2_idx] & 0x0000FFFFFFFFF000UL);
        } else {
            l3 = alloc_page_table(guest);
            for (int i = 0; i < TABLE_ENTRIES; i++) l3[i] = 0;
            l2[l2_idx] = (uint64_t)l3 | S2_TABLE | S2_VALID;
        }

        // 4KB page mapping
        uint64_t l3_idx = ipa_l3_index(ipa);
        l3[l3_idx] = pa | S2_PAGE | S2_AF | attrs;
        ipa += PAGE_4K;
        pa  += PAGE_4K;
    }

    return 0;
}

int sov_stage2_unmap(sov_guest_t *guest, uint64_t ipa, uint64_t size) {
    uint64_t *l1 = guest->stage2_root;
    if (!l1) return -1;

    uint64_t end = ipa + size;

    while (ipa < end) {
        uint64_t l1_idx = ipa_l1_index(ipa);

        if (!(l1[l1_idx] & S2_VALID)) {
            ipa += BLOCK_1G;
            continue;
        }

        if (!(l1[l1_idx] & S2_TABLE)) {
            // 1GB block — invalidate entire entry
            l1[l1_idx] = 0;
            ipa += BLOCK_1G;
            continue;
        }

        uint64_t *l2 = (uint64_t *)(l1[l1_idx] & 0x0000FFFFFFFFF000UL);
        uint64_t l2_idx = ipa_l2_index(ipa);

        if (!(l2[l2_idx] & S2_VALID)) {
            ipa += BLOCK_2M;
            continue;
        }

        if (!(l2[l2_idx] & S2_TABLE)) {
            l2[l2_idx] = 0;
            ipa += BLOCK_2M;
            continue;
        }

        uint64_t *l3 = (uint64_t *)(l2[l2_idx] & 0x0000FFFFFFFFF000UL);
        uint64_t l3_idx = ipa_l3_index(ipa);
        l3[l3_idx] = 0;
        ipa += PAGE_4K;
    }

    return 0;
}

void sov_stage2_flush(sov_guest_t *guest) {
    // TLB invalidation for this VMID
    // TLBI VMALLS12E1IS — invalidate all Stage 1+2 TLB entries for this VMID
    uint64_t vmid = guest->vmid;
    __asm__ volatile(
        "dsb ishst\n"
        "tlbi vmalls12e1is\n"
        "dsb ish\n"
        "isb\n"
        ::: "memory"
    );
    (void)vmid;
}

int sov_guest_create(sov_guest_t *guest, uint32_t vcpu_count) {
    if (vcpu_count > SOV_MAX_VCPUS) return -1;

    guest->vcpu_count = vcpu_count;
    s2_pool_idx[guest->guest_id] = 0;

    // Allocate root table (L1)
    guest->stage2_root = alloc_page_table(guest);
    for (int i = 0; i < TABLE_ENTRIES; i++) {
        guest->stage2_root[i] = 0;
    }

    // Initialize vCPU contexts
    for (uint32_t i = 0; i < vcpu_count; i++) {
        sov_vcpu_context_t *ctx = &guest->vcpus[i];
        for (int r = 0; r < 31; r++) ctx->x[r] = 0;
        ctx->pc = 0;
        ctx->pstate = 0x3C5; // EL1h, DAIF masked
        ctx->sctlr_el1 = 0;
        ctx->ttbr0_el1 = 0;
        ctx->ttbr1_el1 = 0;
        ctx->tcr_el1 = 0;
        ctx->mair_el1 = 0;
        ctx->vbar_el1 = 0;
    }

    guest->mmio_count = 0;
    guest->jit_cache = 0;
    guest->jit_cache_size = 0;

    return 0;
}

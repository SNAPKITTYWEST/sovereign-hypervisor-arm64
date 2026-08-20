#ifndef SOV_HYPERVISOR_H
#define SOV_HYPERVISOR_H

#include <stdint.h>

// =============================================================================
// Sovereign ARM64 EL2 Hypervisor — Core Definitions
// Target: Qualcomm Snapdragon X Elite (Oryon cores, Adreno GPU)
// =============================================================================

#define SOV_MAX_VCPUS       64
#define SOV_MAX_GUESTS      16
#define SOV_PAGE_SIZE       4096
#define SOV_STAGE2_LEVELS   3
#define SOV_IPA_BITS        48
#define SOV_MAX_MMIO_REGIONS 256

// HCR_EL2 bit definitions
#define HCR_VM      (1UL << 0)
#define HCR_SWIO    (1UL << 1)
#define HCR_FMO     (1UL << 3)
#define HCR_IMO     (1UL << 4)
#define HCR_AMO     (1UL << 5)
#define HCR_TWI     (1UL << 13)
#define HCR_TWE     (1UL << 14)
#define HCR_TSC     (1UL << 19)
#define HCR_TVM     (1UL << 26)
#define HCR_RW      (1UL << 31)

// ESR_EL2 Exception Class codes
#define ESR_EC_SHIFT    26
#define ESR_EC_MASK     0x3F
#define ESR_EC_UNKNOWN  0x00
#define ESR_EC_WFI      0x01
#define ESR_EC_HVC64    0x16
#define ESR_EC_SMC64    0x17
#define ESR_EC_SYSREG   0x18
#define ESR_EC_IABT_L   0x20
#define ESR_EC_IABT_H   0x21
#define ESR_EC_DABT_L   0x24
#define ESR_EC_DABT_H   0x25

// Stage 2 page table entry attributes
#define S2_VALID        (1UL << 0)
#define S2_TABLE        (1UL << 1)
#define S2_PAGE         (3UL << 0)
#define S2_AF           (1UL << 10)
#define S2_SH_IS       (3UL << 8)
#define S2_ATTR_MEM     (0xFUL << 2)  // Normal memory
#define S2_ATTR_DEV     (0x1UL << 2)  // Device memory
#define S2_RO           (1UL << 7)    // Read-only (S2AP[1])
#define S2_RW           (3UL << 6)    // Read-write
#define S2_XN           (1UL << 54)   // Execute-never

// vCPU state
typedef struct {
    uint64_t x[31];         // General purpose registers
    uint64_t sp_el0;
    uint64_t sp_el1;
    uint64_t elr_el1;
    uint64_t spsr_el1;
    uint64_t esr_el1;
    uint64_t far_el1;
    uint64_t sctlr_el1;
    uint64_t ttbr0_el1;
    uint64_t ttbr1_el1;
    uint64_t tcr_el1;
    uint64_t mair_el1;
    uint64_t vbar_el1;
    uint64_t pc;            // Guest PC (ELR_EL2)
    uint64_t pstate;        // Guest PSTATE (SPSR_EL2)
    // NEON/SVE state
    __uint128_t v[32];      // NEON/SVE registers
    uint64_t fpcr;
    uint64_t fpsr;
} sov_vcpu_context_t;

// VM-exit reason
typedef enum {
    SOV_EXIT_MMIO_READ,
    SOV_EXIT_MMIO_WRITE,
    SOV_EXIT_HVC,
    SOV_EXIT_SMC,
    SOV_EXIT_SYSREG,
    SOV_EXIT_WFI,
    SOV_EXIT_IRQ,
    SOV_EXIT_FAULT,
    SOV_EXIT_UNKNOWN,
} sov_exit_reason_t;

// VM-exit info
typedef struct {
    sov_exit_reason_t reason;
    uint64_t          esr;
    uint64_t          far;
    uint64_t          ipa;
    uint32_t          len;      // Instruction length (2 or 4)
    uint32_t          srt;      // Source/dest register for MMIO
    uint64_t          data;     // Write data for MMIO write
} sov_exit_info_t;

// MMIO region handler
typedef struct {
    uint64_t base_ipa;
    uint64_t size;
    int (*read)(uint64_t offset, uint32_t size, uint64_t *value, void *opaque);
    int (*write)(uint64_t offset, uint32_t size, uint64_t value, void *opaque);
    void *opaque;
} sov_mmio_region_t;

// Guest VM
typedef struct {
    uint32_t            guest_id;
    uint32_t            vcpu_count;
    sov_vcpu_context_t  vcpus[SOV_MAX_VCPUS];
    uint64_t           *stage2_root;    // VTTBR_EL2 value
    uint64_t            vmid;
    sov_mmio_region_t   mmio_regions[SOV_MAX_MMIO_REGIONS];
    uint32_t            mmio_count;
    // DBT JIT state
    void               *jit_cache;
    uint64_t            jit_cache_size;
} sov_guest_t;

// =============================================================================
// API
// =============================================================================

// Boot & init
void sov_hypervisor_init(void);
int  sov_guest_create(sov_guest_t *guest, uint32_t vcpu_count);
int  sov_guest_destroy(sov_guest_t *guest);

// Memory management (Stage 2)
int  sov_stage2_map(sov_guest_t *guest, uint64_t ipa, uint64_t pa,
                    uint64_t size, uint64_t attrs);
int  sov_stage2_unmap(sov_guest_t *guest, uint64_t ipa, uint64_t size);
void sov_stage2_flush(sov_guest_t *guest);

// vCPU execution
int  sov_vcpu_run(sov_guest_t *guest, uint32_t vcpu_id, sov_exit_info_t *exit);
void sov_vcpu_exit_handler(uint64_t esr, uint64_t far, void *ctx);
int  sov_vcpu_inject_irq(sov_guest_t *guest, uint32_t vcpu_id, uint32_t irq);

// MMIO
int  sov_mmio_register(sov_guest_t *guest, uint64_t base, uint64_t size,
                       int (*read)(uint64_t, uint32_t, uint64_t*, void*),
                       int (*write)(uint64_t, uint32_t, uint64_t, void*),
                       void *opaque);

// DBT (Dynamic Binary Translation)
int  sov_dbt_init(sov_guest_t *guest, uint64_t cache_size);
int  sov_dbt_translate_block(sov_guest_t *guest, uint64_t guest_pc,
                             void **native_code, uint64_t *code_size);
void sov_dbt_invalidate(sov_guest_t *guest, uint64_t guest_addr, uint64_t size);

// VirtIO-GPU (Adreno passthrough)
int  sov_virtio_gpu_init(sov_guest_t *guest);
int  sov_virtio_gpu_submit(sov_guest_t *guest, void *cmd_buf, uint64_t size);

#endif // SOV_HYPERVISOR_H

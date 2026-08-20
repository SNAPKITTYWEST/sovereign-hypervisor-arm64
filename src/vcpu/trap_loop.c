#include "sov_hypervisor.h"

// =============================================================================
// vCPU Trap Loop — Exception Handler & Context Switcher
// =============================================================================
// Run guest via ERET, handle VM-exits by reading ESR_EL2/FAR_EL2,
// dispatch to MMIO handlers, sysreg emulation, or DBT engine.
// =============================================================================

static sov_guest_t *current_guest;
static uint32_t     current_vcpu_id;

// Decode ESR_EL2 into exit info
static void decode_exit(uint64_t esr, uint64_t far, sov_exit_info_t *exit) {
    uint32_t ec = (esr >> ESR_EC_SHIFT) & ESR_EC_MASK;
    uint32_t il = (esr >> 25) & 1;  // Instruction length

    exit->esr = esr;
    exit->far = far;
    exit->len = il ? 4 : 2;

    switch (ec) {
    case ESR_EC_DABT_L:
    case ESR_EC_DABT_H: {
        // Data abort — likely MMIO
        uint32_t isv  = (esr >> 24) & 1;
        uint32_t sas  = (esr >> 22) & 3;   // Access size: 0=byte,1=hw,2=word,3=dw
        uint32_t wnr  = (esr >> 6) & 1;    // Write-not-Read
        uint32_t srt  = (esr >> 16) & 0x1F; // Source register

        if (!isv) {
            exit->reason = SOV_EXIT_FAULT;
            return;
        }

        exit->reason = wnr ? SOV_EXIT_MMIO_WRITE : SOV_EXIT_MMIO_READ;
        exit->ipa = far;  // HPFAR_EL2 gives IPA, FAR gives VA
        exit->srt = srt;

        if (wnr && current_guest) {
            sov_vcpu_context_t *ctx = &current_guest->vcpus[current_vcpu_id];
            exit->data = (srt < 31) ? ctx->x[srt] : 0;
        }
        break;
    }

    case ESR_EC_HVC64:
        exit->reason = SOV_EXIT_HVC;
        break;

    case ESR_EC_SMC64:
        exit->reason = SOV_EXIT_SMC;
        break;

    case ESR_EC_SYSREG:
        exit->reason = SOV_EXIT_SYSREG;
        break;

    case ESR_EC_WFI:
        exit->reason = SOV_EXIT_WFI;
        break;

    default:
        exit->reason = SOV_EXIT_UNKNOWN;
        break;
    }
}

// Handle MMIO access
static int handle_mmio(sov_guest_t *guest, sov_exit_info_t *exit) {
    uint64_t ipa = exit->ipa;

    for (uint32_t i = 0; i < guest->mmio_count; i++) {
        sov_mmio_region_t *region = &guest->mmio_regions[i];
        if (ipa >= region->base_ipa && ipa < region->base_ipa + region->size) {
            uint64_t offset = ipa - region->base_ipa;
            uint32_t access_size = 1 << ((exit->esr >> 22) & 3);

            if (exit->reason == SOV_EXIT_MMIO_READ) {
                uint64_t value = 0;
                int ret = region->read(offset, access_size, &value, region->opaque);
                if (ret == 0 && exit->srt < 31) {
                    sov_vcpu_context_t *ctx = &guest->vcpus[current_vcpu_id];
                    ctx->x[exit->srt] = value;
                }
                return ret;
            } else {
                return region->write(offset, access_size, exit->data, region->opaque);
            }
        }
    }

    return -1;  // Unhandled MMIO region
}

// Called from assembly exception handler
void sov_vcpu_exit_handler(uint64_t esr, uint64_t far, void *ctx) {
    sov_exit_info_t exit;
    decode_exit(esr, far, &exit);

    if (!current_guest) return;

    switch (exit.reason) {
    case SOV_EXIT_MMIO_READ:
    case SOV_EXIT_MMIO_WRITE:
        handle_mmio(current_guest, &exit);
        // Advance PC past faulting instruction
        current_guest->vcpus[current_vcpu_id].pc += exit.len;
        break;

    case SOV_EXIT_HVC:
        // Hypercall — dispatch based on x0 (function ID)
        // Guest uses HVC for paravirt services
        break;

    case SOV_EXIT_SMC:
        // SMC trapped — either emulate or inject to guest
        current_guest->vcpus[current_vcpu_id].pc += 4;
        break;

    case SOV_EXIT_WFI:
        // Guest idle — yield to scheduler
        current_guest->vcpus[current_vcpu_id].pc += 4;
        break;

    case SOV_EXIT_SYSREG:
        // System register access — emulate
        current_guest->vcpus[current_vcpu_id].pc += 4;
        break;

    case SOV_EXIT_FAULT:
    case SOV_EXIT_UNKNOWN:
        // Fatal — halt vCPU
        break;
    }
}

// Load vCPU context into hardware and ERET into guest
static void vcpu_enter(sov_guest_t *guest, uint32_t vcpu_id) {
    sov_vcpu_context_t *ctx = &guest->vcpus[vcpu_id];

    // Load Stage 2 table base with VMID
    uint64_t vttbr = ((uint64_t)guest->stage2_root) | (guest->vmid << 48);

    __asm__ volatile(
        "msr VTTBR_EL2, %0\n"
        "isb\n"
        :: "r"(vttbr) : "memory"
    );

    // Load guest system registers
    __asm__ volatile(
        "msr SCTLR_EL1, %0\n"
        "msr TTBR0_EL1, %1\n"
        "msr TTBR1_EL1, %2\n"
        "msr TCR_EL1, %3\n"
        "msr MAIR_EL1, %4\n"
        "msr VBAR_EL1, %5\n"
        "msr SP_EL1, %6\n"
        "isb\n"
        :: "r"(ctx->sctlr_el1),
           "r"(ctx->ttbr0_el1),
           "r"(ctx->ttbr1_el1),
           "r"(ctx->tcr_el1),
           "r"(ctx->mair_el1),
           "r"(ctx->vbar_el1),
           "r"(ctx->sp_el1)
        : "memory"
    );

    // Set return address and state
    __asm__ volatile(
        "msr ELR_EL2, %0\n"
        "msr SPSR_EL2, %1\n"
        :: "r"(ctx->pc), "r"(ctx->pstate)
    );
}

int sov_vcpu_run(sov_guest_t *guest, uint32_t vcpu_id, sov_exit_info_t *exit) {
    if (vcpu_id >= guest->vcpu_count) return -1;

    current_guest = guest;
    current_vcpu_id = vcpu_id;

    // Enter guest (assembly does ERET, returns on VM-exit)
    vcpu_enter(guest, vcpu_id);

    // After exit handler runs, decode what happened
    uint64_t esr, far;
    __asm__ volatile("mrs %0, ESR_EL2" : "=r"(esr));
    __asm__ volatile("mrs %0, FAR_EL2" : "=r"(far));
    decode_exit(esr, far, exit);

    return 0;
}

int sov_mmio_register(sov_guest_t *guest, uint64_t base, uint64_t size,
                      int (*read)(uint64_t, uint32_t, uint64_t*, void*),
                      int (*write)(uint64_t, uint32_t, uint64_t, void*),
                      void *opaque) {
    if (guest->mmio_count >= SOV_MAX_MMIO_REGIONS) return -1;

    sov_mmio_region_t *region = &guest->mmio_regions[guest->mmio_count++];
    region->base_ipa = base;
    region->size = size;
    region->read = read;
    region->write = write;
    region->opaque = opaque;

    // Unmap this IPA range from Stage 2 so accesses trap
    sov_stage2_unmap(guest, base, size);
    sov_stage2_flush(guest);

    return 0;
}

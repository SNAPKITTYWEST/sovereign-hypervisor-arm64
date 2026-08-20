// =============================================================================
// Sovereign Hypervisor — EL2 Entry Point (AArch64)
// Target: Qualcomm Snapdragon X Elite (Oryon cores)
// =============================================================================
// Boot CPU into Exception Level 2, configure HCR_EL2 for Stage 2 MMU,
// trap privileged instructions, and jump to C initialization.
// =============================================================================

.section .text.boot
.global _start
.global _el2_vector_table

// Exception vector table alignment (11-bit = 2048 bytes)
.balign 2048
_el2_vector_table:
    // Current EL with SP0
    b   _sync_handler       // Synchronous
    .balign 128
    b   _irq_handler        // IRQ
    .balign 128
    b   _fiq_handler        // FIQ
    .balign 128
    b   _serror_handler     // SError

    // Current EL with SPx
    .balign 128
    b   _sync_handler
    .balign 128
    b   _irq_handler
    .balign 128
    b   _fiq_handler
    .balign 128
    b   _serror_handler

    // Lower EL using AArch64
    .balign 128
    b   _lower_sync_handler // Guest VM-exit lands here
    .balign 128
    b   _lower_irq_handler
    .balign 128
    b   _lower_fiq_handler
    .balign 128
    b   _lower_serror_handler

    // Lower EL using AArch32
    .balign 128
    b   _lower_sync_handler
    .balign 128
    b   _lower_irq_handler
    .balign 128
    b   _lower_fiq_handler
    .balign 128
    b   _lower_serror_handler

// =============================================================================
// Boot entry — CPU arrives here at EL2 (or EL3 on some platforms)
// =============================================================================
.balign 16
_start:
    // Verify we're at EL2
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.eq    _at_el2

    // If at EL3, drop to EL2
    cmp     x0, #3
    b.eq    _drop_to_el2
    b       _halt               // Unexpected EL — halt

_drop_to_el2:
    // Configure SCR_EL3: NS=1, HCE=1, SMD=0, RW=1 (AArch64 at EL2)
    mov     x0, #0x5B1
    msr     SCR_EL3, x0

    // Set EL2h as return level
    mov     x0, #0x3C9          // DAIF masked, EL2h
    msr     SPSR_EL3, x0
    adr     x0, _at_el2
    msr     ELR_EL3, x0
    eret

_at_el2:
    // Set stack pointer
    ldr     x0, =_el2_stack_top
    mov     sp, x0

    // Install vector table
    adr     x0, _el2_vector_table
    msr     VBAR_EL2, x0

    // Configure HCR_EL2 — the core hypervisor control register
    // Bits:
    //   VM   (0)  = 1  → Enable Stage 2 translation
    //   SWIO (1)  = 1  → Set/Way Invalidation Override
    //   FMO  (3)  = 1  → Physical FIQ → EL2
    //   IMO  (4)  = 1  → Physical IRQ → EL2
    //   AMO  (5)  = 1  → SError → EL2
    //   TWI  (13) = 1  → Trap WFI
    //   TWE  (14) = 1  → Trap WFE
    //   TSC  (19) = 1  → Trap SMC
    //   TVM  (26) = 1  → Trap virtual memory controls
    //   RW   (31) = 1  → EL1 is AArch64
    mov     x0, #0x0
    orr     x0, x0, #(1 << 0)   // VM
    orr     x0, x0, #(1 << 1)   // SWIO
    orr     x0, x0, #(1 << 3)   // FMO
    orr     x0, x0, #(1 << 4)   // IMO
    orr     x0, x0, #(1 << 5)   // AMO
    orr     x0, x0, #(1 << 13)  // TWI
    orr     x0, x0, #(1 << 14)  // TWE
    orr     x0, x0, #(1 << 19)  // TSC
    orr     x0, x0, #(1 << 26)  // TVM
    orr     x0, x0, #(1 << 31)  // RW
    msr     HCR_EL2, x0

    // Configure VTCR_EL2 — Stage 2 translation control
    // T0SZ=16 (48-bit IPA), IRGN0=WB-WA, ORGN0=WB-WA, SH0=IS, TG0=4KB, PS=48-bit
    mov     x0, #16             // T0SZ = 16 → 48-bit IPA space
    orr     x0, x0, #(1 << 8)  // IRGN0 = 0b01 (Write-Back Write-Allocate)
    orr     x0, x0, #(1 << 10) // ORGN0 = 0b01
    orr     x0, x0, #(3 << 12) // SH0 = 0b11 (Inner Shareable)
    orr     x0, x0, #(0 << 14) // TG0 = 0b00 (4KB granule)
    orr     x0, x0, #(5 << 16) // PS = 0b101 (48-bit, 256TB)
    orr     x0, x0, #(1 << 6)  // SL0 = 0b01 (start at level 1)
    msr     VTCR_EL2, x0

    // Zero out BSS
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
1:  cmp     x0, x1
    b.ge    2f
    str     xzr, [x0], #8
    b       1b
2:
    // Jump to C initialization
    bl      sov_hypervisor_init

_halt:
    wfe
    b       _halt

// =============================================================================
// Exception Handlers (stubs — filled by vcpu trap loop)
// =============================================================================
_sync_handler:
    b       _halt

_irq_handler:
    b       _halt

_fiq_handler:
    b       _halt

_serror_handler:
    b       _halt

// Lower EL handlers — guest VM-exit
_lower_sync_handler:
    // Save guest context
    stp     x0, x1, [sp, #-16]!
    stp     x2, x3, [sp, #-16]!
    stp     x4, x5, [sp, #-16]!
    stp     x6, x7, [sp, #-16]!
    stp     x8, x9, [sp, #-16]!
    stp     x10, x11, [sp, #-16]!
    stp     x12, x13, [sp, #-16]!
    stp     x14, x15, [sp, #-16]!
    stp     x16, x17, [sp, #-16]!
    stp     x18, x19, [sp, #-16]!
    stp     x20, x21, [sp, #-16]!
    stp     x22, x23, [sp, #-16]!
    stp     x24, x25, [sp, #-16]!
    stp     x26, x27, [sp, #-16]!
    stp     x28, x29, [sp, #-16]!
    mrs     x0, ELR_EL2
    mrs     x1, SPSR_EL2
    stp     x30, x0, [sp, #-16]!
    str     x1, [sp, #-8]!

    // Read syndrome
    mrs     x0, ESR_EL2
    mrs     x1, FAR_EL2
    mov     x2, sp              // pointer to saved context

    // Call C handler
    bl      sov_vcpu_exit_handler

    // Restore guest context
    ldr     x1, [sp], #8
    ldp     x30, x0, [sp], #16
    msr     SPSR_EL2, x1
    msr     ELR_EL2, x0
    ldp     x28, x29, [sp], #16
    ldp     x26, x27, [sp], #16
    ldp     x24, x25, [sp], #16
    ldp     x22, x23, [sp], #16
    ldp     x20, x21, [sp], #16
    ldp     x18, x19, [sp], #16
    ldp     x16, x17, [sp], #16
    ldp     x14, x15, [sp], #16
    ldp     x12, x13, [sp], #16
    ldp     x10, x11, [sp], #16
    ldp     x8, x9, [sp], #16
    ldp     x6, x7, [sp], #16
    ldp     x4, x5, [sp], #16
    ldp     x2, x3, [sp], #16
    ldp     x0, x1, [sp], #16
    eret

_lower_irq_handler:
    b       _lower_sync_handler

_lower_fiq_handler:
    b       _lower_sync_handler

_lower_serror_handler:
    b       _lower_sync_handler

// =============================================================================
// Stack
// =============================================================================
.section .bss
.balign 16
_el2_stack_bottom:
    .space 65536                // 64KB EL2 stack
_el2_stack_top:

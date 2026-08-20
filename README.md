# Sovereign ARM64 EL2 Hypervisor

Custom execution engine for Qualcomm Snapdragon Oryon cores. Zero dependency on Apple Hypervisor.framework, Microsoft Hyper-V, or KVM.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Guest VMs                             │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────────┐  │
│  │ 6502 VM  │  │ 6502 VM  │  │   Linux/RTOS Guest   │  │
│  │ (Agent)  │  │ (Agent)  │  │   (AArch64 native)   │  │
│  └────┬─────┘  └────┬─────┘  └──────────┬───────────┘  │
│       │              │                   │              │
├───────┼──────────────┼───────────────────┼──────────────┤
│       │    DBT JIT (6502 → AArch64)      │              │
│       ▼              ▼                   ▼              │
│  ┌─────────────────────────────────────────────────┐    │
│  │            Sovereign EL2 Hypervisor              │    │
│  │                                                   │    │
│  │  ┌───────────┐ ┌──────────┐ ┌────────────────┐  │    │
│  │  │ Stage 2   │ │  vCPU    │ │  VirtIO-GPU    │  │    │
│  │  │ MMU       │ │  Trap    │ │  (Adreno       │  │    │
│  │  │ (IPA→PA)  │ │  Loop    │ │   Passthrough) │  │    │
│  │  └───────────┘ └──────────┘ └────────────────┘  │    │
│  │                                                   │    │
│  └───────────────────────────────────────────────────┘    │
│                          │                               │
├──────────────────────────┼───────────────────────────────┤
│                          ▼                               │
│  ┌─────────────────────────────────────────────────────┐ │
│  │         Snapdragon X Elite Hardware                  │ │
│  │  Oryon CPU (ARMv9) │ Adreno GPU │ NPU │ DDR5        │ │
│  └─────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

## Components

| # | Component | File | Purpose |
|---|-----------|------|---------|
| 1 | EL2 Boot | `src/boot/el2_entry.S` | ARM Exception Level 2 entry, HCR_EL2 config, vector table |
| 2 | Stage 2 MMU | `src/mmu/stage2.c` | IPA→PA translation tables, memory isolation |
| 3 | vCPU Trap Loop | `src/vcpu/trap_loop.c` | ERET/VM-exit cycle, ESR decode, MMIO dispatch |
| 4 | DBT JIT | `src/dbt/jit_engine.c` | 6502→IR→AArch64 translation, SVE2/NEON emit |
| 5 | VirtIO-GPU | `src/virtio/virtio_gpu.c` | Guest rendering → Vulkan on Adreno |

## Key Design Decisions

- **No libc, no CRT** — bare-metal from EL2 entry. Zero external dependencies.
- **6502 as agent ISA** — each sovereign agent runs in a provably-deterministic 6502 VM, JIT-compiled to native AArch64.
- **Stage 2 isolation** — VMID-tagged TLB entries prevent cross-VM memory access at hardware level.
- **Adreno direct** — VirtIO-GPU front-end translates to host Vulkan, bypassing software rendering.
- **SVE2 vectorization** — DBT emitter targets Oryon's scalable vector extensions for batch operations.

## Target Platform

- **SoC:** Qualcomm Snapdragon X Elite (X1E80100)
- **CPU:** Qualcomm Oryon (ARMv9.2-A, 12 cores, SVE2)
- **GPU:** Adreno (Vulkan 1.3)
- **Memory:** LPDDR5X
- **Boot:** UEFI → EL2 takeover

## Build

```bash
# Cross-compile for AArch64 bare-metal
aarch64-none-elf-gcc -nostdlib -nostartfiles -T linker.ld \
    -mcpu=cortex-x4 -march=armv9.2-a+sve2 \
    src/boot/el2_entry.S src/mmu/stage2.c \
    src/vcpu/trap_loop.c src/dbt/jit_engine.c \
    src/virtio/virtio_gpu.c \
    -o sovereign_hypervisor.elf

# Generate binary for UEFI boot
aarch64-none-elf-objcopy -O binary sovereign_hypervisor.elf sovereign_hypervisor.bin
```

## Relation to ORTHO-32

ORTHO-32 (PolarFire FPGA) provides **hardware-enforced** memory isolation.
This hypervisor provides **software-enforced** isolation on commodity ARM silicon.

Same invariants. Same proofs. Different enforcement substrate:
- ORTHO-32: Isolation in silicon (FPGA fabric, custom RTL)
- Sovereign Hypervisor: Isolation in ARM EL2 (Stage 2 MMU, hardware VMID tagging)

Both trace back to the same Lean 4 theorems.

## License

Tri-licensed: BSL-1.1 + AGPL-3.0 + MPL-2.0. See [LICENSE.tri](LICENSE.tri).

Copyright (C) 2026 Jessica L. Williams / SNAPKITTYWEST

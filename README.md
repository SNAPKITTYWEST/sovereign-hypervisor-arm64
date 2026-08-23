<p align="center">
  <img src="https://raw.githubusercontent.com/SNAPKITTYWEST/SNAPKITTYWEST/main/bobs-games/assets/voxel-snapkitty.svg" width="96" />
</p>

<h1 align="center">Sovereign ARM64 EL2 Hypervisor</h1>

<p align="center">
  <strong>Custom execution engine for Qualcomm Snapdragon Oryon · Zero external dependencies</strong>
</p>

<p align="center">
  <a href="LICENSE.tri"><img src="https://img.shields.io/badge/license-AGPL%20%7C%20BSL%201.1%20%7C%20MIT-blue" /></a>
  <img src="https://img.shields.io/badge/target-Snapdragon%20Oryon%20ARMv9-orange" />
  <img src="https://img.shields.io/badge/EL2-bare%20metal-critical" />
  <img src="https://img.shields.io/badge/6502-JIT%20agents-brightgreen" />
  <img src="https://img.shields.io/badge/deps-ZERO-black" />
  <img src="https://img.shields.io/badge/libc-NO-red" />
  <a href="https://github.com/SNAPKITTYWEST/sovereign-stack"><img src="https://img.shields.io/badge/stack-Sovereign%20Stack-blueviolet" /></a>
</p>

---

## What This Is

A from-scratch ARM64 EL2 hypervisor targeting Qualcomm Snapdragon Oryon cores. No Apple Hypervisor.framework. No Microsoft Hyper-V. No KVM. No libc. No CRT.

Bare metal from Exception Level 2 entry. Zero external dependencies.

Each sovereign agent runs in a provably-deterministic **6502 VM**, JIT-compiled to native AArch64 via the DBT engine. Hardware-level isolation via Stage 2 MMU with VMID-tagged TLB entries.

---

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
│  └───────────────────────────────────────────────────┘   │
│                          │                               │
├──────────────────────────┼───────────────────────────────┤
│                          ▼                               │
│  ┌─────────────────────────────────────────────────────┐ │
│  │         Snapdragon X Elite Hardware                  │ │
│  │  Oryon CPU (ARMv9) │ Adreno GPU │ NPU │ DDR5        │ │
│  └─────────────────────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

---

## Components

| # | Component | File | Purpose |
|---|-----------|------|---------|
| 1 | EL2 Boot | `src/boot/el2_entry.S` | ARM Exception Level 2 entry, HCR_EL2 config, vector table |
| 2 | Stage 2 MMU | `src/mmu/stage2.c` | IPA→PA translation tables, memory isolation |
| 3 | vCPU Trap Loop | `src/vcpu/trap_loop.c` | ERET/VM-exit cycle, ESR decode, MMIO dispatch |
| 4 | DBT JIT | `src/dbt/jit_engine.c` | 6502→IR→AArch64 translation, SVE2/NEON emit |
| 5 | VirtIO-GPU | `src/virtio/virtio_gpu.c` | Guest rendering → Vulkan on Adreno |

---

## Key Design Decisions

- **No libc, no CRT** — bare-metal from EL2 entry. Zero external dependencies.
- **6502 as agent ISA** — each sovereign agent runs in a provably-deterministic 6502 VM, JIT-compiled to native AArch64.
- **Stage 2 isolation** — VMID-tagged TLB entries prevent cross-VM memory access at hardware level.
- **Adreno direct** — VirtIO-GPU front-end translates to host Vulkan, bypassing software rendering.
- **SVE2 vectorization** — DBT emitter targets Oryon's scalable vector extensions for batch operations.

---

## The Agent Roster

Every 6502 VM guest maps to a sovereign agent. The hypervisor is their runtime.

| Agent | Role | Color | What they run on this hypervisor |
|-------|------|-------|----------------------------------|
| **ORION** | System Architect | 🔵 Blue | EL2 topology mapping, VM layout design |
| **LEMUR** | Code Engineer | 🟢 Green | DBT JIT code generation, IR optimization |
| **ECHO** | Research Analyst | 🟣 Purple | Stage 2 MMU research, memory model analysis |
| **VECTOR** | Security Analyst | 🟠 Orange | Isolation boundary verification, threat modeling |
| **NEXUS** | Penetration Tester | 🔴 Red | Red team — finds the escape paths before attackers do |
| **SAGE** | Data Scientist | 🩵 Teal | Performance telemetry, bottleneck analysis |
| **PIXEL** | UI/UX Designer | 🟡 Yellow | Dashboard, monitoring interface |
| **LUNA** | Documentation Lead | 🩷 Pink | README, ADRs, invariant documentation |
| **ATLAS** | DevOps Engineer | 🔷 Navy | CI/CD pipeline, build system |
| **TITAN** | QA Engineer | 🟤 Gold | Test harness, conformance suite |
| **PULSE** | Monitoring Specialist | 💠 Cyan | WORM-sealed telemetry, liveness checks |
| **QUANTUM** | AI/ML Engineer | 💜 Purple | Inference stack, sovereign model runtime |

Each agent runs as a 6502 guest VM. The hypervisor provides hardware-isolated execution. The WORM chain seals every output. The ERE gates catch violations before they propagate.

---

## Sovereign Stack Integration

```
Sovereign EL2 Hypervisor (this repo)
    ↓ runs
6502 Agent VMs (sovereign-trinity-kernel)
    ↓ sealed by
LOCKER WORM Chain (worm-engines)
    ↓ verified by
ML-DSA-44 post-quantum signatures
    ↓ governed by
Moorish Covenant (sovereign-covenant)
```

---

## License

Tri-license — AGPL-3.0 | BSL 1.1 → MIT | MIT  
Copyright (C) 2026 Ahmad Ali Parr, Jessica L. Williams / SNAPKITTYWEST  
Bel Esprit D'Accord Irrevocable Trust

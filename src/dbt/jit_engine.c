#include "sov_hypervisor.h"

// =============================================================================
// Dynamic Binary Translation (DBT) JIT Engine
// =============================================================================
// Translates foreign instruction blocks to native AArch64 for Oryon cores.
// Basic-block JIT: parse → IR → optimize → emit native code.
// Optimized for Snapdragon SVE2/NEON vector extensions.
// =============================================================================

#define JIT_CACHE_SIZE      (64 * 1024 * 1024)  // 64MB per guest
#define MAX_BASIC_BLOCKS    65536
#define MAX_IR_OPS          4096
#define BLOCK_MAX_INSNS     256

// IR opcode (platform-neutral intermediate)
typedef enum {
    IR_NOP,
    IR_LOAD8,  IR_LOAD16, IR_LOAD32, IR_LOAD64,
    IR_STORE8, IR_STORE16, IR_STORE32, IR_STORE64,
    IR_ADD, IR_SUB, IR_MUL, IR_DIV,
    IR_AND, IR_OR, IR_XOR, IR_NOT,
    IR_SHL, IR_SHR, IR_SAR,
    IR_CMP, IR_TEST,
    IR_BR, IR_BR_COND, IR_CALL, IR_RET,
    IR_MOV, IR_MOVZ, IR_MOVK,
    IR_SEXT8, IR_SEXT16, IR_SEXT32,
    IR_ZEXT8, IR_ZEXT16, IR_ZEXT32,
    // 6502-specific (for sovereign agent VMs)
    IR_6502_LDA, IR_6502_STA, IR_6502_ADC, IR_6502_SBC,
    IR_6502_AND, IR_6502_ORA, IR_6502_EOR,
    IR_6502_ASL, IR_6502_LSR, IR_6502_ROL, IR_6502_ROR,
    IR_6502_BCC, IR_6502_BCS, IR_6502_BEQ, IR_6502_BNE,
    IR_6502_JMP, IR_6502_JSR, IR_6502_RTS,
    IR_6502_INC, IR_6502_DEC,
    IR_6502_TAX, IR_6502_TXA, IR_6502_TAY, IR_6502_TYA,
    IR_6502_PHA, IR_6502_PLA, IR_6502_PHP, IR_6502_PLP,
    IR_6502_BRK, IR_6502_NOP,
    // Vector (SVE2/NEON)
    IR_VEC_ADD, IR_VEC_MUL, IR_VEC_FMA,
    IR_VEC_LOAD, IR_VEC_STORE,
} ir_opcode_t;

// IR operand
typedef struct {
    enum { OP_REG, OP_IMM, OP_MEM, OP_LABEL } type;
    union {
        uint32_t reg;
        uint64_t imm;
        struct { uint32_t base; int64_t offset; } mem;
        uint32_t label;
    };
} ir_operand_t;

// IR instruction
typedef struct {
    ir_opcode_t   opcode;
    ir_operand_t  dst;
    ir_operand_t  src1;
    ir_operand_t  src2;
    uint8_t       width;    // 1, 2, 4, 8 bytes
    uint8_t       flags;    // Updates condition flags?
} ir_insn_t;

// Basic block (translated unit)
typedef struct {
    uint64_t    guest_pc;       // Starting guest PC
    uint32_t    guest_size;     // Bytes of guest code covered
    uint64_t    native_addr;    // Address in JIT cache
    uint32_t    native_size;    // Bytes of emitted native code
    uint32_t    exec_count;     // Hotness counter
    ir_insn_t   ir[MAX_IR_OPS]; // IR representation
    uint32_t    ir_count;
} jit_block_t;

// Block lookup table (hash map: guest_pc → block)
typedef struct {
    uint64_t    key;
    jit_block_t *block;
} block_entry_t;

static block_entry_t block_table[MAX_BASIC_BLOCKS];
static uint32_t      block_count;
static uint8_t      *jit_emit_ptr;

// =============================================================================
// 6502 Decoder — Translate 6502 opcodes to IR
// =============================================================================

static uint32_t decode_6502_block(const uint8_t *code, uint64_t pc,
                                  ir_insn_t *ir, uint32_t max_ir) {
    uint32_t ir_count = 0;
    uint32_t offset = 0;

    while (ir_count < max_ir && offset < BLOCK_MAX_INSNS) {
        uint8_t opcode = code[offset++];
        ir_insn_t *insn = &ir[ir_count];

        switch (opcode) {
        case 0xA9: // LDA immediate
            insn->opcode = IR_6502_LDA;
            insn->src1.type = OP_IMM;
            insn->src1.imm = code[offset++];
            insn->width = 1;
            insn->flags = 1;
            ir_count++;
            break;

        case 0x85: // STA zero page
            insn->opcode = IR_6502_STA;
            insn->dst.type = OP_MEM;
            insn->dst.mem.base = 0;
            insn->dst.mem.offset = code[offset++];
            insn->width = 1;
            ir_count++;
            break;

        case 0x69: // ADC immediate
            insn->opcode = IR_6502_ADC;
            insn->src1.type = OP_IMM;
            insn->src1.imm = code[offset++];
            insn->width = 1;
            insn->flags = 1;
            ir_count++;
            break;

        case 0xE9: // SBC immediate
            insn->opcode = IR_6502_SBC;
            insn->src1.type = OP_IMM;
            insn->src1.imm = code[offset++];
            insn->width = 1;
            insn->flags = 1;
            ir_count++;
            break;

        case 0x4C: // JMP absolute
            insn->opcode = IR_6502_JMP;
            insn->dst.type = OP_IMM;
            insn->dst.imm = code[offset] | (code[offset+1] << 8);
            offset += 2;
            ir_count++;
            goto block_end;

        case 0x20: // JSR absolute
            insn->opcode = IR_6502_JSR;
            insn->dst.type = OP_IMM;
            insn->dst.imm = code[offset] | (code[offset+1] << 8);
            offset += 2;
            ir_count++;
            goto block_end;

        case 0x60: // RTS
            insn->opcode = IR_6502_RTS;
            ir_count++;
            goto block_end;

        case 0xF0: // BEQ relative
            insn->opcode = IR_6502_BEQ;
            insn->dst.type = OP_IMM;
            insn->dst.imm = pc + offset + 1 + (int8_t)code[offset];
            offset++;
            ir_count++;
            goto block_end;

        case 0xD0: // BNE relative
            insn->opcode = IR_6502_BNE;
            insn->dst.type = OP_IMM;
            insn->dst.imm = pc + offset + 1 + (int8_t)code[offset];
            offset++;
            ir_count++;
            goto block_end;

        case 0x00: // BRK
            insn->opcode = IR_6502_BRK;
            ir_count++;
            goto block_end;

        case 0xEA: // NOP
            insn->opcode = IR_6502_NOP;
            ir_count++;
            break;

        default:
            // Unknown opcode — end block
            goto block_end;
        }
    }

block_end:
    return ir_count;
}

// =============================================================================
// Native Code Emitter — IR → AArch64
// =============================================================================

// AArch64 instruction encoding helpers
static inline uint32_t aarch64_movz(uint8_t rd, uint16_t imm16, uint8_t shift) {
    return 0xD2800000 | ((uint32_t)shift << 21) | ((uint32_t)imm16 << 5) | rd;
}

static inline uint32_t aarch64_add_imm(uint8_t rd, uint8_t rn, uint16_t imm12) {
    return 0x91000000 | ((uint32_t)imm12 << 10) | ((uint32_t)rn << 5) | rd;
}

static inline uint32_t aarch64_sub_imm(uint8_t rd, uint8_t rn, uint16_t imm12) {
    return 0xD1000000 | ((uint32_t)imm12 << 10) | ((uint32_t)rn << 5) | rd;
}

static inline uint32_t aarch64_strb(uint8_t rt, uint8_t rn, uint16_t offset) {
    return 0x39000000 | ((uint32_t)offset << 10) | ((uint32_t)rn << 5) | rt;
}

static inline uint32_t aarch64_ldrb(uint8_t rt, uint8_t rn, uint16_t offset) {
    return 0x39400000 | ((uint32_t)offset << 10) | ((uint32_t)rn << 5) | rt;
}

static inline uint32_t aarch64_ret(void) {
    return 0xD65F03C0;
}

static void emit32(uint32_t insn) {
    *(uint32_t *)jit_emit_ptr = insn;
    jit_emit_ptr += 4;
}

// Emit native AArch64 for a 6502 IR block
// Register allocation:
//   w0 = A (accumulator)
//   w1 = X index
//   w2 = Y index
//   w3 = SP (stack pointer, 8-bit)
//   w4 = P (status flags: NV-BDIZC)
//   x5 = memory base pointer (64KB 6502 address space)
static uint32_t emit_6502_block(ir_insn_t *ir, uint32_t ir_count) {
    uint8_t *start = jit_emit_ptr;

    for (uint32_t i = 0; i < ir_count; i++) {
        ir_insn_t *insn = &ir[i];

        switch (insn->opcode) {
        case IR_6502_LDA:
            if (insn->src1.type == OP_IMM) {
                emit32(aarch64_movz(0, (uint16_t)insn->src1.imm, 0));
            }
            break;

        case IR_6502_STA:
            if (insn->dst.type == OP_MEM) {
                emit32(aarch64_strb(0, 5, (uint16_t)insn->dst.mem.offset));
            }
            break;

        case IR_6502_ADC:
            if (insn->src1.type == OP_IMM) {
                emit32(aarch64_add_imm(0, 0, (uint16_t)insn->src1.imm));
            }
            break;

        case IR_6502_SBC:
            if (insn->src1.type == OP_IMM) {
                emit32(aarch64_sub_imm(0, 0, (uint16_t)insn->src1.imm));
            }
            break;

        case IR_6502_NOP:
            break;

        case IR_6502_RTS:
        case IR_6502_BRK:
            emit32(aarch64_ret());
            break;

        default:
            // Stub — emit NOP for unhandled IR
            emit32(0xD503201F);  // NOP
            break;
        }
    }

    // Ensure block ends with return
    if (ir_count > 0 && ir[ir_count-1].opcode != IR_6502_RTS &&
        ir[ir_count-1].opcode != IR_6502_BRK) {
        emit32(aarch64_ret());
    }

    return (uint32_t)(jit_emit_ptr - start);
}

// =============================================================================
// Public API
// =============================================================================

int sov_dbt_init(sov_guest_t *guest, uint64_t cache_size) {
    if (cache_size == 0) cache_size = JIT_CACHE_SIZE;

    // In bare-metal: allocate from physical memory pool
    // The JIT cache must be mapped as RWX in Stage 2
    guest->jit_cache_size = cache_size;
    // guest->jit_cache = sov_alloc_pages(cache_size / SOV_PAGE_SIZE);
    // Placeholder — real implementation maps executable pages
    guest->jit_cache = (void *)0;

    block_count = 0;
    jit_emit_ptr = (uint8_t *)guest->jit_cache;

    return 0;
}

int sov_dbt_translate_block(sov_guest_t *guest, uint64_t guest_pc,
                            void **native_code, uint64_t *code_size) {
    // Check cache first
    uint32_t hash = (uint32_t)(guest_pc >> 2) & (MAX_BASIC_BLOCKS - 1);
    if (block_table[hash].key == guest_pc && block_table[hash].block) {
        jit_block_t *blk = block_table[hash].block;
        blk->exec_count++;
        *native_code = (void *)blk->native_addr;
        *code_size = blk->native_size;
        return 0;
    }

    // Miss — translate new block
    // Read guest memory at guest_pc (via Stage 2 walk or direct mapping)
    const uint8_t *guest_code = (const uint8_t *)(uintptr_t)guest_pc;

    // Allocate block
    static jit_block_t blocks[MAX_BASIC_BLOCKS];
    jit_block_t *blk = &blocks[block_count++];
    blk->guest_pc = guest_pc;
    blk->exec_count = 1;
    blk->native_addr = (uint64_t)jit_emit_ptr;

    // Decode to IR
    blk->ir_count = decode_6502_block(guest_code, guest_pc,
                                      blk->ir, MAX_IR_OPS);

    // Emit native code
    blk->native_size = emit_6502_block(blk->ir, blk->ir_count);

    // Cache it
    block_table[hash].key = guest_pc;
    block_table[hash].block = blk;

    *native_code = (void *)blk->native_addr;
    *code_size = blk->native_size;

    return 0;
}

void sov_dbt_invalidate(sov_guest_t *guest, uint64_t guest_addr, uint64_t size) {
    // Invalidate any cached blocks that overlap [guest_addr, guest_addr+size)
    for (uint32_t i = 0; i < MAX_BASIC_BLOCKS; i++) {
        if (block_table[i].block) {
            jit_block_t *blk = block_table[i].block;
            if (blk->guest_pc >= guest_addr &&
                blk->guest_pc < guest_addr + size) {
                block_table[i].key = 0;
                block_table[i].block = 0;
            }
        }
    }

    // Flush instruction cache for invalidated regions
    __asm__ volatile("ic iallu\nisb\n" ::: "memory");
}

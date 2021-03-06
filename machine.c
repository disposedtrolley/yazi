#include "machine.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

enum OpcodeKind {
    OpcodeKind_0OP,
    OpcodeKind_1OP,
    OpcodeKind_2OP,
    OpcodeKind_VAR,
    OpcodeKind_EXT,
};

typedef struct Instruction {
    enum OpcodeKind opcode_kind;
    uint8_t opcode_number;
    uint8_t n_operands;
    uint16_t operands[8];
    uint8_t pc_incr;
} Instruction;

// v3
static const uint8_t HEADER_STATUS_TYPE = (1U << 1);
static const uint8_t HEADER_CENSOR = (1U << 3);
static const uint8_t HEADER_STATUS_NOT_AVAILABLE = (1U << 4);
static const uint8_t HEADER_UPPER_WINDOW_AVAILABLE = (1U << 5);
static const uint8_t HEADER_FONT_VARIABLE = (1U << 6);

// v4+
static const uint8_t HEADER_COLOURS_AVAILABLE = (1U << 0);
static const uint8_t HEADER_PICTURES_AVAILABLE = (1U << 1);
static const uint8_t HEADER_BOLDFACE_AVAILABLE = (1U << 2);
static const uint8_t HEADER_EMPHASIS_AVAILABLE = (1U << 3);
static const uint8_t HEADER_FIXEDWIDTH_AVAILABLE = (1U << 4);
static const uint8_t HEADER_SOUND_AVAILABLE = (1U << 5);

uint8_t memory_read_byte(Machine *m, uint32_t addr) {
    assert(addr < m->memory_size);
    return m->memory[addr];
}

uint16_t memory_read_word(Machine *m, uint32_t addr) {
    assert(addr < m->memory_size-1);
    return m->memory[addr] << 8 | m->memory[addr + 1];
}

void memory_write_byte(Machine *m, uint32_t addr, uint8_t data) {
    assert(addr < m->memory_size);
    m->memory[addr] = data;
}

void memory_write_word(Machine *m, uint32_t addr, uint16_t data) {
    assert(addr < m->memory_size-1);
    m->memory[addr] = data >> 8;
    m->memory[addr+1] = (uint8_t) data;
}

void initialise_stack(Machine *m) {
    // §6.3.3 of the spec provides two example stack sizes. We're using the
    // nfrotz stack size of 61440 bytes, which is 0x30 (sizeof Frame) * 0x500.
    m->stack = (Frame*)malloc(sizeof(Frame) * 0x500);
}

ZRet push(Machine *m, Frame f) {
    m->stack[m->sp] = f;
    m->sp++;

    return ZRet_Success;
}

ZRet initialise_irom(Machine *m, Config config, uint8_t zversion) {
    // Screen dimensions
    if (zversion >= 4) {
        memory_write_byte(m, 0x1E, config.interpreter_number);
        memory_write_byte(m, 0x1F, config.interpreter_version);
        memory_write_byte(m, 0x20, config.screen_height);
        memory_write_byte(m, 0x21, config.screen_width);
    }

    if (zversion >= 5) {
        // The "units" measurement appears to exist for the purposes of
        // displaying pictures. We're not concerned about that now, so it's
        // probably safe to stick with the humble dimenions of 80x24.
        memory_write_word(m, 0x22, config.screen_width);
        memory_write_word(m, 0x24, config.screen_height);
    }

    return ZRet_Success;
}

uint16_t get_variable(Machine *m, uint8_t var) {
    /*
     * Variable number $00 refers to the top of the stack,
     * $01 to $0f mean the local variables of the current
     * routine and $10 to $ff mean the global variables.
     *
     * It is illegal to refer to local variables which do
     * not exist for the current routine (there may even be
     * none).
     */

    return 0;
}

Instruction decode_ins_long(Machine *m, uint8_t opcode) {
    uint16_t offset = m->pc;

    Instruction parsed = {
            .opcode_kind = OpcodeKind_2OP,
            .opcode_number = opcode &0x1F,
            .n_operands = 2,
    };

    uint8_t operand_types[2] = {(opcode >> 6) & 0b1, (opcode >> 5) & 0b1};
    for (size_t i = 0; i < sizeof operand_types; i++) {
        if (operand_types[i] == 0b0) {
            // Byte constant
            parsed.operands[i] = memory_read_byte(m, offset+1);
        } else {
            // Byte variable
            parsed.operands[i] = get_variable(m, memory_read_byte(m, offset+1));
        }
    }

    return parsed;
}

Instruction decode_ins_extended(Machine *m, uint8_t opcode) {
    uint16_t offset = m->pc;

    Instruction parsed = {
            .opcode_kind = OpcodeKind_EXT,
            .opcode_number = memory_read_byte(m, offset+1),
    };

    uint8_t operand_types_bitfield = memory_read_byte(m, offset+2);
    uint8_t iter_offset = 0;
    for (size_t shift = 6; shift >= 0; shift-=2) {
        uint8_t type = operand_types_bitfield >> shift & 0b11;
        if (type == 0b11) break;

        switch (type) {
            case 0b00:
                parsed.operands[parsed.n_operands] = memory_read_word(m, offset+3+iter_offset);
                parsed.pc_incr = 4;
                iter_offset += 2;
                break;
            case 0b01:
                parsed.operands[parsed.n_operands] = memory_read_byte(m, offset+3+iter_offset);
                parsed.pc_incr = 3;
                iter_offset += 1;
                break;
            case 0b10:
                get_variable(m, offset+3+iter_offset);
                parsed.pc_incr = 3;
                iter_offset += 1;
                break;
        }

        parsed.n_operands += 1;
    }

    return parsed;
}

Instruction decode_ins_variable(Machine *m, uint8_t opcode) {
    uint16_t offset = m->pc;

    Instruction parsed = {
            .opcode_kind = OpcodeKind_VAR,
            .opcode_number = opcode & 0x1F,
    };

    // There used to be a condition here to check bit 5 to determine whether
    // this variable form instruction is a 2OP (see §4.3.3), but I don't think
    // it's strictly required as we're going to need to iterate through the
    // operand types bitfield which will tell us how many to expect.

    uint8_t operand_types_bitfield = memory_read_byte(m, offset+1);
    uint8_t iter_offset = 0;
    for (size_t shift = 6; shift >= 0; shift-=2) {
        uint8_t type = operand_types_bitfield >> shift & 0b11;
        if (type == 0b11) break;

        switch (type) {
            case 0b00:
                parsed.operands[parsed.n_operands] = memory_read_word(m, offset+2+iter_offset);
                parsed.pc_incr = 4;
                iter_offset += 2;
                break;
            case 0b01:
                parsed.operands[parsed.n_operands] = memory_read_byte(m, offset+2+iter_offset);
                parsed.pc_incr = 3;
                iter_offset += 1;
                break;
            case 0b10:
                get_variable(m, offset+2+iter_offset);
                parsed.pc_incr = 3;
                iter_offset += 1;
                break;
        }

        parsed.n_operands += 1;
    }

    return parsed;
}

Instruction decode_ins_short(Machine *m, uint8_t opcode) {
    uint16_t offset = m->pc;

    Instruction parsed = {
            .opcode_number = opcode & 0xF,
    };

    // Bits 4 and 5
    if ((opcode >> 4 & 0x3) == 0x3) {
        parsed.opcode_kind = OpcodeKind_0OP;
        parsed.n_operands = 0;
    } else {
        parsed.opcode_kind = OpcodeKind_1OP;
        parsed.n_operands = 1;

        uint8_t operand_type = (opcode >> 4) & 0x3;
        if (operand_type == 0b00) {
            // Word constant
            parsed.operands[0] = memory_read_word(m, offset+1);
            parsed.pc_incr = 3;
        } else if (operand_type == 0b01) {
            // Byte constant
            parsed.operands[0] = memory_read_byte(m, offset+1);
            parsed.pc_incr = 2;
        } else if (operand_type == 0b10) {
            // Byte variable
            parsed.operands[0] = get_variable(m, memory_read_byte(m, offset+1));
            parsed.pc_incr = 2;
        }
    }
    return parsed;
}

Instruction decode(Machine *m, uint16_t offset, uint8_t zversion) {
    uint8_t opcode = memory_read_byte(m, offset);

    uint8_t opcode_top_bits = opcode >> 6;
    if ((opcode_top_bits ^ 0b11) == 0) {
        return decode_ins_variable(m, opcode);
    } else if ((opcode_top_bits ^ 0b10) == 0) {
        return decode_ins_short(m, opcode);
    } else if ((opcode_top_bits ^ 0xBE) == 0 && zversion >= 5) {
        return decode_ins_extended(m, opcode);
    } else {
        return decode_ins_long(m, opcode);
    }
}

ZRet start_game_loop(Machine *m, Config config, uint8_t zversion, uint8_t zversion_specific) {
    // IROM
    initialise_irom(m, config, zversion);

    // Stack
    initialise_stack(m);

    // Program Counter
    uint16_t initial_pc = memory_read_word(m, 0x06);
    push(m, (Frame){.return_pc = initial_pc});
    m->pc = initial_pc;

    Instruction instruction = decode(m, m->pc, zversion);

    return ZRet_Success;
}
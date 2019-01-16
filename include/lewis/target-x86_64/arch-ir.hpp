// Copyright the lewis authors (AUTHORS.md) 2018
// SPDX-License-Identifier: MIT

#pragma once

#include <lewis/ir.hpp>

namespace lewis::targets::x86_64 {

namespace arch_value_kinds {
    enum : ValueKindType {
        unused = value_kinds::kindsForX86,
        modeMResult
    };
}

namespace arch_instruction_kinds {
    // Convention: x86 instructions follow the naming scheme <opcode>_<operands>.
    // Operands are encoded as single letters, as follows:
    // M: Register or memory reference (ModRM + SIB).
    // R: Register reference.
    // C: Immediate constant.
    enum : InstructionKindType {
        unused = instruction_kinds::kindsForX86,
        pseudoMovMR,
        xchgMR,
        movMC,
        movMR,
        // TODO: We certainly want to drop the "WithOffset" specialization.
        //       This should probably be done after we rewrite Values and make them more useful.
        movRMWithOffset,
        negM,
        addMR,
        andMR,
        call,
    };
}

namespace arch_phi_kinds {
    enum : PhiKindType {
        unused = phi_kinds::kindsForX86,
        modeRArgument,
        modeMDataFlow
    };
}

namespace arch_branch_kinds {
    enum : BranchKindType {
        unused = instruction_kinds::kindsForX86,
        ret,
        jmp,
    };
}

struct ModeMValue
: Value,
        CastableIfValueKind<ModeMValue, arch_value_kinds::modeMResult> {
    ModeMValue()
    : Value{arch_value_kinds::modeMResult} { }

    int modeRegister = -1;
};

struct ModeRArgumentPhi
: PhiNode,
        CastableIfPhiKind<ModeRArgumentPhi, arch_phi_kinds::modeRArgument> {
    ModeRArgumentPhi()
    : PhiNode{arch_phi_kinds::modeRArgument} { }

    int modeRegister = -1;
};

struct ModeMDataFlowPhi
: PhiNode,
        CastableIfPhiKind<ModeMDataFlowPhi, arch_phi_kinds::modeMDataFlow> {
    ModeMDataFlowPhi()
    : PhiNode{arch_phi_kinds::modeMDataFlow} { }

    int modeRegister = -1;
};

// Instruction that takes a single operand and overwrites a mode M result.
struct UnaryMOverwriteInstruction
: Instruction,
        CastableIfInstructionKind<UnaryMOverwriteInstruction,
                arch_instruction_kinds::pseudoMovMR,
                arch_instruction_kinds::movMR,
                arch_instruction_kinds::movRMWithOffset> {
    UnaryMOverwriteInstruction(InstructionKindType kind, Value *operand_ = nullptr)
    : Instruction{kind}, operand{this, operand_} { }

    ValueOrigin result;
    ValueUse operand;
};

// Instruction that takes a single mode M operand and replaces it by the result.
struct UnaryMInPlaceInstruction
: Instruction,
        CastableIfInstructionKind<UnaryMInPlaceInstruction, arch_instruction_kinds::negM> {
    UnaryMInPlaceInstruction(InstructionKindType kind, Value *primary_ = nullptr)
    : Instruction{kind}, primary{this, primary_} { }

    ValueOrigin result;
    ValueUse primary;
};

struct BinaryMRInPlaceInstruction
: Instruction, CastableIfInstructionKind<BinaryMRInPlaceInstruction,
        arch_instruction_kinds::addMR,
        arch_instruction_kinds::andMR> {
    BinaryMRInPlaceInstruction(InstructionKindType kind,
            Value *primary_ = nullptr, Value *secondary_ = nullptr)
    : Instruction{kind}, primary{this, primary_}, secondary{this, secondary_} { }

    ValueOrigin result;
    ValueUse primary;
    ValueUse secondary;
};

struct PseudoMovMRInstruction
: UnaryMOverwriteInstruction,
        CastableIfInstructionKind<PseudoMovMRInstruction, arch_instruction_kinds::pseudoMovMR> {
    PseudoMovMRInstruction(Value *operand_ = nullptr)
    : UnaryMOverwriteInstruction{arch_instruction_kinds::pseudoMovMR, operand_} { }
};

// TODO: Turn this into a UnaryMOverwriteInstruction.
struct MovMCInstruction
: Instruction,
        CastableIfInstructionKind<MovMCInstruction, arch_instruction_kinds::movMC> {
    MovMCInstruction()
    : Instruction{arch_instruction_kinds::movMC} { }

    ValueOrigin result;
    uint64_t value = 0;
};

struct MovMRInstruction
: UnaryMOverwriteInstruction,
        CastableIfInstructionKind<MovMRInstruction, arch_instruction_kinds::movMR> {
    MovMRInstruction(Value *operand_ = nullptr)
    : UnaryMOverwriteInstruction{arch_instruction_kinds::movMR, operand_} { }
};

struct MovRMWithOffsetInstruction
: UnaryMOverwriteInstruction,
        CastableIfInstructionKind<MovRMWithOffsetInstruction,
                arch_instruction_kinds::movRMWithOffset> {
    MovRMWithOffsetInstruction(Value *operand_ = nullptr, int32_t offset_ = 0)
    : UnaryMOverwriteInstruction{arch_instruction_kinds::movRMWithOffset, operand_},
            offset{offset_} { }

    int32_t offset;
};

struct XchgMRInstruction
: Instruction,
        CastableIfInstructionKind<XchgMRInstruction, arch_instruction_kinds::xchgMR>{

    XchgMRInstruction(Value *first = nullptr, Value *second = nullptr)
    : Instruction{arch_instruction_kinds::xchgMR},
        firstOperand{this, first}, secondOperand{this, second} { }

    ModeMValue *firstResult() {
        return &_firstResult;
    }

    ModeMValue *secondResult() {
        return &_secondResult;
    }

    ValueUse firstOperand;
    ValueUse secondOperand;

private:
    ModeMValue _firstResult;
    ModeMValue _secondResult;
};

struct NegMInstruction
: UnaryMInPlaceInstruction,
        CastableIfInstructionKind<NegMInstruction, arch_instruction_kinds::negM> {
    NegMInstruction(Value *primary_ = nullptr)
    : UnaryMInPlaceInstruction{arch_instruction_kinds::negM, primary_} { }
};

struct AddMRInstruction
: BinaryMRInPlaceInstruction,
        CastableIfInstructionKind<AddMRInstruction, arch_instruction_kinds::addMR> {
    AddMRInstruction(Value *primary_ = nullptr, Value *secondary_ = nullptr)
    : BinaryMRInPlaceInstruction{arch_instruction_kinds::addMR, primary_, secondary_} { }
};

struct AndMRInstruction
: BinaryMRInPlaceInstruction,
        CastableIfInstructionKind<AndMRInstruction, arch_instruction_kinds::andMR> {
    AndMRInstruction(Value *primary_ = nullptr, Value *secondary_ = nullptr)
    : BinaryMRInPlaceInstruction{arch_instruction_kinds::andMR, primary_, secondary_} { }
};

struct CallInstruction
: Instruction,
        CastableIfInstructionKind<CallInstruction, arch_instruction_kinds::call> {
    CallInstruction(Value *operand_ = nullptr)
    : Instruction{arch_instruction_kinds::call}, operand{this, operand_} { }

    std::string function;
    ValueOrigin result;
    ValueUse operand;
};

struct RetBranch
: Branch,
        CastableIfBranchKind<RetBranch, arch_branch_kinds::ret> {
    RetBranch()
    : Branch{arch_branch_kinds::ret} { }
};

struct JmpBranch
: Branch,
        CastableIfBranchKind<JmpBranch, arch_branch_kinds::jmp> {
    JmpBranch(BasicBlock *target_ = nullptr)
    : Branch{arch_branch_kinds::jmp}, target{target_} { }

    // TODO: Use a BlockLink class similar to ValueUse.
    BasicBlock *target;
};

} // namespace lewis::targets::x86_64

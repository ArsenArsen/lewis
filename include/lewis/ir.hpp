// Copyright the lewis authors (AUTHORS.md) 2018
// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <algorithm>
#include <memory>
#include <vector>
#include <frg/list.hpp>
#include <lewis/hierarchy.hpp>

namespace lewis {

struct Value;
struct Instruction;
struct BasicBlock;

//---------------------------------------------------------------------------------------
// ValueUse class to represent "uses" of a Value.
//---------------------------------------------------------------------------------------

// Represents a single "use" of a Value. This is necessary to support Value replacement.
// For convenience, this class also has a Value pointer-like interface.
struct ValueUse {
    friend struct Value;

    ValueUse(Instruction *inst)
    : _inst{inst}, _ref{nullptr} { }

    ValueUse(Instruction *inst, Value *v)
    : _inst{inst}, _ref{nullptr} {
        assign(v);
    }

    ValueUse(const ValueUse &) = delete;

    ValueUse &operator= (const ValueUse &) = delete;

    Instruction *instruction() {
        return _inst;
    }

    void assign(Value *v);

    Value *get() {
        return _ref;
    }

    // The following operators define the pointer-like interface.

    ValueUse &operator= (Value *v) {
        assign(v);
        return *this;
    }

    bool operator== (Value *v) { return _ref == v; }
    bool operator!= (Value *v) { return _ref != v; }

    explicit operator bool () { return _ref; }

    Value &operator* () { return *_ref; }
    Value *operator-> () { return _ref; }

private:
    Instruction *_inst;
    Value *_ref;
    frg::default_list_hook<ValueUse> _useListHook;
};

//---------------------------------------------------------------------------------------
// Value base class.
//---------------------------------------------------------------------------------------

// Defines an ID for each subclass of Value.
using ValueKindType = uint32_t;

namespace value_kinds {
    enum : ValueKindType {
        null,
        phi,
        genericResult,

        // Give each architecture 16k values; that should be enough.
        kindsForX86 = 1 << 14
    };
}

struct Value {
    friend struct ValueUse;

    using UseList = frg::intrusive_list<
        ValueUse,
        frg::locate_member<
            ValueUse,
            frg::default_list_hook<ValueUse>,
            &ValueUse::_useListHook
        >
    >;

    struct UseRange {
        UseRange(Value *v)
        : _v{v} { }

        auto begin() { return _v->_useList.begin(); }
        auto end() { return _v->_useList.end(); }

    private:
        Value *_v;
    };

    using UseIterator = UseList::iterator;

    Value(ValueKindType valueKind_)
    : valueKind{valueKind_} { }

    UseRange uses() {
        return UseRange{this};
    }

    void replaceAllUses(Value *other);

    const ValueKindType valueKind;

private:
    // Linked list of all uses of this Value.
    UseList _useList;
};

// Template magic to enable hierarchy_cast<>.
template<ValueKindType K>
struct IsValueKind {
    bool operator() (Value *p) {
        return p->valueKind == K;
    }
};

// Template magic to enable hierarchy_cast<>.
template<typename T, ValueKindType K>
struct CastableIfValueKind : Castable<T, IsValueKind<K>> { };

//---------------------------------------------------------------------------------------
// Instruction base class.
//---------------------------------------------------------------------------------------

// Defines an ID for each subclass of Instruction.
using InstructionKindType = uint32_t;

namespace instruction_kinds {
    enum : InstructionKindType {
        null,
        loadConst,
        unaryMath,

        // Give each architecture 16k instructions; that should be enough.
        kindsForX86 = 1 << 14
    };
}

struct Instruction {
    friend struct BasicBlock;

    Instruction(InstructionKindType kind_)
    : kind{kind_} { }

    const InstructionKindType kind;

private:
    frg::default_list_hook<Instruction> _instListHook;
};

// Template magic to enable hierarchy_cast<>.
template<InstructionKindType... S>
struct IsInstructionKind {
    bool operator() (Instruction *p) {
        std::array<InstructionKindType, sizeof...(S)> kinds{S...};
        return std::find(kinds.begin(), kinds.end(), p->kind) != kinds.end();
    }
};

// Specialization for the simple case.
template<InstructionKindType K>
struct IsInstructionKind<K> {
    bool operator() (Instruction *p) {
        return p->kind == K;
    }
};

// Template magic to enable hierarchy_cast<>.
template<typename T, InstructionKindType... S>
struct CastableIfInstructionKind : Castable<T, IsInstructionKind<S...>> { };

//---------------------------------------------------------------------------------------
// Branch base class.
//---------------------------------------------------------------------------------------

// Defines an ID for each subclass of Branch.
using BranchKindType = uint32_t;

namespace branch_kinds {
    enum : BranchKindType {
        null,
        functionReturn,
        unconditional,

        // Give each architecture 16k branches; that should be enough.
        kindsForX86 = 1 << 14
    };
}

struct Branch {
    Branch(BranchKindType kind_)
    : kind{kind_} { }

    const BranchKindType kind;
};

template<BranchKindType K>
struct IsBranchKind {
    bool operator() (Branch *p) {
        return p->kind == K;
    }
};

// Template magic to enable hierarchy_cast<>.
template<typename T, BranchKindType... S>
struct CastableIfBranchKind : Castable<T, IsBranchKind<S...>> { };

struct FunctionReturnBranch
: Branch,
        CastableIfBranchKind<FunctionReturnBranch, branch_kinds::functionReturn> {
    FunctionReturnBranch()
    : Branch{branch_kinds::functionReturn} { }
};

struct UnconditionalBranch
: Branch,
        CastableIfBranchKind<UnconditionalBranch, branch_kinds::unconditional> {
    UnconditionalBranch(BasicBlock *target_ = nullptr)
    : Branch{branch_kinds::unconditional}, target{target_} { }

    // TODO: Use a BlockLink class similar to ValueUse.
    BasicBlock *target;
};

//---------------------------------------------------------------------------------------
// BasicBlock class and Phi classes.
//---------------------------------------------------------------------------------------

// Defines an ID for each subclass of PhiNode.
using PhiKindType = uint32_t;

namespace phi_kinds {
    enum : PhiKindType {
        null,
        generic,

        // Give each architecture 16k values; that should be enough.
        kindsForX86 = 1 << 14
    };
}

struct PhiEdge {
    friend struct PhiNode;

    // TODO: Do not pass nullptr as an Instruction to the ValueUse.
    PhiEdge(BasicBlock *source_ = nullptr, Value *alias_ = nullptr)
    : source{source_}, alias{nullptr, alias_} { }

    // TODO: Use a BlockBacklink class.
    BasicBlock *source;
    ValueUse alias;

private:
    frg::default_list_hook<PhiEdge> _edgeListHook;
};

struct PhiNode
: Value,
        CastableIfValueKind<PhiNode, value_kinds::phi> {
    friend struct BasicBlock;

    using EdgeList = frg::intrusive_list<
        PhiEdge,
        frg::locate_member<
            PhiEdge,
            frg::default_list_hook<PhiEdge>,
            &PhiEdge::_edgeListHook
        >
    >;

    using EdgeIterator = EdgeList::iterator;

    struct EdgeRange {
        EdgeRange(PhiNode *node)
        : _node{node} { }

        EdgeIterator begin() {
            return _node->_edges.begin();
        }
        EdgeIterator end() {
            return _node->_edges.end();
        }

    private:
        PhiNode *_node;
    };

    PhiNode(PhiKindType phiKind_)
    : Value{value_kinds::phi}, phiKind{phiKind_} { }

    EdgeRange edges() {
        return EdgeRange{this};
    }

    PhiEdge *attachEdge(std::unique_ptr<PhiEdge> edge) {
        auto ptr = edge.get();
        _edges.push_back(edge.release());
        return ptr;
    }

    const PhiKindType phiKind;

private:
    frg::default_list_hook<PhiNode> _phiListHook;

    EdgeList _edges;
};

// Template magic to enable hierarchy_cast<>.
template<PhiKindType K>
struct IsPhiKind {
    bool operator() (PhiNode *p) {
        return p->phiKind == K;
    }
};

// Template magic to enable hierarchy_cast<>.
template<typename T, PhiKindType... S>
struct CastableIfPhiKind : Castable<T, IsPhiKind<S...>> { };

struct GenericPhiNode : PhiNode {
    GenericPhiNode()
    : PhiNode{phi_kinds::generic} { }
};

struct BasicBlock {
    friend struct Function;

    using PhiList = frg::intrusive_list<
        PhiNode,
        frg::locate_member<
            PhiNode,
            frg::default_list_hook<PhiNode>,
            &PhiNode::_phiListHook
        >
    >;

    using PhiIterator = PhiList::iterator;

    struct PhiRange {
        PhiRange(BasicBlock *bb)
        : _bb{bb} { }

        PhiIterator begin() {
            return _bb->_phis.begin();
        }
        PhiIterator end() {
            return _bb->_phis.end();
        }

    private:
        BasicBlock *_bb;
    };

    using InstructionList = frg::intrusive_list<
        Instruction,
        frg::locate_member<
            Instruction,
            frg::default_list_hook<Instruction>,
            &Instruction::_instListHook
        >
    >;

    struct InstructionIterator {
        friend struct BasicBlock;
    private:
        using Base = InstructionList::iterator;

    public:
        InstructionIterator(Base b)
        : _b{b} { }

        bool operator!= (const InstructionIterator &other) const {
            return _b != other._b;
        }

        Instruction *operator* () const {
            return *_b;
        }

        void operator++ () {
            ++_b;
        }

    private:
        Base _b;
    };

    struct InstructionRange {
        InstructionRange(BasicBlock *bb)
        : _bb{bb} { }

        InstructionIterator begin() {
            return InstructionIterator{_bb->_insts.begin()};
        }
        InstructionIterator end() {
            return InstructionIterator{_bb->_insts.end()};
        }

    private:
        BasicBlock *_bb;
    };

    PhiRange phis() {
        return PhiRange{this};
    }

    PhiNode *attachPhi(std::unique_ptr<PhiNode> phi) {
        auto ptr = phi.get();
        _phis.push_back(phi.release());
        return ptr;
    }

    PhiIterator replacePhi(PhiIterator from, std::unique_ptr<PhiNode> to) {
        auto it = from;
        auto nit = _phis.insert(it, to.release());
        _phis.erase(it);
        return nit;
    }

    InstructionRange instructions() {
        return InstructionRange{this};
    }

    void doInsertInstruction(std::unique_ptr<Instruction> inst) {
        _insts.push_back(inst.release());
    }

    void doInsertInstruction(InstructionIterator before, std::unique_ptr<Instruction> inst) {
        _insts.insert(before._b, inst.release());
    }

    template<typename I>
    I *insertInstruction(std::unique_ptr<I> inst) {
        auto ptr = inst.get();
        doInsertInstruction(std::move(inst));
        return ptr;
    }

    template<typename I>
    I *insertInstruction(InstructionIterator it, std::unique_ptr<I> inst) {
        auto ptr = inst.get();
        doInsertInstruction(it, std::move(inst));
        return ptr;
    }

    InstructionIterator replaceInstruction(InstructionIterator from, std::unique_ptr<Instruction> to) {
        auto it = from._b;
        auto nit = _insts.insert(it, to.release());
        _insts.erase(it);
        return nit;
    }

    void setBranch(std::unique_ptr<Branch> branch) {
        _branch = std::move(branch);
    }

    Branch *branch() {
        return _branch.get();
    }

private:
    frg::default_list_hook<BasicBlock> _blockListHook;

    PhiList _phis;
    InstructionList _insts;
    std::unique_ptr<Branch> _branch;
};

//---------------------------------------------------------------------------------------
// Function class.
//---------------------------------------------------------------------------------------

struct Function {
    using BlockList = frg::intrusive_list<
        BasicBlock,
        frg::locate_member<
            BasicBlock,
            frg::default_list_hook<BasicBlock>,
            &BasicBlock::_blockListHook
        >
    >;

    using BlockIterator = BlockList::iterator;

    struct BlockRange {
        BlockRange(Function *fn)
        : _fn{fn} { }

        BlockIterator begin() {
            return _fn->_blocks.begin();
        }
        BlockIterator end() {
            return _fn->_blocks.end();
        }

    private:
        Function *_fn;
    };

    BlockRange blocks() {
        return BlockRange{this};
    }

    BasicBlock *addBlock(std::unique_ptr<BasicBlock> block) {
        auto ptr = block.get();
        _blocks.push_back(block.release());
        return ptr;
    }

private:
    BlockList _blocks;
};

//---------------------------------------------------------------------------------------
// Helper class to define instructions with a single result.
//---------------------------------------------------------------------------------------

// Value subclass for results of instructions without special properties.
struct GenericResult
: Value,
        CastableIfValueKind<GenericResult, value_kinds::genericResult> {
    GenericResult()
    : Value{value_kinds::genericResult} { }
};

struct WithGenericResult {
    GenericResult *result() {
        return &_result;
    }

private:
    GenericResult _result;
};

//---------------------------------------------------------------------------------------
// Next, the actual instruction classes are defined.
//---------------------------------------------------------------------------------------

struct LoadConstInstruction
: Instruction, WithGenericResult,
        CastableIfInstructionKind<LoadConstInstruction, instruction_kinds::loadConst> {
    LoadConstInstruction(uint64_t value_ = 0)
    : Instruction{instruction_kinds::loadConst}, value{value_} { }

    // TODO: This value should probably be more generic. For now, uint64_t is sufficient though.
    uint64_t value;
};

enum class UnaryMathOpcode {
    null,
    negate
};

struct UnaryMathInstruction
: Instruction, WithGenericResult,
        CastableIfInstructionKind<UnaryMathInstruction, instruction_kinds::unaryMath> {
    UnaryMathInstruction(UnaryMathOpcode opcode_ = UnaryMathOpcode::null,
            Value *operand_ = nullptr)
    : Instruction{instruction_kinds::unaryMath}, opcode{opcode_}, operand{this, operand_} { }

    UnaryMathOpcode opcode;
    ValueUse operand;
};

} // namespace lewis

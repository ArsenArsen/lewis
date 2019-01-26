
#include <cassert>
#include <iostream>
#include <queue>
#include <unordered_map>
#include <frg/interval_tree.hpp>
#include <lewis/target-x86_64/arch-ir.hpp>
#include <lewis/target-x86_64/arch-passes.hpp>

namespace lewis::targets::x86_64 {

namespace {
    std::unique_ptr<Value> cloneModeValue(Value *value) {
        auto modeM = hierarchy_cast<ModeMValue *>(value);
        assert(modeM);
        auto clone = std::make_unique<ModeMValue>();
        clone->operandSize = modeM->operandSize;
        return clone;
    }

    void setRegister(Value *v, int registerIdx) {
        if (auto modeMValue = hierarchy_cast<ModeMValue *>(v); modeMValue) {
            modeMValue->modeRegister = registerIdx;
        } else {
            assert(!"Unexpected x86_64 IR value");
        }
    }
}

enum SubBlock {
    beforeBlock = -1,
    inBlock = 0,
    afterBlock = 1,
};

// We need to be able to distinguish the allocation situation before and after an instruction.
// The atInstruction value is not really used.
enum SubInstruction {
    beforeInstruction = -1,
    atInstruction = 0,
    afterInstruction = 1,
};

// Represents a point in the program at which we perform register allocation.
struct ProgramCounter {
    bool operator== (const ProgramCounter &other) const {
        return block == other.block
                && subBlock == other.subBlock
                && instruction == other.instruction
                && subInstruction == other.subInstruction;
    }
    bool operator!= (const ProgramCounter &other) const {
        return !(*this == other);
    }

    bool operator< (const ProgramCounter &other) const {
        if (block != other.block)
            return block < other.block;
        if (subBlock != other.subBlock)
            return subBlock < other.subBlock;
        if (instruction != other.instruction) {
            auto index = block->indexOfInstruction(instruction);
            auto otherIndex = block->indexOfInstruction(other.instruction);
            if (index != otherIndex)
                return index < otherIndex;
        }
        return subInstruction < other.subInstruction;
    }
    bool operator<= (const ProgramCounter &other) const {
        return (*this < other) || (*this == other);
    }

    BasicBlock *block = nullptr;
    SubBlock subBlock = inBlock;
    Instruction *instruction = nullptr;
    SubInstruction subInstruction = atInstruction;
};

struct LiveCompound;

struct LiveInterval {
    // Value that is allocated to the register.
    // Note that for LiveCompounds that represent phi nodes, the associatedValue is
    // different in each source BasicBlock.
    Value *associatedValue = nullptr;

    LiveCompound *compound = nullptr;

    // Program counters of interval origin and final use.
    ProgramCounter originPc;
    ProgramCounter finalPc;

    bool inMoveChain = false;
    LiveInterval *previousMoveInChain = nullptr;

    // List of intervals that share the same compound.
    frg::default_list_hook<LiveInterval> compoundHook;

    // Intrusive tree that stores all intervals.
    frg::rbtree_hook rbHook;
    frg::interval_hook<ProgramCounter> intervalHook;
};

// Encapsulates multiple LiveIntervals that are always allocated to the same register.
struct LiveCompound {
    frg::intrusive_list<
        LiveInterval,
        frg::locate_member<
            LiveInterval,
            frg::default_list_hook<LiveInterval>,
            &LiveInterval::compoundHook
        >
    > intervals;

    int allocatedRegister = -1;

    uint64_t possibleRegisters = 0;
};

struct AllocateRegistersImpl : AllocateRegistersPass {
    AllocateRegistersImpl(Function *fn)
    : _fn{fn} { }

    void run() override;

private:
    void _collectBlockIntervals(BasicBlock *bb);
    void _collectPhiIntervals(BasicBlock *bb);
    std::optional<ProgramCounter> _determineFinalPc(BasicBlock *bb, Value *v);
    void _establishAllocation(BasicBlock *bb);

    Function *_fn;

    // Stores all intervals that still need to be allocated.
    // TODO: Prioritize the intervals in some way, e.g. by use density.
    std::queue<LiveCompound *> _queue;

    // Stores all intervals that have already been allocated.
    frg::interval_tree<
        LiveInterval,
        ProgramCounter,
        &LiveInterval::originPc,
        &LiveInterval::finalPc,
        &LiveInterval::rbHook,
        &LiveInterval::intervalHook
    > _allocated;
};

void AllocateRegistersImpl::run() {
    for (auto bb : _fn->blocks())
        _collectBlockIntervals(bb);
    for (auto bb : _fn->blocks())
        _collectPhiIntervals(bb);

    // The following loop performs the actual allocation.
    while (!_queue.empty()) {
        auto compound = _queue.front();
        _queue.pop();

        // Determine a bitmask of registers that are already allocated.
        uint64_t registersBlocked = 0;
        for (auto interval : compound->intervals)
            _allocated.for_overlaps([&] (LiveInterval *overlap) {
                assert(overlap->compound->allocatedRegister >= 0);
                registersBlocked |= 1 << overlap->compound->allocatedRegister;
            }, interval->originPc, interval->finalPc);

        // Chose the first free register using the bitmask.
        // TODO: Currently, we just allocate to the first 4 registers: rax, rcx, rdx, rbx.
        //       Generalize this register model.
        for (int i = 0; i < 8; i++) {
            if (!(compound->possibleRegisters & (1 << i)))
                continue;
            if (registersBlocked & (1 << i))
                continue;
            compound->allocatedRegister = i;
            break;
        }
        assert(compound->allocatedRegister >= 0
                && "Register spilling is not implemented yet");
        std::cout << "Allocating to register " << compound->allocatedRegister << std::endl;
        for (auto interval : compound->intervals)
            std::cout << "    Affects " << interval->associatedValue
                    << " at [" << interval->originPc.instruction
                    << ", " << interval->finalPc.instruction << "]" << std::endl;

        for (auto interval : compound->intervals)
            _allocated.insert(interval);
    }

    for (auto bb : _fn->blocks())
        _establishAllocation(bb);
}

// Called before allocation. Generates all LiveIntervals and adds them to the queue.
void AllocateRegistersImpl::_collectBlockIntervals(BasicBlock *bb) {
    std::vector<LiveCompound *> collected;

    // Generate LiveIntervals for instructions.
    for (auto it = bb->instructions().begin(); it != bb->instructions().end(); ++it) {
        if (auto movMC = hierarchy_cast<MovMCInstruction *>(*it); movMC) {
            auto compound = new LiveCompound;
            compound->possibleRegisters = 0xF;

            auto interval = new LiveInterval;
            compound->intervals.push_back(interval);
            interval->associatedValue = movMC->result.get();
            interval->compound = compound;
            interval->originPc = ProgramCounter{bb, inBlock, *it, afterInstruction};
            assert(interval->associatedValue);

            collected.push_back(compound);
        } else if (auto unaryMOverwrite = hierarchy_cast<UnaryMOverwriteInstruction *>(*it);
                unaryMOverwrite) {
            auto compound = new LiveCompound;
            compound->possibleRegisters = 0xF;

            auto resultInterval = new LiveInterval;
            compound->intervals.push_back(resultInterval);
            resultInterval->associatedValue = unaryMOverwrite->result.get();
            resultInterval->compound = compound;
            resultInterval->originPc = ProgramCounter{bb, inBlock, *it, afterInstruction};
            assert(resultInterval->associatedValue);

            collected.push_back(compound);
        } else if (auto unaryMInPlace = hierarchy_cast<UnaryMInPlaceInstruction *>(*it);
                unaryMInPlace) {
            auto pseudoMove = bb->insertInstruction(it,
                    std::make_unique<PseudoMoveSingleInstruction>(unaryMInPlace->primary.get()));
            auto pseudoMoveResult = pseudoMove->result.set(cloneModeValue(unaryMInPlace->primary.get()));
            unaryMInPlace->primary = pseudoMoveResult;

            auto compound = new LiveCompound;
            compound->possibleRegisters = 0xF;

            auto copyInterval = new LiveInterval;
            compound->intervals.push_back(copyInterval);
            copyInterval->associatedValue = pseudoMoveResult;
            copyInterval->compound = compound;
            copyInterval->originPc = ProgramCounter{bb, inBlock, pseudoMove, afterInstruction};

            auto resultInterval = new LiveInterval;
            compound->intervals.push_back(resultInterval);
            resultInterval->associatedValue = unaryMInPlace->result.get();
            resultInterval->compound = compound;
            resultInterval->originPc = ProgramCounter{bb, inBlock, *it, afterInstruction};
            assert(resultInterval->associatedValue);

            collected.push_back(compound);
        } else if (auto binaryMRInPlace = hierarchy_cast<BinaryMRInPlaceInstruction *>(*it);
                binaryMRInPlace) {
            auto pseudoMove = bb->insertInstruction(it,
                    std::make_unique<PseudoMoveSingleInstruction>(binaryMRInPlace->primary.get()));
            auto pseudoMoveResult = pseudoMove->result.set(cloneModeValue(binaryMRInPlace->primary.get()));
            binaryMRInPlace->primary = pseudoMoveResult;

            auto compound = new LiveCompound;
            compound->possibleRegisters = 0xF;

            auto copyInterval = new LiveInterval;
            compound->intervals.push_back(copyInterval);
            copyInterval->associatedValue = pseudoMoveResult;
            copyInterval->compound = compound;
            copyInterval->originPc = ProgramCounter{bb, inBlock, pseudoMove, afterInstruction};

            auto resultInterval = new LiveInterval;
            compound->intervals.push_back(resultInterval);
            resultInterval->associatedValue = binaryMRInPlace->result.get();
            resultInterval->compound = compound;
            resultInterval->originPc = {bb, inBlock, *it, afterInstruction};
            assert(resultInterval->associatedValue);

            collected.push_back(compound);
        } else if (auto call = hierarchy_cast<CallInstruction *>(*it); call) {
            // Add a PseudoMove instruction for the operands.
            auto pseudoMove = bb->insertInstruction(it,
                    std::make_unique<PseudoMoveMultipleInstruction>(call->numOperands()));
            for (size_t i = 0; i < call->numOperands(); ++i) {
                pseudoMove->operand(i) = call->operand(i).get();
                auto pseudoMoveResult = pseudoMove->result(i).set(cloneModeValue(call->operand(i).get()));
                call->operand(i) = pseudoMoveResult;

                auto copyCompound = new LiveCompound;
                switch (i) {
                case 0: copyCompound->possibleRegisters = 0x80; break;
                case 1: copyCompound->possibleRegisters = 0x40; break;
                default: assert(!"TODO: Implement correct ABI for arbitrary arguments");
                }

                auto copyInterval = new LiveInterval;
                copyCompound->intervals.push_back(copyInterval);
                copyInterval->associatedValue = pseudoMoveResult;
                copyInterval->compound = copyCompound;
                copyInterval->originPc = ProgramCounter{bb, inBlock, pseudoMove, afterInstruction};

                collected.push_back(copyCompound);
            }

            // Add LiveIntervals for the results.
            auto resultCompound = new LiveCompound;
            resultCompound->possibleRegisters = 0x1;

            auto resultInterval = new LiveInterval;
            resultCompound->intervals.push_back(resultInterval);
            resultInterval->associatedValue = call->result.get();
            resultInterval->compound = resultCompound;
            resultInterval->originPc = ProgramCounter{bb, inBlock, *it, afterInstruction};
            assert(resultInterval->associatedValue);

            collected.push_back(resultCompound);
        } else {
            std::cout << "lewis: Unknown instruction kind " << (*it)->kind << std::endl;
            assert(!"Unexpected IR instruction");
        }
    }

    // Generate a PseudoMove instruction for data-flow PhiNodes.
    std::vector<DataFlowEdge *> edges; // Need to index edges.
    for (auto edge : bb->source.edges())
        edges.push_back(edge);

    if (!edges.empty()) {
        auto pseudoMove = bb->insertInstruction(
                std::make_unique<PseudoMoveMultipleInstruction>(edges.size()));
        for (size_t i = 0; i < edges.size(); i++) {
            pseudoMove->operand(i) = edges[i]->alias.get();
            edges[i]->alias = pseudoMove->result(i).set(cloneModeValue(pseudoMove->operand(i).get()));
        }
    }

    // Post-process the generated intervals.
    for (auto compound : collected) {
        for (auto interval : compound->intervals) {
            // Compute the final PC for each interval.
            assert(interval->associatedValue);
            auto maybeFinalPc = _determineFinalPc(bb, interval->associatedValue);
            interval->finalPc = maybeFinalPc.value_or(interval->originPc);
        }

        _queue.push(compound);
    }
}

void AllocateRegistersImpl::_collectPhiIntervals(BasicBlock *bb) {
    // Generate LiveIntervals for PhiNodes.
    for (auto phi : bb->phis()) {
        if (auto argument = hierarchy_cast<ArgumentPhi *>(phi); argument) {
            auto compound = new LiveCompound;
            compound->possibleRegisters = 0x80;

            auto nodeInterval = new LiveInterval;
            compound->intervals.push_back(nodeInterval);
            nodeInterval->associatedValue = phi->value.get();
            nodeInterval->compound = compound;
            nodeInterval->originPc = {bb, beforeBlock, nullptr, afterInstruction};
            assert(nodeInterval->associatedValue);
            auto maybeFinalPc = _determineFinalPc(bb, phi->value.get());
            nodeInterval->finalPc = maybeFinalPc.value_or(nodeInterval->originPc);

            _queue.push(compound);
        } else if (auto dataFlow = hierarchy_cast<DataFlowPhi *>(phi); dataFlow) {
            auto compound = new LiveCompound;
            compound->possibleRegisters = 0xF;

            auto nodeInterval = new LiveInterval;
            compound->intervals.push_back(nodeInterval);
            nodeInterval->associatedValue = phi->value.get();
            nodeInterval->compound = compound;
            nodeInterval->originPc = {bb, beforeBlock, nullptr, afterInstruction};
            assert(nodeInterval->associatedValue);
            auto maybeFinalPc = _determineFinalPc(bb, phi->value.get());
            nodeInterval->finalPc = maybeFinalPc.value_or(nodeInterval->originPc);

            // By construction, we only see values from the PseudoMove instruction
            // that was generated in _collectBlockIntervals().
            for (auto edge : dataFlow->sink.edges()) {
                auto sourceInterval = new LiveInterval;
                assert(edge->alias);
                compound->intervals.push_back(sourceInterval);
                sourceInterval->associatedValue = edge->alias.get();
                sourceInterval->compound = compound;
                assert(sourceInterval->associatedValue);
                sourceInterval->originPc = ProgramCounter{edge->source()->block(), inBlock,
                        edge->alias.get()->origin()->instruction(), afterInstruction};
                sourceInterval->finalPc = ProgramCounter{edge->source()->block(), afterBlock,
                        nullptr, afterInstruction};
            }

            _queue.push(compound);

        } else {
            assert(!"Unexpected IR phi");
        }
    }
}

std::optional<ProgramCounter> AllocateRegistersImpl::_determineFinalPc(BasicBlock *bb, Value *v) {
    Instruction *finalInst = nullptr;
    size_t finalIndex;
    for (auto use : v->uses()) {
        // We should never see uses in DataFlowEdges, as they should all originate from the
        // PseudoMove instruction generated in _collectBlockIntervals().
        // This function is never called on those values.
        assert(use->instruction());
        auto useInst = use->instruction();
        auto useIndex = bb->indexOfInstruction(useInst);
        if (!finalInst || useIndex > finalIndex) {
            finalInst = useInst;
            finalIndex = useIndex;
        }
    }
    if(finalInst)
        return ProgramCounter{bb, inBlock, finalInst, beforeInstruction};
    return std::nullopt;
}

// This is called *after* the actual allocation is done. It "implements" the allocation by
// fixing registers in the IR and generating necessary move instructions.
void AllocateRegistersImpl::_establishAllocation(BasicBlock *bb) {
    std::unordered_map<Value *, LiveInterval *> liveMap;
    std::unordered_map<Value *, LiveInterval *> resultMap;
    std::cout << "Fixing basic block " << bb << std::endl;

    // Find all intervals that originate from phis.
    _allocated.for_overlaps([&] (LiveInterval *interval) {
        std::cout << "    Value " << interval->associatedValue
                << " (from phi node) is live" << std::endl;
        liveMap.insert({interval->associatedValue, interval});
        setRegister(interval->associatedValue, interval->compound->allocatedRegister);
    }, {bb, beforeBlock, nullptr, afterInstruction});

    for (auto it = bb->instructions().begin(); it != bb->instructions().end(); ) {
        std::cout << "    Fixing instruction " << *it << ", kind "
                << (*it)->kind << std::endl;
        // Determine the current register allocation.
        LiveInterval *currentState[16] = {};

        for (auto entry : liveMap) {
            auto interval = entry.second;
            auto currentRegister = interval->compound->allocatedRegister;
            assert(currentRegister >= 0);
            assert(!currentState[currentRegister]);
            currentState[currentRegister] = interval;
            std::cout << "    Current state[" << currentRegister << "]: "
                    << interval->associatedValue << std::endl;
        }

        // Find all intervals that originate from the current PC.
        _allocated.for_overlaps([&] (LiveInterval *interval) {
            if (interval->originPc.instruction != *it)
                return;
            std::cout << "        Instruction returns " << interval->associatedValue << std::endl;
            resultMap.insert({interval->associatedValue, interval});
        }, {bb, inBlock, *it, afterInstruction});

        // Helper function to fuse a result interval (from resultMap)
        // into a live interval (from liveMap).
        auto fuseResultInterval = [&] (LiveInterval *interval, LiveInterval *into) {
            auto resIt = resultMap.find(interval->associatedValue);
            assert(resIt != resultMap.end());
            resultMap.erase(resIt);
            _allocated.remove(interval);

            assert((into->finalPc == ProgramCounter{bb, inBlock, *it, beforeInstruction}));
            _allocated.remove(into);
            into->finalPc = interval->finalPc;
            _allocated.insert(into);
        };

        // Helper function to rewrite the associatedValue of a result interval (from resultMap).
        auto reassociateResultInterval = [&] (LiveInterval *interval, Value *newValue) {
            auto resIt = resultMap.find(interval->associatedValue);
            assert(resIt != resultMap.end());
            resultMap.erase(resIt);
            _allocated.remove(interval);

            interval->associatedValue = newValue;
            resultMap.insert({newValue, interval});
        };

        // Rewrite pseudo instructions to real instructions.
        bool rewroteInstruction = false;
        if (auto pseudoMoveSingle = hierarchy_cast<PseudoMoveSingleInstruction *>(*it);
                pseudoMoveSingle) {
            auto operandInterval = liveMap.at(pseudoMoveSingle->operand.get());
            auto resultInterval = resultMap.at(pseudoMoveSingle->result.get());
            if (operandInterval->compound->allocatedRegister
                    == resultInterval->compound->allocatedRegister) {
                std::cout << "        Rewriting pseudoMoveSingle (fuse)" << std::endl;
                pseudoMoveSingle->result.get()->replaceAllUses(pseudoMoveSingle->operand.get());

                fuseResultInterval(resultInterval, operandInterval);
            }else{
                std::cout << "        Rewriting pseudoMoveSingle (reassociate)" << std::endl;
                auto movMR = std::make_unique<MovMRInstruction>(pseudoMoveSingle->operand.get());
                auto movMRResult = movMR->result.set(cloneModeValue(pseudoMoveSingle->operand.get()));
                setRegister(movMRResult, resultInterval->compound->allocatedRegister);

                pseudoMoveSingle->operand = nullptr;
                pseudoMoveSingle->result.get()->replaceAllUses(movMRResult);

                reassociateResultInterval(resultInterval, movMRResult);
                bb->insertInstruction(it, std::move(movMR));
            }

            rewroteInstruction = true;
        } else if (auto pseudoMoveMultiple = hierarchy_cast<PseudoMoveMultipleInstruction *>(*it);
                pseudoMoveMultiple) {
            // The following code minimizes the number of move instructions.
            // This is done as follows:
            // - The code constructs "move chains", i.e., chains of registers that need to be moved.
            //   For example, such a chain could be rax -> rcx -> rdx.
            // - The resulting graph only consists of paths and cycles
            //   (as every register has in-degree at most 1).
            // - Emit those paths in cycles.
            std::cout << "        Rewriting pseudoMoveMultiple" << std::endl;

            // Represents a node of the move chain graph.
            struct MoveChain {
                // ----------------------------------------------------------------------
                // Members defined for all MoveChains.
                // ----------------------------------------------------------------------

                bool isTail() {
                    if(!isTarget())
                        return false;
                    if(isSource() && pendingMovesFromThisSource)
                        return false;
                    return true;
                }

                bool seenInTraversal = false;
                bool traversalFinished = false;

                MoveChain *cyclePointer = nullptr;

                // ----------------------------------------------------------------------
                // Members defined for move *sources*.
                // ----------------------------------------------------------------------

                bool isSource() {
                    return firstTarget;
                }

                // Index of the corresponding PseudoMoveMultiple operand.
                int operandIndex = -1;

                // First chain that has this chain as uniqueSource.
                // TODO: Do we actually need this linked list?
                MoveChain *firstTarget = nullptr;

                // Number of non-emitted moves until this tail becomes active.
                int pendingMovesFromThisSource = 0;

                // ----------------------------------------------------------------------
                // Members defined for move *targets*.
                // ----------------------------------------------------------------------

                bool isTarget() {
                    return uniqueSource;
                }

                // Index of the corresponding PseudoMoveMultiple result.
                // TODO: Do we actually need this index?
                int resultIndex = -1;

                MoveChain *uniqueSource = nullptr;

                // Next chain that shares the same uniqueSource.
                MoveChain *siblingTarget = nullptr;

                bool didMoveToThisTarget = false;

                // ----------------------------------------------------------------------
                // Members defined for cycle representatives (pointed to by cyclePointer).
                // ----------------------------------------------------------------------

                // Number of non-emitted moves until this cycle becomes active.
                int pendingMovesFromThisCycle = 0;
            };

            MoveChain chains[16];

            auto chainRegister = [&] (MoveChain *chain) -> int {
                return chain - chains;
            };

            // Build the MoveChains from the PseudoMoveMultiple instruction.
            for (size_t i = 0; i < pseudoMoveMultiple->arity(); ++i) {
                auto operandInterval = liveMap.at(pseudoMoveMultiple->operand(i).get());
                auto resultInterval = resultMap.at(pseudoMoveMultiple->result(i).get());

                // Special case self-loops in move chains (no move is necessary).
                auto operandRegister = operandInterval->compound->allocatedRegister;
                auto resultRegister = resultInterval->compound->allocatedRegister;
                assert(operandRegister >= 0);
                assert(resultRegister >= 0);
                if (operandRegister == resultRegister) {
                    fuseResultInterval(resultInterval, operandInterval);
                    continue;
                }

                // Setup the MoveChain structs.
                auto operandChain = &chains[operandRegister];
                auto resultChain = &chains[resultRegister];

                operandChain->resultIndex = i;
                resultChain->operandIndex = i;

                assert(!resultChain->uniqueSource);
                assert(!resultChain->siblingTarget);
                resultChain->uniqueSource = operandChain;
                resultChain->siblingTarget = operandChain->firstTarget;

                operandChain->firstTarget = resultChain;
                operandChain->pendingMovesFromThisSource++;
            }

            std::vector<MoveChain *> activeTails;
            std::vector<MoveChain *> activeCycles;

            // Helper function to emit a single move of a move chain.
            auto emitMoveToChain = [&] (MoveChain *targetChain) {
                auto index = targetChain->operandIndex;
                auto srcChain = targetChain->uniqueSource;
                assert(!targetChain->didMoveToThisTarget);
                assert(srcChain->pendingMovesFromThisSource > 0);

                auto operandInterval = liveMap.at(pseudoMoveMultiple->operand(index).get());
                auto resultInterval = resultMap.at(pseudoMoveMultiple->result(index).get());
                assert(operandInterval->compound->allocatedRegister == chainRegister(srcChain));
                assert(resultInterval->compound->allocatedRegister == chainRegister(targetChain));

                // Emit the new move instruction.
                auto move = std::make_unique<MovMRInstruction>(
                        pseudoMoveMultiple->operand(index).get());
                auto moveResult = move->result.set(cloneModeValue(pseudoMoveMultiple->operand(index).get()));
                setRegister(moveResult, resultInterval->compound->allocatedRegister);

                pseudoMoveMultiple->operand(index) = nullptr;
                pseudoMoveMultiple->result(index).get()->replaceAllUses(moveResult);

                reassociateResultInterval(resultInterval, moveResult);
                bb->insertInstruction(it, std::move(move));

                // Update the MoveChain structs.
                targetChain->didMoveToThisTarget = true;

                srcChain->pendingMovesFromThisSource--;
                if (srcChain->isTail())
                    activeTails.push_back(srcChain);

                auto cycleChain = srcChain->cyclePointer;
                if(cycleChain && cycleChain != targetChain->cyclePointer) {
                    cycleChain->pendingMovesFromThisCycle--;
                    if (!cycleChain->pendingMovesFromThisCycle)
                        activeCycles.push_back(cycleChain);
                }
            };

            // Traverse the graph backwards and determine all tails and cycles.
            std::vector<MoveChain *> stack;
            for (int i = 0; i < 16; i++) {
                auto rootChain = &chains[i];

                if (rootChain->isTail())
                    activeTails.push_back(rootChain);

                auto current = rootChain;
                while (current) {
                    // Check if we ran into a chain that was already traversed completely.
                    if (current->traversalFinished)
                        break;

                    // If we reach a visited but not finished chain, we ran into a cycle.
                    if (current->seenInTraversal) {
                        // current will become the cyclePointer.
                        auto it = stack.rbegin();
                        do {
                            // As current is not finished, it must be on the stack.
                            assert(it != stack.rend());
                            (*it)->cyclePointer = current;

                            // Accumulate the moves out of the cycle. Note that exactly one of the
                            // moves from pendingMovesFromThisSource is inside the cycle.
                            assert((*it)->pendingMovesFromThisSource > 0);
                            current->pendingMovesFromThisCycle
                                    += (*it)->pendingMovesFromThisSource - 1;
                        } while(*(it++) != current);
                        break;
                    }

                    current->seenInTraversal = true;
                    stack.push_back(current);

                    current = current->uniqueSource;
                }

                for (auto chain : stack)
                    chain->traversalFinished = true;
                stack.clear();
            }

            // First, handle all tails.
            while (!activeTails.empty()) {
                auto tailRegister = activeTails.back();
                activeTails.pop_back();
                emitMoveToChain(tailRegister);
            }

            // Now, handle all cycles.
            while (!activeCycles.empty()) {
                assert (!"Implement move cycles");
                // TODO: For cycles of length 2, use xchg.
                //       Otherwise, allocate a new temporary register.

                // Resolving the cycle always results in a tail.
                while (!activeTails.empty()) {
                    auto tailRegister = activeTails.back();
                    activeTails.pop_back();
                    emitMoveToChain(tailRegister);
                }
            }

            rewroteInstruction = true;
        }

        // Fix the allocation for all results of the current instruction.
        for (auto [value, interval] : resultMap)
            setRegister(value, interval->compound->allocatedRegister);

        // Erase all intervals that end before the current instruction.
        // TODO: In principle, the loops to erase intervals can be accelerated by maintaining
        //       a tree of intervals, ordered by their finalPc. For now, we just use a linear scan.
        for (auto liveIt = liveMap.begin(); liveIt != liveMap.end(); ) {
            auto finalPc = liveIt->second->finalPc;
            if (finalPc == ProgramCounter{bb, inBlock, *it, beforeInstruction}) {
                liveIt = liveMap.erase(liveIt);
            } else {
                assert(finalPc.block == bb);
                assert(!(finalPc <= ProgramCounter{bb, inBlock, *it, afterInstruction}));
                ++liveIt;
            }
        }

        // Merge all intervals that originate from the previous PC.
        liveMap.merge(resultMap);
        assert(resultMap.empty());

        // Erase all intervals that end after the current instruction.
        for (auto liveIt = liveMap.begin(); liveIt != liveMap.end(); ) {
            auto finalPc = liveIt->second->finalPc;
            if (finalPc == ProgramCounter{bb, inBlock, *it, afterInstruction}) {
                liveIt = liveMap.erase(liveIt);
            } else {
                assert(finalPc.block == bb);
                assert(!(finalPc <= ProgramCounter{bb, inBlock, *it, afterInstruction}));
                ++liveIt;
            }
        }

        auto nextIt = it;
        ++nextIt;
        if (rewroteInstruction)
            bb->eraseInstruction(it);
        it = nextIt;
    }
}

std::unique_ptr<AllocateRegistersPass> AllocateRegistersPass::create(Function *fn) {
    return std::make_unique<AllocateRegistersImpl>(fn);
}

} // namespace lewis::targets::x86_64

#ifndef LLVM_CODEGEN_COMPACT_ORDER
#define LLVM_CODEGEN_COMPACT_ORDER

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegionInfo.h"

using namespace llvm;

namespace llvm {

struct CompactNode {
  bool IsLoop;
  union {
    MachineBasicBlock *MBB;
    MachineLoop *Loop;
  } Val;

  CompactNode(MachineBasicBlock *MBB) : IsLoop(false) {
    Val.MBB = MBB;   
  }

  CompactNode(MachineLoop *Loop) : IsLoop(true) {
    Val.Loop = Loop;   
  }

  MachineBasicBlock *getBlock() {
    assert(!IsLoop && "Wrong accessor");
    return Val.MBB;
  }

  MachineLoop *getLoop() {
    assert(IsLoop && "Wrong accessor");
    return Val.Loop;
  }

  MachineBasicBlock *asBlock() {
    if (IsLoop)
      return Val.Loop->getHeader();
    return Val.MBB;
  }
};

class CompactOrder : public MachineFunctionPass {
  const MachineLoopInfo *MLI;

public:
  static char ID;
  SmallVector<CompactNode *> Order;

  CompactOrder();

  SmallVector<CompactNode *, 4> postOrder(CompactNode *Entry);
  SmallVector<CompactNode *, 4> compactOrder(CompactNode *Entry);
  SmallVector<CompactNode *, 4> getSuccessors(CompactNode *Node) const;
  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineLoopInfo>();
    AU.addRequired<MachineDominatorTree>();
    // AU.addRequiredTransitive<MachineRegionInfoPass>();
    // AU.addUsedIfAvailable<LiveVariables>();
    // AU.addRequired<SlotIndexes>();
    // AU.addPreserved<SlotIndexes>();
    // AU.addRequired<LiveIntervals>();
    // AU.addPreserved<LiveIntervals>();
    // AU.addPreserved<LiveVariables>();
    // AU.setPreservesCFG();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  void print(raw_ostream &OS, const Module *) const override;
};

} // namespace llvm

#endif // LLVM_CODEGEN_COMPACT_ORDER

#ifndef LLVM_CODEGEN_INSERT_CONFLICTING_DEFS
#define LLVM_CODEGEN_INSERT_CONFLICTING_DEFS

#include "llvm/CodeGen/AMiLinearizationAnalysis.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/PersistencyAnalysis.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/SlotIndexes.h"

using namespace llvm;

namespace llvm {

class InsertConflictingDefs : public MachineFunctionPass {
  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;
  MachineRegisterInfo *MRI;

  LiveIntervals *LIS;

  DenseMap<ActivatingRegion *, MachineBasicBlock *> ConstraintMBBMap;

public:
  static char ID;

  InsertConflictingDefs();

  MachineBasicBlock *createConstraintMBB(MachineFunction &MF,
                                         MachineBasicBlock *From,
                                         MachineBasicBlock *To);
  void insertGhostLoad(MachineInstr *StoreMI);
  void addConstraints(MachineInstr *MI);

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AMiLinearizationAnalysis>();
    AU.addPreserved<AMiLinearizationAnalysis>();
    AU.addRequired<PersistencyAnalysisPass>();
    AU.addPreserved<PersistencyAnalysisPass>();
    AU.addUsedIfAvailable<LiveIntervals>();
    AU.addPreserved<LiveIntervals>();
    AU.addPreserved<MachineDominatorTree>();
    // AU.addRequiredTransitive<MachineRegionInfoPass>();
    // AU.addUsedIfAvailable<LiveVariables>();
    // AU.addRequired<SlotIndexes>();
    // AU.addPreserved<SlotIndexes>();
    // AU.addRequired<LiveIntervals>();
    // AU.addPreserved<LiveIntervals>();
    // AU.addPreserved<LiveVariables>();
    // AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_INSERT_CONFLICTING_DEFS

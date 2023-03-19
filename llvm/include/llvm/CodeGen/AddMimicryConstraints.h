#ifndef LLVM_CODEGEN_ADD_MIMICRY_CONSTRAINTS
#define LLVM_CODEGEN_ADD_MIMICRY_CONSTRAINTS

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

class AddMimicryConstraints : public MachineFunctionPass {
  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;
  MachineRegisterInfo *MRI;

  LiveIntervals *LIS;

public:
  static char ID;

  AddMimicryConstraints();
  
  void insertGhostLoad(MachineInstr *StoreMI);
  void addConstraints(MachineInstr *MI);
  void addConstraint(LiveInterval &LI, SlotIndex SI, MachineInstr *ConflictingMI);
  
  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AMiLinearizationAnalysis>();
    AU.addPreserved<AMiLinearizationAnalysis>();
    AU.addRequired<PersistencyAnalysisPass>();
    AU.addPreserved<PersistencyAnalysisPass>();
    // AU.addRequiredTransitive<MachineRegionInfoPass>();
    // AU.addUsedIfAvailable<LiveVariables>();
    AU.addRequired<SlotIndexes>();
    AU.addPreserved<SlotIndexes>();
    AU.addRequired<LiveIntervals>();
    AU.addPreserved<LiveIntervals>();
    // AU.addPreserved<LiveVariables>();
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_ADD_MIMICRY_CONSTRAINTS


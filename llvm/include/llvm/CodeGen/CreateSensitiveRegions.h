#ifndef LLVM_CODEGEN_CREATE_SENSITIVE_REGIONS_H
#define LLVM_CODEGEN_CREATE_SENSITIVE_REGIONS_H

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/TrackSecrets.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/SensitiveRegion.h"

using namespace llvm;

namespace llvm {

class CreateSensitiveRegions : public MachineFunctionPass {
  MachineRegionInfo *MRI;
  MachineDominatorTree *MDT;
  MachinePostDominatorTree *MPDT;
  MachineDominanceFrontier *MDF;
  SensitiveRegionAnalysis *SRA;
  const TargetInstrInfo *TII;

public:
  static char ID;

  CreateSensitiveRegions();

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<SensitiveRegionAnalysis>();
    AU.addPreserved<SensitiveRegionAnalysis>();
    AU.addUsedIfAvailable<MachineRegionInfoPass>();
    AU.addPreserved<MachineRegionInfoPass>();
    AU.addUsedIfAvailable<MachineDominatorTree>();
    AU.addPreserved<MachineDominatorTree>();
    AU.addUsedIfAvailable<MachinePostDominatorTree>();
    AU.addPreserved<MachinePostDominatorTree>();
    AU.addUsedIfAvailable<MachineDominanceFrontier>();
    AU.addPreserved<MachineDominanceFrontier>();
    AU.addUsedIfAvailable<LiveVariables>();
    AU.addPreserved<LiveVariables>();
    AU.addPreservedID(UnreachableMachineBlockElimID);
    AU.setPreservesCFG();
    // AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_CREATE_SENSITIVE_REGIONS_H

#ifndef LLVM_CODEGEN_AMI_LINEARIZE_REGION_H
#define LLVM_CODEGEN_AMI_LINEARIZE_REGION_H

#include "RISCVInstrInfo.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/PersistencyAnalysis.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

using namespace llvm;

namespace {

class RISCVLinearizeBranch : public MachineFunctionPass {
public:
  static char ID;

  MachineRegionInfo *MRI;
  MachineDominatorTree *MDT;
  MachinePostDominatorTree *MPDT;
  MachineDominanceFrontier *MDF;
  SensitiveRegionAnalysis *SRA;
  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;
  SmallPtrSet<MachineRegion *, 16> ActivatingRegions;
  SmallVector<SensitiveBranch, 16> ActivatingBranches;

  RISCVLinearizeBranch();

  void findActivatingBranches();
  MachineBasicBlock *createFlowBlock(MachineFunction &MF, MachineRegion *MR);
  void createFlowBlocks(MachineFunction &MF);
  void linearizeBranches(MachineFunction &MF);

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<SensitiveRegionAnalysis>();
    AU.addPreserved<SensitiveRegionAnalysis>();
    AU.addPreserved<TrackSecretsAnalysis>();
    AU.addRequired<PersistencyAnalysisPass>();
    AU.addPreserved<PersistencyAnalysisPass>();
    AU.addRequiredTransitive<MachineRegionInfoPass>();
    AU.addPreserved<MachineRegionInfoPass>();
    AU.addUsedIfAvailable<MachineDominatorTree>();
    AU.addPreserved<MachineDominatorTree>();
    AU.addUsedIfAvailable<MachinePostDominatorTree>();
    AU.addPreserved<MachinePostDominatorTree>();
    AU.addUsedIfAvailable<MachineDominanceFrontier>();
    AU.addPreserved<MachineDominanceFrontier>();
    AU.addPreserved<ReachingDefAnalysis>();
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace

#endif // LLVM_CODEGEN_AMI_LINEARIZE_REGION

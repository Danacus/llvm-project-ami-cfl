#ifndef LLVM_CODEGEN_AMI_LINEARIZE_REGION_H
#define LLVM_CODEGEN_AMI_LINEARIZE_REGION_H

#include "RISCVInstrInfo.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

using namespace llvm;

namespace {

struct ActivatingBranch {
  MachineInstr *MI;
  SmallVector<MachineOperand> Cond;
  MachineRegion *ElseRegion;
  MachineBasicBlock *NewElseExit;
  MachineRegion *IfRegion;
  MachineBasicBlock *NewIfExit;

  ActivatingBranch(MachineInstr *MI, SmallVector<MachineOperand> Cond,
                   MachineRegion *TR, MachineRegion *FR)
      : MI(MI), Cond(Cond), ElseRegion(TR), IfRegion(FR) {}

  bool operator<(const ActivatingBranch &Other) const {
    return IfRegion->getDepth() < Other.IfRegion->getDepth();
  }

  bool operator>(const ActivatingBranch &Other) const {
    return IfRegion->getDepth() > Other.IfRegion->getDepth();
  }
};

class AMiLinearizeBranch : public MachineFunctionPass {
public:
  static char ID;

  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;
  SmallPtrSet<MachineRegion *, 16> ActivatingRegions;
  SmallVector<ActivatingBranch, 16> ActivatingBranches;

  AMiLinearizeBranch();

  template <RISCV::AMi::Qualifier Q> void setQualifier(MachineInstr *I);
  void findActivatingRegionsOld();
  void findActivatingBranches();

  // Linearize a region, returns the new exit
  MachineBasicBlock *linearizeRegion(MachineFunction &MF, MachineRegion *MR);

  void linearizeBranches(MachineFunction &MF);
  bool setBranchActivating(MachineBasicBlock &MBB);
  void removePseudoSecret(MachineFunction &MF);

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineRegionInfoPass>();
    AU.addRequiredTransitive<TrackSecretsAnalysisVirtReg>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace

#endif // LLVM_CODEGEN_AMI_LINEARIZE_REGION

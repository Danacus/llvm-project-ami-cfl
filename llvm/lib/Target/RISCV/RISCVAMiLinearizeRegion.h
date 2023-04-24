#ifndef LLVM_CODEGEN_AMI_LINEARIZE_REGION_H
#define LLVM_CODEGEN_AMI_LINEARIZE_REGION_H

#include "RISCVInstrInfo.h"
#include "llvm/CodeGen/AMiLinearizationAnalysisSESE.h"
#include "llvm/CodeGen/TrackSecrets.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominanceFrontier.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachinePostDominators.h"
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

class RISCVAMiLinearizeRegion : public MachineFunctionPass {
public:
  static char ID;

  const RISCVInstrInfo *TII;
  const RISCVRegisterInfo *TRI;
  LinearizationResult *ALA;
  PersistencyAnalysisPass *PA;

  RISCVAMiLinearizeRegion();

  template <RISCV::AMi::Qualifier Q> void setQualifier(MachineInstr *I);
  bool setBranchActivating(MachineBasicBlock &MBB,
                           MachineBasicBlock *Target = nullptr);
  void setBranchInstrActivating(MachineBasicBlock::iterator I,
                                MachineBasicBlock *Target = nullptr);
  bool isActivatingBranch(MachineBasicBlock &MBB);
  void findActivatingRegions();
  void handlePersistentInstr(MachineInstr *I);
  void handleRegion(ActivatingRegion *Region);

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequiredTransitive<MachineDominatorTree>();
    AU.addRequiredTransitive<MachinePostDominatorTree>();
    AU.addRequiredTransitive<MachineDominanceFrontier>();
    AU.addRequired<AMiLinearizationAnalysis>();
    AU.addRequired<PersistencyAnalysisPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace

#endif // LLVM_CODEGEN_AMI_LINEARIZE_REGION

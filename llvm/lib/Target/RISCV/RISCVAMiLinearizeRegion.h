#ifndef LLVM_CODEGEN_AMI_LINEARIZE_REGION_H
#define LLVM_CODEGEN_AMI_LINEARIZE_REGION_H

#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

using namespace llvm;

namespace {

class AMiLinearizeRegion : public MachineFunctionPass {
public:
  static char ID;

  AMiLinearizeRegion();
  
  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG(); // I believe AMi transformation usually preserve the CFG
    AU.addRequired<ReachingDefAnalysis>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace

#endif // LLVM_CODEGEN_AMI_LINEARIZE_REGION

#ifndef LLVM_CODEGEN_TRACKSECRETS_H
#define LLVM_CODEGEN_TRACKSECRETS_H

#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

using namespace llvm;

namespace {

class TrackSecretsAnalysis : public MachineFunctionPass {
public:
  static char ID;
  FunctionSecrets Secrets;

  TrackSecretsAnalysis();

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<FindSecretsAnalysis>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace

#endif // LLVM_CODEGEN_TRACKSECRETS_H

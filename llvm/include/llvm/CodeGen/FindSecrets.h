#ifndef LLVM_CODEGEN_FINDSECRETS_H
#define LLVM_CODEGEN_FINDSECRETS_H

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"

using namespace llvm;

struct SecretVar {
  Instruction* Instr;
  
  SecretVar(Instruction* I) {
    Instr = I;
  }
};

struct BlockSecrets {
  BasicBlock* Block;
  SmallVector<SecretVar> SecretVars;
  
  BlockSecrets(BasicBlock* B) {
    Block = B;
  }
};

struct FunctionSecrets {
  Function* Func;
  SmallVector<Instruction*> Upgrades;
  SmallVector<Instruction*> Downgrades;
  SmallVector<Argument*> Args;
  
  FunctionSecrets(Function* F) {
    Func = F;
  }
  
  FunctionSecrets() {}
};

struct SecretParams {
  SmallVector<Argument*> Args;
};

namespace {

class FindSecretsAnalysis : public MachineFunctionPass {
public:
  static char ID;
  FunctionSecrets Secrets;

  FindSecretsAnalysis();

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

class FindSecretsPrinter : public MachineFunctionPass {
public:
  static char ID;

  FindSecretsPrinter();

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<FindSecretsAnalysis>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace

#endif // LLVM_CODEGEN_FINDSECRETS_H

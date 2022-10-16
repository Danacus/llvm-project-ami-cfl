#ifndef LLVM_TRANSFORMS_SECRETS_H
#define LLVM_TRANSFORMS_SECRETS_H

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include <string>

using namespace llvm;

struct SecretVar {
  Instruction* Instr;
  
  SecretVar(Instruction* I) {
    Instr = I;
  }
};

struct BlockSecrets {
  BasicBlock* Block;
  std::vector<SecretVar> SecretVars;
  
  BlockSecrets(BasicBlock* B) {
    Block = B;
  }
};

struct FunctionSecrets {
  Function* Func;
  std::vector<BlockSecrets> Blocks;
  
  FunctionSecrets(Function* F) {
    Func = F;
  }
};

struct Secrets {
  std::vector<FunctionSecrets> Functions;
};

#endif // LLVM_TRANSFORMS_SECRETS_H

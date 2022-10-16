#ifndef LLVM_TRANSFORMS_SECRETS_H
#define LLVM_TRANSFORMS_SECRETS_H

#include "llvm/IR/Instruction.h"
#include <string>

struct SecretVar {
  llvm::Instruction* Instr;
};

#endif // LLVM_TRANSFORMS_SECRETS_H

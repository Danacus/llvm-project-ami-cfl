#ifndef LLVM_TRANSFORMS_FINDSECRETS_H
#define LLVM_TRANSFORMS_FINDSECRETS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Secrets.h"

namespace llvm {

class FindSecretsPass : public PassInfoMixin<FindSecretsPass> {
public:
  std::vector<SecretVar> SecretVars;
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_FINDSECRETS_H

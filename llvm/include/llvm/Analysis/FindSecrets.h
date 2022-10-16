#ifndef LLVM_TRANSFORMS_FINDSECRETS_H
#define LLVM_TRANSFORMS_FINDSECRETS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Analysis/Secrets.h"

namespace llvm {

class FindSecretsAnalysis : public AnalysisInfoMixin<FindSecretsAnalysis> {
  std::vector<SecretVar> SecretVars;
  
public:
  static AnalysisKey Key;
  using Result = Secrets;

  Secrets run(Module &M, ModuleAnalysisManager &AM);
};

class FindSecretsPrinterPass : public PassInfoMixin<FindSecretsPrinterPass> {
  raw_ostream &OS;

public:
  explicit FindSecretsPrinterPass(raw_ostream &OS) : OS(OS) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_FINDSECRETS_H

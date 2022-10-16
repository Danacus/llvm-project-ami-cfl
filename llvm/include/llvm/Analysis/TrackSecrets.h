#ifndef LLVM_TRANSFORMS_TRACKSECRETS_H
#define LLVM_TRANSFORMS_TRACKSECRETS_H

#include "llvm/IR/Module.h"
#include "llvm/Analysis/Secrets.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

class TrackSecretsAnalysis : public AnalysisInfoMixin<TrackSecretsAnalysis> {
public:
  static AnalysisKey Key;
  using Result = std::vector<SecretVar>;
  
  std::vector<SecretVar> run(Module &M, ModuleAnalysisManager &AM);
};

class TrackSecretsPrinterPass : public PassInfoMixin<TrackSecretsPrinterPass> {
  raw_ostream &OS;

public:
  explicit TrackSecretsPrinterPass(raw_ostream &OS) : OS(OS) {}

  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_TRACKSECRETS_H

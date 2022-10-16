#ifndef LLVM_TRANSFORMS_TRACKSECRETS_H
#define LLVM_TRANSFORMS_TRACKSECRETS_H

#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Secrets.h"

namespace llvm {

class TrackSecretsPass : public PassInfoMixin<TrackSecretsPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_TRACKSECRETS_H

#ifndef LLVM_TRANSFORMS_SCALAR_BRANCHENHANCE_H
#define LLVM_TRANSFORMS_SCALAR_BRANCHENHANCE_H

#include "llvm/IR/PassManager.h"

namespace llvm {

class BranchEnhancePass : public PassInfoMixin<BranchEnhancePass> {
public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_BRANCHENHANCE_H
#include "llvm/IR/PassManager.h"
#include "llvm/Transforms/Secrets.h"
#include "llvm/Transforms/Secrets/TrackSecrets.h"


using namespace llvm;

#define DEBUG_TYPE "tracksecrets"

PreservedAnalyses TrackSecretsPass::run(Module &M,
                                      ModuleAnalysisManager &AM) {
  errs() << "Track secrets\n";
  return PreservedAnalyses::all();
}

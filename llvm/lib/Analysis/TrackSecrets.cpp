#include "llvm/IR/PassManager.h"
#include "llvm/Analysis/FindSecrets.h"
#include "llvm/Analysis/TrackSecrets.h"


using namespace llvm;

#define DEBUG_TYPE "tracksecrets"

AnalysisKey TrackSecretsAnalysis::Key;

std::vector<SecretVar> TrackSecretsAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
  errs() << "Track secrets\n";
  std::vector<SecretVar> SecretVars = AM.getResult<FindSecretsAnalysis>(M);
  return SecretVars;
}

PreservedAnalyses TrackSecretsPrinterPass::run(Module &M, ModuleAnalysisManager &AM) {
  auto SecretVars = AM.getResult<TrackSecretsAnalysis>(M);
  
  for (SecretVar Secret : SecretVars) {
    Secret.Instr->dump();
  }
  
  return PreservedAnalyses::all();
}

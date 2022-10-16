#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Analysis/FindSecrets.h"
#include "llvm/Analysis/TrackSecrets.h"


using namespace llvm;

#define DEBUG_TYPE "tracksecrets"

AnalysisKey TrackSecretsAnalysis::Key;

Secrets TrackSecretsAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
  errs() << "Track secrets\n";
  Secrets Secrets = AM.getResult<FindSecretsAnalysis>(M);

  for (auto &FS : Secrets.Functions) {
    for (auto &BS : FS.Blocks) {
      for (auto &I : *BS.Block) {
        auto OC = I.getOpcode();
        
        switch(OC) {
          case Instruction::Load: {
            LoadInst *LI = llvm::dyn_cast<LoadInst>(&I);
            auto *OP = LI->getOperand(0);
            
            for (auto S : BS.SecretVars) {
              if (S.Instr->getName().compare(OP->getName()) == 0) {
                BS.SecretVars.push_back(SecretVar(LI));
                break;
              } 
            }
            
            break;
          }
          default:
            break;
        }
      }
    }
  }

  return Secrets;
}

PreservedAnalyses TrackSecretsPrinterPass::run(Module &M, ModuleAnalysisManager &AM) {
  auto Secrets = AM.getResult<TrackSecretsAnalysis>(M);
  
  for (auto &FS : Secrets.Functions) {
    OS << "Function: " << FS.Func->getName() << "\n";
    for (auto &BS : FS.Blocks) {
      OS << "BasicBlock: " << BS.Block->getName() << "\n";
      for (auto &S : BS.SecretVars) {
        S.Instr->print(OS);
        OS << "\n";
      }
      OS << "\n";
    }
    OS << "\n";
  }
  
  return PreservedAnalyses::all();
}

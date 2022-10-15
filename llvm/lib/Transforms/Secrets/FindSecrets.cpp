#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Secrets.h"
#include "llvm/Transforms/Secrets/FindSecrets.h"


using namespace llvm;

#define DEBUG_TYPE "find_secrets"

PreservedAnalyses FindSecretsPass::run(Module &M,
                                      ModuleAnalysisManager &AM) {
  for (Function &F : M.getFunctionList()) {
    for (BasicBlock &BB : F.getBasicBlockList()) {
      for (Instruction &I : BB.getInstList()) {
        if (!isa<IntrinsicInst>(I))
          continue;

        IntrinsicInst *II = dyn_cast<IntrinsicInst>(&I);
        Function *CF = II->getCalledFunction();
  
        if (CF->getName().compare("llvm.var.annotation") != 0)
          continue;

        Value *OP = II->getArgOperand(1);
  
        GlobalVariable *GV = dyn_cast<GlobalVariable>(OP);
        if (!GV) continue;
    
        ConstantDataSequential *CDS = dyn_cast<ConstantDataSequential>(GV->getInitializer());
        if (!CDS) continue;
    
        if (!CDS->isString() || CDS->getAsCString().compare("secret") != 0)
          continue;

        errs() << "Found secret! ";
    
        Value *Target = II->getArgOperand(0);
    
        errs() << Target->getName() << "\n";
    
        /*
        if (Instruction *TargetInstr = dyn_cast<Instruction>(Target)) {
          TargetInstr->dump();
          errs() << TargetInstr->getName() << "\n";
        }
        */
    
        SecretVar Secret = SecretVar();
        Secret.Func = F.getName();
        Secret.BB = BB.getName();
        Secret.Name = Target->getName().str();
    
        SecretVars.push_back(Secret);
      }
    }
  }
  return PreservedAnalyses::all();
}

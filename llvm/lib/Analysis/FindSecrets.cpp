#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/FindSecrets.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"


using namespace llvm;

#define DEBUG_TYPE "findsecrets"

AnalysisKey FindSecretsAnalysis::Key;

std::vector<SecretVar> FindSecretsAnalysis::run(Module &M, ModuleAnalysisManager &AM) {
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

        // It seems that Clang can't decide on whether to generate a GlobalVariable
        // or a getelementptr here, so we have to support both cases
        if (!GV) {
          ConstantExpr *CE = dyn_cast<ConstantExpr>(OP);
          if (!CE) continue;
          
          GV = dyn_cast<GlobalVariable>(CE->getOperand(0));
        }
        
        if (!GV) continue;

        ConstantDataSequential *CDS = dyn_cast<ConstantDataSequential>(GV->getInitializer());
        if (!CDS) continue;
    
        if (!CDS->isString() || CDS->getAsCString().compare("secret") != 0)
          continue;

        //errs() << "Found secret! \n";
    
        //II->dump();
        Value *Target = II->getArgOperand(0);
        //Target->dump();
    
        //errs() << Target->getName() << "\n";
    
        Instruction *TargetInstr = dyn_cast<Instruction>(Target);
        
        if (!TargetInstr) continue;
        
        //TargetInstr->dump();
        //errs() << TargetInstr->getName() << "\n";
    
        SecretVar Secret = SecretVar();
        Secret.Instr = TargetInstr;
        //Secret.Func = F.getName();
        //Secret.BB = BB.getName();
        //Secret.Name = Target->getName().str();
    
        SecretVars.push_back(Secret);
      }
    }
  }
  return SecretVars;
}

PreservedAnalyses FindSecretsPrinterPass::run(Module &M, ModuleAnalysisManager &AM) {
  auto SecretVars = AM.getResult<FindSecretsAnalysis>(M);
  
  for (SecretVar Secret : SecretVars) {
    Secret.Instr->dump();
    //OS << Secret.Func << "." << Secret.BB << "." << Secret.Name << "\n";
  }
  
  return PreservedAnalyses::all();
}

#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineFunction.h"
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
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"


using namespace llvm;

#define DEBUG_TYPE "find-secrets"

char FindSecretsAnalysis::ID = 0;

bool FindSecretsAnalysis::runOnMachineFunction(MachineFunction &MF) {
  auto &F = MF.getFunction();
  Secrets.Func = &F;
  
  for (auto &Arg : F.args()) {
    if (Arg.hasAttribute(Attribute::Secret)) {
      Secrets.Args.push_back(&Arg);
    }
  }

  auto FS = FunctionSecrets(&F);

  for (BasicBlock &BB : F) {
    //auto BS = BlockSecrets(&BB);
    for (Instruction &I : BB) {
      
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
      
      if (!CDS->isString())
        continue;
      
      bool Upgrade = CDS->getAsCString().compare("secret_upgrade") == 0;
      bool Downgrade = CDS->getAsCString().compare("secret_downgrade") == 0;
  
      if (!Upgrade && !Downgrade)
        continue;

      //II->dump();
      Value *Target = II->getArgOperand(0);
      //Target->dump();
  
      //errs() << Target->getName() << "\n";
  
      Instruction *TargetInstr = dyn_cast<Instruction>(Target);
      
      if (!TargetInstr) continue;
      
      //TargetInstr->dump();
      
      if (Upgrade) {
        //Secrets.Upgrades.push_back(TargetInstr);
      } else if (Downgrade) {
        //Secrets.Downgrades.push_back(TargetInstr);
      }
    }
  }
  return false;
}

FindSecretsAnalysis::FindSecretsAnalysis() : MachineFunctionPass(ID), Secrets() {
  initializeMachineCFGPrinterPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS(FindSecretsAnalysis, DEBUG_TYPE, "Find Secrets",
                false, true)

#define DEBUG_TYPE_2 "print-secrets"

char FindSecretsPrinter::ID = 0;

bool FindSecretsPrinter::runOnMachineFunction(MachineFunction &MF) {
  auto Secrets = getAnalysis<FindSecretsAnalysis>().Secrets;
  
  errs() << "Secrets for function: " << Secrets.Func->getName() << "\n";
  
  if (Secrets.ReturnSecret) {
    errs() << "Returns secret\n";
  }
  
  errs() << "Secret arguments:\n";
  
  for (auto &Arg : Secrets.Args) {
    Arg->dump();
  }

  return false;
}

FindSecretsPrinter::FindSecretsPrinter() : MachineFunctionPass(ID) {
  initializeMachineCFGPrinterPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(FindSecretsPrinter, DEBUG_TYPE_2,
    "Print secrets", false, false)
INITIALIZE_PASS_DEPENDENCY(FindSecretsAnalysis)
INITIALIZE_PASS_END(FindSecretsPrinter, DEBUG_TYPE_2,
    "Print secrets", false, false)


//===- Hello.cpp - Example code from "Writing an LLVM Pass" ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements two versions of the LLVM "Hello World" pass described
// in docs/WritingAnLLVMPass.html
//
//===----------------------------------------------------------------------===//

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
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
using namespace llvm;

#define DEBUG_TYPE "find_secrets"


namespace {
  struct SecretVar {
    std::string Func;
    std::string BB;
    std::string Name;
  };

  // Hello - The first implementation, without getAnalysisUsage.
  struct FindSecrets : public ModulePass {
    static char ID; // Pass identification, replacement for typeid
    std::vector<SecretVar> secretVars;
  
    FindSecrets() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
      for (Function &F : M.getFunctionList()) {
        errs() << F.getName() << "\n";
        for (BasicBlock &BB : F.getBasicBlockList()) {
          errs() << BB.getName() << "\n";
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
        
            secretVars.push_back(Secret);
          }
        }
      }
    
      return false;
    }
  };
} // namespace

char FindSecrets::ID = 0;
static RegisterPass<FindSecrets> X("find_secrets", "Find secrets");


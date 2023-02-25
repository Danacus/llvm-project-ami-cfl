//===-- HelloWorld.h - Example Transformations ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_CFL_H
#define LLVM_TRANSFORMS_SCALAR_CFL_H

#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Regex.h"

namespace llvm {

static inline void passListRegexInit(std::vector<Regex *> &regexes,
                                     const std::vector<std::string> &strings) {
  for (auto &s : strings)
    regexes.push_back(new Regex(s, 0));
}

static inline bool passListRegexMatch(const std::vector<Regex *> &regexes,
                                      const StringRef &string) {
  for (auto &regex : regexes) {
    if (regex->match(string))
      return true;
  }

  return false;
}

// static inline Function *passGetCalledFunction(Instruction *I) {
//   auto *CS = dyn_cast<CallBase>(I);
//   if (!CS)
//     return NULL; // not a call
//   return CS->getCalledFunction();
// }

// static inline CallInst *passCreateCallInstruction(FunctionCallee *F,
//                                                   std::vector<Value *> &args,
//                                                   const Twine &NameStr,
//                                                   Instruction *InsertBefore) {
//   ArrayRef<Value *> ref(args);
//   return CallInst::Create(F, ref, NameStr, InsertBefore);
// }

typedef long imd_t;

class IfCondition {
public:
  BranchInst *Branch;
  BasicBlock *MergePoint;
  BasicBlock *IfTrue;
  BasicBlock *IfFalse;
  BasicBlock *IfTruePred;
};

class CFLPass : public PassInfoMixin<CFLPass> {
  unsigned long CFLedFuncs = 0;
  unsigned long totalFuncs = 0;
  unsigned long linearizedBranches = 0;
  unsigned long totalBranches = 0;
  unsigned long totalIFCs = 0;

  void cfl(Function *F, DominatorTree *DT, PostDominatorTree *PDT);
  IfCondition *getIfCondition(DominatorTree *DT, PostDominatorTree *PDT,
                              BranchInst *BI);
  BasicBlock *getImmediatePostdominator(PostDominatorTree *PDT, BasicBlock *BB);
  void wrapCondition(IfCondition &IFC);
  void wrapUninterestingCondition(IfCondition &IFC);
  void wrapStore(StoreInst *SI);
  void wrapLoad(LoadInst *LI);
  void wrapExtCall(CallBase &CS, Function *Callee);
  void wrapIntrinsicCall(CallBase &CS, Function *Callee);
  ConstantInt *makeConstBool(LLVMContext &C, int value);
  ConstantInt *makeConstI32(LLVMContext &C, int value);
  int getIBID(Instruction &I);
  int getBGID(Instruction &I);
  bool getUninterestingDirection(Instruction &I);
  bool isInstructionUninteresting(Instruction &I);
  bool getInstructionTaint(Instruction &I);
  void setInstructionTaint(Instruction *I, bool taint);

public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_CFL_H

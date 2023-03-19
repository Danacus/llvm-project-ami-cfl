
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/PersistencyAnalysis.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/PassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "persistency-analysis"

void PersistencyAnalysisPass::propagatePersistency(
    const MachineFunction &MF, MachineInstr &MI, const MachineOperand &MO,
    const ActivatingRegion *MR) {
  LLVM_DEBUG(errs() << "propagatePersistency\n");
  LLVM_DEBUG(MI.dump());
  LLVM_DEBUG(MO.dump());
  LLVM_DEBUG(errs() << "\n");

  if (!MO.isReg())
    return;

  SmallVector<MachineInstr *> WorkSet;
  if (IsSSA) {
    for (auto &DI : MF.getRegInfo().def_instructions(MO.getReg())) {
      if (MR->contains(&DI))
        WorkSet.push_back(&DI);
    }
  } else {
    SmallPtrSet<MachineInstr *, 16> Defs;
    RDA->getGlobalReachingDefs(&MI, MO.getReg(), Defs);
    for (auto *DI : Defs) {
      if (MR->contains(DI))
        WorkSet.push_back(DI);
    }
  }

  while (!WorkSet.empty()) {
    auto *I = WorkSet.pop_back_val();
    LLVM_DEBUG(I->dump());

    if (PersistentInstructions[MR].contains(I))
      continue;
    PersistentInstructions[MR].insert(I);

    for (auto &Op : I->operands()) {
      if (!Op.isReg() || !Op.isUse())
        continue;

      Register Reg = Op.getReg();

      SmallPtrSet<MachineInstr *, 16> Defs;

      if (IsSSA) {
        for (auto &DI : MF.getRegInfo().def_instructions(Reg)) {
          Defs.insert(&DI);
        }
      } else {
        RDA->getGlobalReachingDefs(I, Reg, Defs);
      }

      for (auto *DI : Defs) {
        if (MR->contains(DI))
          WorkSet.push_back(DI);
        else
          PersistentRegionInputMap[MR].insert(DI);
      }
    }
  }

  LLVM_DEBUG(errs() << "Done propagating\n");
}

void PersistencyAnalysisPass::analyzeRegion(const MachineFunction &MF,
                                            const ActivatingRegion *MR) {
  LLVM_DEBUG(errs() << "Analyze region: \n");
  LLVM_DEBUG(MR->dump());

  for (auto *MBB : MR->blocks()) {
    SmallVector<MachineOperand, 4> LeakedOperands;
    for (MachineInstr &MI : *MBB) {
      if (TII->isPersistentStore(MI)) {
        PersistentStores[MR].insert(&MI);
      }
      if (MI.isCall()) {
        CallInstructions[MR].insert(&MI);
      }

      LeakedOperands.clear();
      TII->constantTimeLeakage(MI, LeakedOperands);

      for (auto &MO : LeakedOperands) {
        propagatePersistency(MF, MI, MO, MR);
      }
    }
  }
}

bool PersistencyAnalysisPass::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  ALA = &getAnalysis<AMiLinearizationAnalysis>();

  if (!IsSSA) {
    RDA = &getAnalysis<ReachingDefAnalysis>();
  }

  PersistentStores.clear();
  PersistentInstructions.clear();
  CallInstructions.clear();
  PersistentRegionInputMap.clear();

  for (auto &Pair : ALA->ActivatingRegions) {
    analyzeRegion(MF, &Pair.getSecond());
  }

  LLVM_DEBUG(errs() << "Persistent instructions: \n");

  for (const auto &Pair : PersistentInstructions) {
    LLVM_DEBUG(Pair.first->dump());

    for (const auto *MI : Pair.second) {
      LLVM_DEBUG(MI->dump());
    }
  }

  LLVM_DEBUG(errs() << "Persistent stores: \n");

  for (const auto &Pair : PersistentStores) {
    LLVM_DEBUG(Pair.first->dump());

    for (const auto *MI : Pair.second) {
      LLVM_DEBUG(MI->dump());
    }
  }

  return false;
}

char PersistencyAnalysisPass::ID = 0;
char &llvm::PersistencyAnalysisPassID = PersistencyAnalysisPass::ID;

PersistencyAnalysisPass::PersistencyAnalysisPass(bool IsSSA)
    : MachineFunctionPass(ID), IsSSA(IsSSA) {
  initializePersistencyAnalysisPassPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(PersistencyAnalysisPass, DEBUG_TYPE,
                      "Persistency Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(AMiLinearizationAnalysis)
INITIALIZE_PASS_DEPENDENCY(ReachingDefAnalysis)
INITIALIZE_PASS_END(PersistencyAnalysisPass, DEBUG_TYPE, "Persistency Analysis",
                    true, true)

namespace llvm {

FunctionPass *createPersistencyAnalysisPass(bool IsSSA) {
  return new PersistencyAnalysisPass(IsSSA);
}

} // namespace llvm

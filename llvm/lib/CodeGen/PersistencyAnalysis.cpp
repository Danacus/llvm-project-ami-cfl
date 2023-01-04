
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
    const MachineFunction &MF, const MachineInstr &MI, const MachineOperand &MO,
    const MachineRegion &MR,
    SmallPtrSet<MachineInstr *, 16> &PersistentDefs) {
  errs() << "propagatePersistency\n";
  MI.dump();
  MO.dump();
  MR.dump();
  errs() << "\n";

  if (!MO.isReg())
    return;

  SmallVector<MachineInstr *> WorkSet;
  for (auto &DI : MF.getRegInfo().def_instructions(MO.getReg())) {
    if (MR.contains(&DI))
      WorkSet.push_back(&DI);
  }

  while (!WorkSet.empty()) {
    auto *I = WorkSet.pop_back_val();
    I->dump();

    PersistentDefs.insert(I);

    for (auto &Op : I->operands()) {
      if (!Op.isReg() || !Op.isUse())
        continue;

      Register Reg = Op.getReg();

      // SmallPtrSet<MachineInstr *, 16> Defs;
      // RDA.getGlobalReachingDefs(I, Reg, Defs);

      for (auto &DI : MF.getRegInfo().def_instructions(Reg)) {
        if (MR.contains(&DI))
          WorkSet.push_back(&DI);
        else
          PersistentRegionInputMap[&MR].insert(&DI);
      }
    }
  }
}

void PersistencyAnalysisPass::analyzeRegion(const MachineFunction &MF,
                                            const MachineRegion &MR,
                                            const MachineRegion &Scope) {
  errs() << "Analyze region: \n";
  MR.dump();
  errs() << "in scope\n";
  Scope.dump();
  errs() << "\n";

  SmallPtrSet<MachineInstr *, 16> *LocalPersistentDefs =
      &PersistentInstructions[&Scope];

  for (const auto *Node : MR.elements()) {
    if (Node->isSubRegion()) {
      analyzeRegion(MF, *Node->getNodeAs<MachineRegion>(), Scope);
    } else {
      MachineBasicBlock *MBB = Node->getNodeAs<MachineBasicBlock>();
      SmallVector<MachineOperand, 4> LeakedOperands;
      for (MachineInstr &MI : *MBB) {
        LeakedOperands.clear();
        TII->constantTimeLeakage(MI, LeakedOperands);

        if (TII->isPersistentStore(MI)) {
          if (&Scope == &MR)
            PersistentStores[&Scope].insert(&MI);
        }

        for (auto &MO : LeakedOperands) {
          propagatePersistency(MF, MI, MO, Scope, *LocalPersistentDefs);
        }
      }
    }
  }
}

bool PersistencyAnalysisPass::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  SRA = &getAnalysis<SensitiveRegionAnalysisVirtReg>().getSRA();
  for (auto &B : SRA->sensitive_branches()) {
    errs() << "Sensitive branch: " << B.MBB->getFullName() << "\n";
    errs() << "if region:\n";
    B.IfRegion->dump();

    if (B.ElseRegion) {
      errs() << "else region:\n";
      B.ElseRegion->dump();
    }
  }
  auto Branches = SmallVector<SensitiveBranch>(SRA->sensitive_branches());
  std::sort(Branches.begin(), Branches.end());

  for (auto &Branch : Branches) {
    analyzeRegion(MF, *Branch.IfRegion);
    analyzeRegion(MF, *Branch.ElseRegion);
  }

  errs() << "Persistent instructions: \n";

  for (const auto &Pair : PersistentInstructions) {
    Pair.first->dump();

    for (const auto *MI : Pair.second) {
      MI->dump();
    }
  }

  return false;
}

char PersistencyAnalysisPass::ID = 0;
char &llvm::PersistencyAnalysisPassID = PersistencyAnalysisPass::ID;

PersistencyAnalysisPass::PersistencyAnalysisPass() : MachineFunctionPass(ID) {
  initializePersistencyAnalysisPassPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(PersistencyAnalysisPass, DEBUG_TYPE,
                      "Persistency Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(SensitiveRegionAnalysisVirtReg)
INITIALIZE_PASS_END(PersistencyAnalysisPass, DEBUG_TYPE, "Persistency Analysis",
                    true, true)

namespace llvm {

FunctionPass *createPersistencyAnalysisPass() {
  return new PersistencyAnalysisPass();
}

} // namespace llvm

#include "RISCVAMiInsertPersistentDefs.h"
#include "RISCV.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/PHIEliminationUtils.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/CodeGen/TargetInstrInfo.h"

using namespace llvm;

#define DEBUG_TYPE "ami-insert-persistent-defs"

char AMiInsertPersistentDefs::ID = 0;

void AMiInsertPersistentDefs::insertImplicitDef(MachineFunction &MF, MachineRegion &MR, Register Reg) {
  SmallVector<MachineBasicBlock *> Exitings;
  MR.getExitingBlocks(Exitings);

  for (auto &Exiting : Exitings) {
    auto InsertPoint = findPHICopyInsertPoint(Exiting, MR.getExit(), Reg);
    BuildMI(*Exiting, InsertPoint, DebugLoc(), TII->get(TargetOpcode::IMPLICIT_DEF), Reg);
    BuildMI(*Exiting, InsertPoint, DebugLoc(), TII->get(TargetOpcode::EXTEND)).addReg(Reg);
  }
}

bool AMiInsertPersistentDefs::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  auto &SRA = getAnalysis<SensitiveRegionAnalysisPass>();
  auto &PA = getAnalysis<PersistencyAnalysisPass>();
  auto &SensitiveBranches = SRA.getSensitiveBranches();

  for (auto &B : SensitiveBranches) {
    if (B.ElseRegion) {
      auto PersistentInstrs = PA.getPersistentInstructions(B.ElseRegion);

      for (auto *MI : PersistentInstrs) {
        for (auto &Def : MI->defs()) {
          if (Def.isReg())
            insertImplicitDef(MF, *B.IfRegion, Def.getReg());
        }
      }
    }
  }

  MF.dump();

  return true;
}

AMiInsertPersistentDefs::AMiInsertPersistentDefs() : MachineFunctionPass(ID) {
  initializeAMiInsertPersistentDefsPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AMiInsertPersistentDefs, DEBUG_TYPE, "AMi Insert Persistent Defs",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(SensitiveRegionAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(PersistencyAnalysisPass)
INITIALIZE_PASS_END(AMiInsertPersistentDefs, DEBUG_TYPE, "AMi Insert Persistent Defs",
                    false, false)

namespace llvm {

FunctionPass *createAMiInsertPersistentDefsPass() {
  return new AMiInsertPersistentDefs();
}

} // namespace llvm

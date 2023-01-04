#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/PHIEliminationUtils.h"
#include "llvm/CodeGen/RemoveBranchPseudos.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "remove-branch-pseudos"

char RemoveBranchPseudos::ID = 0;
char &llvm::RemoveBranchPseudosPassID = RemoveBranchPseudos::ID;

bool RemoveBranchPseudos::runOnMachineFunction(MachineFunction &MF) {
  SmallPtrSet<MachineInstr *, 8> ToRemove;

  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      if (MI.getOpcode() == TargetOpcode::BRANCH_TARGET ||
          MI.getOpcode() == TargetOpcode::SECRET_DEP_BR)
        ToRemove.insert(&MI);
    }
  }

  for (auto *MI : ToRemove) {
    MI->eraseFromParent();
  }

  LLVM_DEBUG(MF.dump());
  return true;
}

RemoveBranchPseudos::RemoveBranchPseudos() : MachineFunctionPass(ID) {
  initializeRemoveBranchPseudosPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(RemoveBranchPseudos, DEBUG_TYPE,
                      "Remove Persistent Defs", false, false)
INITIALIZE_PASS_END(RemoveBranchPseudos, DEBUG_TYPE, "Remove Persistent Defs",
                    false, false)

namespace llvm {

FunctionPass *createRemoveBranchPseudosPass() {
  return new RemoveBranchPseudos();
}

} // namespace llvm

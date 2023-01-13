#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/PHIEliminationUtils.h"
#include "llvm/CodeGen/RemoveSecretPseudos.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "remove-secret-pseudos"

char RemoveSecretPseudos::ID = 0;
char &llvm::RemoveSecretPseudosPassID = RemoveSecretPseudos::ID;

bool RemoveSecretPseudos::runOnMachineFunction(MachineFunction &MF) {
  SmallPtrSet<MachineInstr *, 8> ToRemove;

  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      if (MI.getOpcode() == TargetOpcode::SECRET)
        ToRemove.insert(&MI);
    }
  }

  for (auto *MI : ToRemove) {
    MI->eraseFromParent();
  }

  LLVM_DEBUG(MF.dump());
  return true;
}

RemoveSecretPseudos::RemoveSecretPseudos() : MachineFunctionPass(ID) {
  initializeRemoveSecretPseudosPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(RemoveSecretPseudos, DEBUG_TYPE,
                      "Remove Secret Pseudos", false, false)
INITIALIZE_PASS_END(RemoveSecretPseudos, DEBUG_TYPE, "Remove Secret Pseudos",
                    false, false)

namespace llvm {

FunctionPass *createRemoveSecretPseudosPass() {
  return new RemoveSecretPseudos();
}

} // namespace llvm

#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/PHIEliminationUtils.h"
#include "llvm/CodeGen/RemovePersistentDefs.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "remove-persistent-defs"

char RemovePersistentDefs::ID = 0;
char &llvm::RemovePersistentDefsPassID = RemovePersistentDefs::ID;

bool RemovePersistentDefs::runOnMachineFunction(MachineFunction &MF) {
  SmallPtrSet<MachineInstr *, 8> ToRemove;

  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      if (MI.getOpcode() == TargetOpcode::PERSISTENT_DEF ||
          MI.getOpcode() == TargetOpcode::EXTEND)
        ToRemove.insert(&MI);
    }
  }

  for (auto *MI : ToRemove) {
    MI->eraseFromParent();
  }

  LLVM_DEBUG(MF.dump());
  return true;
}

RemovePersistentDefs::RemovePersistentDefs() : MachineFunctionPass(ID) {
  initializeRemovePersistentDefsPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(RemovePersistentDefs, DEBUG_TYPE,
                      "Remove Persistent Defs", false, false)
INITIALIZE_PASS_END(RemovePersistentDefs, DEBUG_TYPE, "Remove Persistent Defs",
                    false, false)

namespace llvm {

FunctionPass *createRemovePersistentDefsPass() {
  return new RemovePersistentDefs();
}

} // namespace llvm

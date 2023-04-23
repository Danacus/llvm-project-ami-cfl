#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/PHIEliminationUtils.h"
#include "llvm/CodeGen/RemoveConflictingDefs.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "remove-conflicting-defs"

char RemoveConflictingDefs::ID = 0;
char &llvm::RemoveConflictingDefsPassID = RemoveConflictingDefs::ID;

bool RemoveConflictingDefs::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();

  SmallPtrSet<MachineInstr *, 8> ToRemove;

  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      if (MI.getOpcode() == TargetOpcode::PERSISTENT_DEF ||
          MI.getOpcode() == TargetOpcode::EXTEND)
        ToRemove.insert(&MI);
    }
  }

  SmallPtrSet<MachineBasicBlock *, 8> BlocksToRemove;

  for (auto *MI : ToRemove) {
    auto *Parent = MI->getParent();
    MI->eraseFromParent();
    if (Parent->empty() || Parent->getFirstTerminator() == Parent->begin()) {
      BlocksToRemove.insert(Parent);
    }
  }

  for (auto *MBB : BlocksToRemove) {
    assert(MBB->pred_size() == 1 && "Temporary block should have single predecessor");
    assert(MBB->succ_size() == 1 && "Temporary block should have single successor");

    MachineBasicBlock *Pred = *MBB->pred_begin();
    MachineBasicBlock *Succ = MBB->getSingleSuccessor();

    MachineBasicBlock *TBB;
    MachineBasicBlock *FBB;
    SmallVector<MachineOperand> Cond;
    TII->analyzeBranch(*Pred, TBB, FBB, Cond, false);

    assert(TBB == MBB || FBB == MBB && "TBB or FBB should be the temporary block");
  
    TII->removeBranch(*Pred);
    if (TBB == MBB) {
      TII->insertBranch(*Pred, Succ, FBB, Cond, DebugLoc());
    } else {
      TII->insertBranch(*Pred, TBB, Succ, Cond, DebugLoc());
    }
    MBB->removeSuccessor(Succ);
    Pred->removeSuccessor(MBB);
    Pred->addSuccessor(Succ);
  }

  for (auto *MBB : BlocksToRemove) {    
    MBB->eraseFromParent();
  }

  LLVM_DEBUG(MF.dump());
  return true;
}

RemoveConflictingDefs::RemoveConflictingDefs() : MachineFunctionPass(ID) {
  initializeRemoveConflictingDefsPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(RemoveConflictingDefs, DEBUG_TYPE,
                      "Remove Conflicting Defs", false, false)
INITIALIZE_PASS_END(RemoveConflictingDefs, DEBUG_TYPE, "Remove Conflicting Defs",
                    false, false)

namespace llvm {

FunctionPass *createRemoveConflictingDefsPass() {
  return new RemoveConflictingDefs();
}

} // namespace llvm

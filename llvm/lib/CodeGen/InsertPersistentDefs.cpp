#include "llvm/CodeGen/InsertPersistentDefs.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/PHIEliminationUtils.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "insert-persistent-defs"

char InsertPersistentDefs::ID = 0;
char &llvm::InsertPersistentDefsPassID = InsertPersistentDefs::ID;

void InsertPersistentDefs::insertPersistentDef(MachineFunction &MF,
                                               MachineRegion &MR,
                                               Register Reg) {
  auto &LV = getAnalysis<LiveVariables>();
  auto *LIS = getAnalysisIfAvailable<LiveIntervals>();
  SmallVector<MachineBasicBlock *> Exitings;
  MR.getExitingBlocks(Exitings);

  for (auto &Exiting : Exitings) {
    auto InsertPoint = Exiting->getFirstTerminator();
    auto DefBuilder = BuildMI(*Exiting, InsertPoint, DebugLoc(),
                              TII->get(TargetOpcode::PERSISTENT_DEF), Reg);
    auto ExtendBuilder = BuildMI(*Exiting, InsertPoint, DebugLoc(),
                                 TII->get(TargetOpcode::EXTEND))
                             .addReg(Reg);

    for (unsigned RegI = 0; RegI < MF.getRegInfo().getNumVirtRegs(); RegI++) {
      Register OtherReg = Register::index2VirtReg(RegI);
      if (LIS) {
        if (LIS->hasInterval(OtherReg)) {
          if (LIS->isLiveOutOfMBB(LIS->getInterval(OtherReg), Exiting))
            ExtendBuilder.addReg(OtherReg);
        }
      } else {
        if (OtherReg.isVirtual() && LV.isLiveOut(OtherReg, *Exiting)) {
          if (LV.isLiveIn(OtherReg, *Exiting->getSingleSuccessor()))
            ExtendBuilder.addReg(OtherReg);
        }
      }
    }

    if (LIS) {
      SlotIndex MBBStartIndex = LIS->getMBBStartIdx(Exiting);
      SlotIndex DefIndex = LIS->InsertMachineInstrInMaps(*DefBuilder);
      SlotIndex ExtendIndex = LIS->InsertMachineInstrInMaps(*ExtendBuilder);

      LiveInterval &DefLI = LIS->getInterval(Reg);
      VNInfo *DefVNI = DefLI.getVNInfoAt(MBBStartIndex);
      if (!DefVNI)
        DefVNI = DefLI.getNextValue(MBBStartIndex, LIS->getVNInfoAllocator());
      DefLI.addSegment(LiveInterval::Segment(DefIndex, ExtendIndex, DefVNI));

      for (MachineOperand &MO : ExtendBuilder->operands()) {
        if (!MO.isReg() || !MO.isUse())
          continue;

        LiveInterval &IncomingLI = LIS->getInterval(MO.getReg());
        LIS->extendToIndices(IncomingLI, ExtendIndex);
      }
    }
  }
}

void InsertPersistentDefs::insertPersistentDef(MachineInstr *MI) {
  auto &SRA = getAnalysis<SensitiveRegionAnalysisPass>();
  auto *MBB = MI->getParent();

  for (auto &Branch : SRA.sensitive_branches(MBB, true)) {
    for (auto &Def : MI->defs()) {
      if (Def.isReg())
        insertPersistentDef(*MBB->getParent(), *Branch.IfRegion, Def.getReg());
    }
  }
}

void InsertPersistentDefs::insertGhostLoad(MachineInstr *StoreMI) {
  auto *MBB = StoreMI->getParent();
  auto *MF = MBB->getParent();
  auto &MRI = MF->getRegInfo();

  // TODO: Should be moved to target-specific code
  Register Reg = StoreMI->getOperand(0).getReg();
  Register NewReg = MRI.createVirtualRegister(MRI.getRegClass(Reg));
  auto GhostMI = BuildMI(*MBB, StoreMI->getIterator(), DebugLoc(), TII->get(TargetOpcode::GHOST_LOAD), NewReg).addReg(Reg);
  StoreMI->getOperand(0).setReg(NewReg);
  insertPersistentDef(&*GhostMI);
}

bool InsertPersistentDefs::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  auto &SRA = getAnalysis<SensitiveRegionAnalysisPass>();
  auto &PA = getAnalysis<PersistencyAnalysisPass>();

  for (auto &B : SRA.sensitive_branches()) {
    for (auto *MI : PA.getPersistentStores(B.IfRegion)) {
      insertGhostLoad(MI);
    }

    if (B.ElseRegion) {
      auto PersistentInstrs = PA.getPersistentInstructions(B.ElseRegion);

      for (auto *MI : PersistentInstrs) {
        for (auto &Def : MI->defs()) {
          if (Def.isReg())
            insertPersistentDef(MF, *B.IfRegion, Def.getReg());
        }
      }

      for (auto *MI : PA.getPersistentStores(B.ElseRegion)) {
        insertGhostLoad(MI);
      }
    }
  }

  MF.dump();

  return true;
}

InsertPersistentDefs::InsertPersistentDefs() : MachineFunctionPass(ID) {
  initializeInsertPersistentDefsPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(InsertPersistentDefs, DEBUG_TYPE,
                      "Insert Persistent Defs", false, false)
INITIALIZE_PASS_DEPENDENCY(SensitiveRegionAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(PersistencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(LiveVariables)
INITIALIZE_PASS_END(InsertPersistentDefs, DEBUG_TYPE, "Insert Persistent Defs",
                    false, false)

namespace llvm {

FunctionPass *createInsertPersistentDefsPass() {
  return new InsertPersistentDefs();
}

} // namespace llvm

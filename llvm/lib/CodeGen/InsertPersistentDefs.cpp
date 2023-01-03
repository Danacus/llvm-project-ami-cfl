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

void InsertPersistentDefs::insertPersistentDefEnd(MachineFunction &MF,
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
          if (LV.isLiveIn(OtherReg, *MR.getExit()))
            ExtendBuilder.addReg(OtherReg);
        }
      }
    }

    updateLiveIntervals(Exiting, *DefBuilder, *ExtendBuilder, Reg);
  }
}

void InsertPersistentDefs::insertPersistentDefEnd(MachineInstr *MI) {
  auto &SRA = getAnalysis<SensitiveRegionAnalysisPass>();
  auto *MBB = MI->getParent();

  for (auto &Branch : SRA.sensitive_branches(MBB, true)) {
    for (auto &Def : MI->defs()) {
      if (Def.isReg())
        insertPersistentDefEnd(*MBB->getParent(), *Branch.IfRegion,
                               Def.getReg());
    }
  }
}

void InsertPersistentDefs::insertPersistentDefStart(MachineFunction &MF,
                                                    MachineRegion &MR,
                                                    Register Reg) {
  auto &LV = getAnalysis<LiveVariables>();
  auto *LIS = getAnalysisIfAvailable<LiveIntervals>();

  auto *Entry = MR.getEntry();
  auto InsertPoint = Entry->begin();
  auto DefBuilder = BuildMI(*Entry, InsertPoint, DebugLoc(),
                            TII->get(TargetOpcode::PERSISTENT_DEF), Reg);
  auto ExtendBuilder =
      BuildMI(*Entry, InsertPoint, DebugLoc(), TII->get(TargetOpcode::EXTEND))
          .addReg(Reg);

  for (unsigned RegI = 0; RegI < MF.getRegInfo().getNumVirtRegs(); RegI++) {
    Register OtherReg = Register::index2VirtReg(RegI);
    if (LIS) {
      if (LIS->hasInterval(OtherReg)) {
        if (LIS->isLiveInToMBB(LIS->getInterval(OtherReg), Entry))
          ExtendBuilder.addReg(OtherReg);
      }
    } else {
      if (OtherReg.isVirtual() && LV.isLiveIn(OtherReg, *Entry)) {
        ExtendBuilder.addReg(OtherReg);
      }
    }
  }

  updateLiveIntervals(Entry, *DefBuilder, *ExtendBuilder, Reg);
}

void InsertPersistentDefs::insertPersistentDefStart(MachineInstr *MI) {
  auto &SRA = getAnalysis<SensitiveRegionAnalysisPass>();
  auto *MBB = MI->getParent();

  for (auto &Branch : SRA.sensitive_branches(MBB, false)) {
    if (!Branch.ElseRegion)
      continue;
    for (auto &Def : MI->defs()) {
      if (Def.isReg()) {
        insertPersistentDefStart(*MBB->getParent(), *Branch.ElseRegion,
                                 Def.getReg());
      }
    }
  }
}

void InsertPersistentDefs::insertPersistentDef(MachineInstr *MI) {
  insertPersistentDefStart(MI);
  insertPersistentDefEnd(MI);
}

void InsertPersistentDefs::updateLiveIntervals(MachineBasicBlock *MBB,
                                               MachineInstr &Def,
                                               MachineInstr &Extend,
                                               Register Reg) {
  auto *LIS = getAnalysisIfAvailable<LiveIntervals>();

  if (LIS) {
    SlotIndex MBBStartIndex = LIS->getMBBStartIdx(MBB);
    SlotIndex DefIndex = LIS->InsertMachineInstrInMaps(Def);
    SlotIndex ExtendIndex = LIS->InsertMachineInstrInMaps(Extend);

    LiveInterval &DefLI = LIS->getInterval(Reg);
    VNInfo *DefVNI = DefLI.getVNInfoAt(MBBStartIndex);
    if (!DefVNI)
      DefVNI = DefLI.getNextValue(MBBStartIndex, LIS->getVNInfoAllocator());
    DefLI.addSegment(LiveInterval::Segment(DefIndex, ExtendIndex, DefVNI));

    for (MachineOperand &MO : Extend.operands()) {
      if (!MO.isReg() || !MO.isUse())
        continue;

      LiveInterval &IncomingLI = LIS->getInterval(MO.getReg());
      LIS->extendToIndices(IncomingLI, ExtendIndex);
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
  auto GhostMI = BuildMI(*MBB, StoreMI->getIterator(), DebugLoc(),
                         TII->get(TargetOpcode::GHOST_LOAD), NewReg)
                     .addReg(Reg);
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
    if (B.ElseRegion) {
      auto ElsePersistentInstrs = PA.getPersistentInstructions(B.ElseRegion);

      for (auto *MI : ElsePersistentInstrs) {
        for (auto &Def : MI->defs()) {
          if (Def.isReg())
            insertPersistentDefEnd(MF, *B.IfRegion, Def.getReg());
        }
      }

      for (auto *MI : PA.getPersistentStores(B.ElseRegion)) {
        insertGhostLoad(MI);
      }

      auto IfPersistentInstrs = PA.getPersistentInstructions(B.IfRegion);

      for (auto *MI : IfPersistentInstrs) {
        for (auto &Def : MI->defs()) {
          if (Def.isReg())
            insertPersistentDefStart(MF, *B.ElseRegion, Def.getReg());
        }
      }

      for (auto *MI : PA.getPersistentStores(B.IfRegion)) {
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

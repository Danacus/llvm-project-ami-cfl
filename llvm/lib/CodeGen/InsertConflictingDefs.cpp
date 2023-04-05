#include "llvm/CodeGen/InsertConflictingDefs.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/LiveInterval.h"
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

#define DEBUG_TYPE "insert-conflicting-defs"

char InsertConflictingDefs::ID = 0;
char &llvm::InsertConflictingDefsPassID = InsertConflictingDefs::ID;

void InsertConflictingDefs::addConstraints(MachineInstr *MI) {
  auto &ALA = getAnalysis<AMiLinearizationAnalysis>();
  auto *MBB = MI->getParent();

  LLVM_DEBUG(MI->dump());

  // For each activating region that contains this instruction
  for (auto *AR : ALA.RegionMap[MBB]) {
    MachineBasicBlock *ConstraintMBB = ConstraintMBBMap[AR];
    if (!ConstraintMBB) {
      // Top level region, no need to add constraints
      continue;
    }

    for (auto &Def : MI->defs()) {
      auto Reg = Def.getReg();

      auto Extend = BuildMI(*ConstraintMBB, ConstraintMBB->begin(), DebugLoc(),
              TII->get(TargetOpcode::EXTEND))
          .addReg(Reg);
      auto PDef = BuildMI(*ConstraintMBB, ConstraintMBB->begin(), DebugLoc(),
              TII->get(TargetOpcode::PERSISTENT_DEF), Reg);

      if (LIS) {
        SlotIndex Start = LIS->InsertMachineInstrInMaps(*PDef);
        SlotIndex End = LIS->InsertMachineInstrInMaps(*Extend);
        auto &LI = LIS->getInterval(Reg);
        VNInfo *VNI = LI.getVNInfoAt(LIS->getInstructionIndex(*MI));
        LI.addSegment(LiveRange::Segment(Start, End, VNI));
      }
    }
  }
}

void InsertConflictingDefs::insertGhostLoad(MachineInstr *StoreMI) {
  auto *MBB = StoreMI->getParent();
  auto *MF = MBB->getParent();
  MRI = &MF->getRegInfo();

  MachineBasicBlock::iterator Iter = std::prev(StoreMI->getIterator());

  // Don't insert duplicate GHOST_LOAD
  if (Iter.isValid() && Iter->getOpcode() == TargetOpcode::GHOST_LOAD &&
      Iter->getOperand(0).getReg() == StoreMI->getOperand(0).getReg())
    return;

  // TODO: Should be moved to target-specific code
  Register Reg = StoreMI->getOperand(0).getReg();
  Register NewReg = MRI->createVirtualRegister(MRI->getRegClass(Reg));
  auto GhostMI = BuildMI(*MBB, StoreMI->getIterator(), DebugLoc(),
                         TII->get(TargetOpcode::GHOST_LOAD), NewReg)
                     .addReg(Reg);
  StoreMI->getOperand(0).setReg(NewReg);

  addConstraints(&*GhostMI);
}

MachineBasicBlock *InsertConflictingDefs::createConstraintMBB(
    MachineFunction &MF, MachineBasicBlock *From, MachineBasicBlock *To) {
  MachineBasicBlock *MBB = MF.CreateMachineBasicBlock();

  From->removeSuccessor(To);
  From->addSuccessor(MBB);
  MBB->addSuccessor(To);

  MF.insert(MF.end(), MBB);

  MachineBasicBlock *TBB;
  MachineBasicBlock *FBB;
  SmallVector<MachineOperand> Cond;
  TII->analyzeBranch(*From, TBB, FBB, Cond);
  if (TBB == To)
    TBB = MBB;
  else if (FBB == To)
    FBB = MBB;
  TII->removeBranch(*From);
  TII->insertBranch(*From, TBB, FBB, Cond, DebugLoc());
  TII->insertUnconditionalBranch(*MBB, To, DebugLoc());

  auto *MDT = getAnalysisIfAvailable<MachineDominatorTree>();
  if (MDT) {
    MDT->addNewBlock(MBB, From);
  }
  
  return MBB;
}

bool InsertConflictingDefs::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  ConstraintMBBMap.clear();

  auto &PA = getAnalysis<PersistencyAnalysisPass>();
  auto &ALA = getAnalysis<AMiLinearizationAnalysis>();
  LIS = getAnalysisIfAvailable<LiveIntervals>();
  // assert(LIS && "LIS must be available");

  for (auto &Pair : ALA.ActivatingRegions) {
    auto &Region = Pair.getSecond();
    if (!Region.Branch || !Region.Exit) {
      // Top level region, no need to add constraints
      continue;
    }
    ConstraintMBBMap.insert(
        {&Region, createConstraintMBB(MF, Region.Branch, Region.Exit)});
  }

  for (auto &Pair : ALA.ActivatingRegions) {
    auto &Region = Pair.getSecond();
    if (Region.Branch && Region.Exit) {
      auto PersistentInstrs = PA.getPersistentInstructions(&Region);
      for (auto *MI : PersistentInstrs) {
        addConstraints(MI);
      }
    }
    for (auto *MI : PA.getPersistentStores(&Region))
      insertGhostLoad(MI);
  }

  LLVM_DEBUG(MF.dump());

  return true;
}

InsertConflictingDefs::InsertConflictingDefs() : MachineFunctionPass(ID) {
  initializeInsertConflictingDefsPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(InsertConflictingDefs, DEBUG_TYPE,
                      "Insert Conflicting Defs", false, false)
INITIALIZE_PASS_DEPENDENCY(AMiLinearizationAnalysis)
INITIALIZE_PASS_DEPENDENCY(PersistencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(LiveIntervals)
INITIALIZE_PASS_END(InsertConflictingDefs, DEBUG_TYPE,
                    "Insert Conflicting Defs", false, false)

namespace llvm {

FunctionPass *createInsertConflictingDefsPass() {
  return new InsertConflictingDefs();
}

} // namespace llvm

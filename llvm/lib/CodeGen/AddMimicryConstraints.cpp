#include "llvm/CodeGen/AddMimicryConstraints.h"
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

#define DEBUG_TYPE "add-mimicry-constraints"

char AddMimicryConstraints::ID = 0;
char &llvm::AddMimicryConstraintsPassID = AddMimicryConstraints::ID;

void AddMimicryConstraints::addConstraintsToIfRegions(MachineInstr *MI) {
  auto &SRA = getAnalysis<SensitiveRegionAnalysis>();
  auto *MBB = MI->getParent();

  MachineFunction &MF = *MBB->getParent();
  MI->dump();

  for (auto &Branch : SRA.sensitive_branches(MBB, true)) {
    if (!Branch.ElseRegion->contains(MI))
      continue;

    MachineRegion &MR = *Branch.IfRegion;
    SmallVector<MachineBasicBlock *> Exitings;
    MR.getExitingBlocks(Exitings);

    for (auto &Exiting : Exitings) {
      for (unsigned RegI = 0; RegI < MF.getRegInfo().getNumVirtRegs(); RegI++) {
        Register OtherReg = Register::index2VirtReg(RegI);
        if (LIS->hasInterval(OtherReg)) {
          LiveInterval &OtherI = LIS->getInterval(OtherReg);
          if (LIS->isLiveOutOfMBB(OtherI, Exiting)) {
            addConstraint(OtherI, LIS->getMBBEndIdx(Exiting).getPrevIndex(), MI);
          }
        }
      }
    }
  }
}

void AddMimicryConstraints::addConstraintsToElseRegions(MachineInstr *MI) {
  auto &SRA = getAnalysis<SensitiveRegionAnalysis>();
  auto *MBB = MI->getParent();

  MachineFunction &MF = *MBB->getParent();
  MI->dump();

  for (auto &Branch : SRA.sensitive_branches(MBB, false)) {
    if (!Branch.ElseRegion)
      continue;

    if (!Branch.IfRegion->contains(MI))
      continue;

    MachineRegion &MR = *Branch.ElseRegion;
    auto *Entry = MR.getEntry();

    for (unsigned RegI = 0; RegI < MF.getRegInfo().getNumVirtRegs(); RegI++) {
      Register OtherReg = Register::index2VirtReg(RegI);
      if (LIS->hasInterval(OtherReg)) {
        LiveInterval &OtherI = LIS->getInterval(OtherReg);
        if (LIS->isLiveInToMBB(OtherI, Entry)) {
          addConstraint(OtherI, LIS->getMBBStartIdx(Entry), MI);
        }
      }
    }
  }
}

void AddMimicryConstraints::addConstraintsToRegions(MachineInstr *MI) {
  addConstraintsToElseRegions(MI);
  addConstraintsToIfRegions(MI);
}

void AddMimicryConstraints::addConstraint(LiveInterval &LI, SlotIndex SI, MachineInstr *ConflictingMI) {
  // SlotIndex Start =
  //     LIS->getSlotIndexes()->getInstructionIndex(*ConflictingMI).getRegSlot();
  SlotIndex End =
      LIS->getSlotIndexes()->getIndexAfter(*ConflictingMI).getRegSlot();
  SlotIndex Start = End.getPrevIndex().getRegSlot();
  VNInfo *VNI = LI.getVNInfoAt(SI);
  if (!LI.liveAt(Start))
    LI.addSegment(LiveRange::Segment(Start, End, VNI));
}

void AddMimicryConstraints::insertGhostLoad(MachineInstr *StoreMI) {
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
  LIS->InsertMachineInstrInMaps(*GhostMI);

  addConstraintsToRegions(&*GhostMI);
}

bool AddMimicryConstraints::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  auto &SRA = getAnalysis<SensitiveRegionAnalysis>();
  const auto *MRI = SRA.getRegionInfo();
  auto &PA = getAnalysis<PersistencyAnalysisPass>();
  LIS = getAnalysisIfAvailable<LiveIntervals>();
  assert(LIS && "LIS must be available");

  for (auto *MI : PA.getPersistentStores(MRI->getTopLevelRegion()))
    insertGhostLoad(MI);

  for (auto &B : SRA.sensitive_branches()) {
    if (B.ElseRegion) {
      auto ElsePersistentInstrs = PA.getPersistentInstructions(B.ElseRegion);
      for (auto *MI : ElsePersistentInstrs)
        addConstraintsToIfRegions(MI);
      for (auto *MI : PA.getPersistentStores(B.ElseRegion))
        insertGhostLoad(MI);

      auto IfPersistentInstrs = PA.getPersistentInstructions(B.IfRegion);
      for (auto *MI : IfPersistentInstrs)
        addConstraintsToElseRegions(MI);
      for (auto *MI : PA.getPersistentStores(B.IfRegion))
        insertGhostLoad(MI);
    }
  }

  for (auto &B : SRA.sensitive_branches()) {
    SmallVector<MachineBasicBlock *> Exitings;
    B.IfRegion->getExitingBlocks(Exitings);

    if (!B.ElseRegion)
      continue;

    MachineBasicBlock::iterator I = B.MBB->getLastNonDebugInstr();
    if (I != B.MBB->end()) {
      for (auto J = I.getReverse(); J != B.MBB->rend() && J->isTerminator();
           J++) {
        if (J->getDesc().isConditionalBranch()) {
          for (auto &MO : J->operands()) {
            if (MO.isReg() && MO.isUse() && MO.getReg().isVirtual()) {
              LiveInterval &IncomingLI = LIS->getInterval(MO.getReg());
              for (auto &Exiting : Exitings) {
                SlotIndex End = LIS->getMBBEndIdx(Exiting)
                                    .getRegSlot()
                                    .getPrevIndex()
                                    .getRegSlot();
                LIS->extendToIndices(IncomingLI, End);
              }
            }
          }
        }
      }
    }
  }

  MF.dump();
  LIS->dump();

  return true;
}

AddMimicryConstraints::AddMimicryConstraints() : MachineFunctionPass(ID) {
  initializeAddMimicryConstraintsPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AddMimicryConstraints, DEBUG_TYPE,
                      "Add Mimicry Constraints", false, false)
INITIALIZE_PASS_DEPENDENCY(SensitiveRegionAnalysis)
INITIALIZE_PASS_DEPENDENCY(PersistencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(LiveIntervals)
INITIALIZE_PASS_END(AddMimicryConstraints, DEBUG_TYPE,
                    "Add Mimicry Constraints", false, false)

namespace llvm {

FunctionPass *createAddMimicryConstraintsPass() {
  return new AddMimicryConstraints();
}

} // namespace llvm

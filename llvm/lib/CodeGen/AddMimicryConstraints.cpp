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

void AddMimicryConstraints::addConstraints(MachineInstr *MI) {
  auto &ALA = getAnalysis<AMiLinearizationAnalysis>();
  auto *MBB = MI->getParent();

  MachineFunction &MF = *MBB->getParent();
  LLVM_DEBUG(MI->dump());

  // For each activating region that contains this instruction
  for (auto *AR : ALA.RegionMap[MBB]) {
    for (unsigned RegI = 0; RegI < MF.getRegInfo().getNumVirtRegs(); RegI++) {
      Register OtherReg = Register::index2VirtReg(RegI);
      if (LIS->hasInterval(OtherReg)) {
        LiveInterval &OtherI = LIS->getInterval(OtherReg);
        if (LIS->isLiveInToMBB(OtherI, AR->Exit) && LIS->isLiveOutOfMBB(OtherI, AR->Branch)) {
          // Add a constraint for each register that is live on the activating edge,
          // by adding a live segment of MI at the start of the exit.
          // That way, a segment of the persistent instruction is within a segment
          // of each register that lives through the activating region.
          addConstraint(OtherI, LIS->getMBBStartIdx(AR->Exit), MI);
        }
      }
    }
  }
}

void AddMimicryConstraints::addConstraint(LiveInterval &LI, SlotIndex SI, MachineInstr *ConflictingMI) {
  LLVM_DEBUG(errs() << "--- addConstraint ---\n");
  LLVM_DEBUG(LI.dump());
  LLVM_DEBUG(ConflictingMI->dump());
  LLVM_DEBUG(SI.dump());
  // SlotIndex Start =
  //     LIS->getSlotIndexes()->getInstructionIndex(*ConflictingMI).getRegSlot();
  SlotIndex End =
      LIS->getSlotIndexes()->getIndexAfter(*ConflictingMI).getRegSlot();
  SlotIndex Start = End.getPrevIndex().getRegSlot();
  VNInfo *VNI = LI.getVNInfoAt(SI);
  if (!LI.liveAt(Start))
    LI.addSegment(LiveRange::Segment(Start, End, VNI));
  LLVM_DEBUG(LI.dump());
  LLVM_DEBUG(errs() << "-------------\n");
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
  auto &LI = LIS->getInterval(NewReg);
  LLVM_DEBUG(errs() << "Created interval for ghost load\n");
  LLVM_DEBUG(LI.dump());

  addConstraints(&*GhostMI);
}

bool AddMimicryConstraints::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  auto &PA = getAnalysis<PersistencyAnalysisPass>();
  auto &ALA = getAnalysis<AMiLinearizationAnalysis>();
  LIS = getAnalysisIfAvailable<LiveIntervals>();
  assert(LIS && "LIS must be available");

  // TODO: AMiLinearizationAnalysis should detect top level region as an activating region
  // for (auto *MI : PA.getPersistentStores(MRI->getTopLevelRegion()))
    // insertGhostLoad(MI);

  for (auto &Pair : ALA.ActivatingRegions) {
    auto &Region = Pair.getSecond();
    auto PersistentInstrs = PA.getPersistentInstructions(&Region);
    for (auto *MI : PersistentInstrs) {
      addConstraints(MI);
    }
    for (auto *MI : PA.getPersistentStores(&Region))
      insertGhostLoad(MI);    
  }

  LLVM_DEBUG(MF.dump());
  LLVM_DEBUG(LIS->dump());

  return true;
}

AddMimicryConstraints::AddMimicryConstraints() : MachineFunctionPass(ID) {
  initializeAddMimicryConstraintsPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AddMimicryConstraints, DEBUG_TYPE,
                      "Add Mimicry Constraints", false, false)
INITIALIZE_PASS_DEPENDENCY(AMiLinearizationAnalysis)
INITIALIZE_PASS_DEPENDENCY(PersistencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(LiveIntervals)
INITIALIZE_PASS_END(AddMimicryConstraints, DEBUG_TYPE,
                    "Add Mimicry Constraints", false, false)

namespace llvm {

FunctionPass *createAddMimicryConstraintsPass() {
  return new AddMimicryConstraints();
}

} // namespace llvm

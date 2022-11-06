#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "RISCV.h"
#include "RISCVAMiLinearizeRegion.h"
#include "RISCVSubtarget.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>

using namespace llvm;

#define DEBUG_TYPE "ami-linearize"

char AMiLinearizeRegion::ID = 0;

void AMiLinearizeRegion::setPersistent(MachineInstr *I) {
  switch (I->getOpcode()) {
  case RISCV::SLLI:
    // I->setDesc(TII->get(RISCV::PSLLI));
    break;
  case RISCV::LUI:
    I->setDesc(TII->get(RISCV::PLUI));
    break;
  case RISCV::ADD:
    I->setDesc(TII->get(RISCV::PADD));
    break;
  case RISCV::ADDI:
    I->setDesc(TII->get(RISCV::PADDI));
    break;
  }
}

void AMiLinearizeRegion::handlePersistentInstr(MachineInstr *I) {
  errs() << "Handling always-persistent instruction " << *I << "\n";
  auto &RDA = getAnalysis<ReachingDefAnalysis>();

  SmallVector<MachineInstr *> WorkSet;
  WorkSet.push_back(I);

  while (!WorkSet.empty()) {
    auto *I = WorkSet.pop_back_val();
    setPersistent(I);

    for (auto &Op : I->operands()) {
      if (!Op.isReg())
        continue;

      MCRegister Reg = Op.getReg().asMCReg();

      SmallPtrSet<MachineInstr *, 16> Defs;
      RDA.getGlobalReachingDefs(I, Reg, Defs);

      for (auto *DI : Defs) {
        WorkSet.push_back(DI);
      }
    }
  }

  DebugLoc DL;

  if (I->getNumOperands() > 0 && I->getOperand(0).isReg()) {
    // BuildMI(*I->getParent(), I->getIterator(), DL, TII->get(RISCV::GLW),
    // I->getOperand(0).getReg());
  }
}

void AMiLinearizeRegion::handleRegion(MachineRegion *Region) {
  errs() << "Handling region " << *Region << "\n";

  for (MachineBasicBlock *Pred : Region->getEntry()->predecessors()) {
    MachineInstr &BranchI = Pred->instr_back();
    if (BranchI.getOpcode() == RISCV::BEQ) {
      BranchI.setDesc(TII->get(RISCV::ABEQ));
    }
  }

  SmallVector<MachineInstr *> AlwaysPersistent;

  for (auto &MRNode : Region->elements()) {
    // Skip nested activating regions, as we can assume they run in constant
    // time regardless of mimicry mode
    if (MRNode->isSubRegion() &&
        ActivatingRegions.contains(MRNode->getNodeAs<MachineRegion>()))
      continue;

    for (MachineInstr &I : *MRNode->getEntry()) {
      if (I.getOpcode() == RISCV::SW) {
        AlwaysPersistent.push_back(&I);
      }
    }
  }

  for (auto *PI : AlwaysPersistent) {
    handlePersistentInstr(PI);
  }
}

bool AMiLinearizeRegion::runOnMachineFunction(MachineFunction &MF) {
  errs() << "AMi Linearize Region Pass\n";
  // auto &RDA = getAnalysis<ReachingDefAnalysis>();
  auto &MRI = getAnalysis<MachineRegionInfoPass>().getRegionInfo();

  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  // MRI.getTopLevelRegion()->dump();

  // Temporary implementation
  SmallPtrSet<MachineRegion *, 16> ActivatingRegions;

  for (auto &MRNode : MRI.getTopLevelRegion()->elements()) {
    if (MRNode->isSubRegion()) {
      for (auto &MRNode : MRNode->getNodeAs<MachineRegion>()->elements()) {
        if (MRNode->isSubRegion()) {
          MachineRegion *MR = MRNode->getNodeAs<MachineRegion>();
          ActivatingRegions.insert(MR);
        }
      }
    }
  }

  SmallVector<MachineRegion *> ToTransform;
  SmallVector<MachineRegion *> WorkList;

  auto *TopRegion = MRI.getTopLevelRegion();

  WorkList.push_back(TopRegion);
  if (ActivatingRegions.contains(TopRegion))
    ToTransform.push_back(TopRegion);

  // Push all activating regions such that a child region
  // we always be processed before it's parent (back of the vector)
  while (!WorkList.empty()) {
    MachineRegion *Region = WorkList.pop_back_val();

    for (auto &MRNode : Region->elements()) {
      if (MRNode->isSubRegion()) {
        auto *ChildRegion = MRNode->getNodeAs<MachineRegion>();
        WorkList.push_back(ChildRegion);

        if (ActivatingRegions.contains(ChildRegion))
          ToTransform.push_back(ChildRegion);
      }
    }
  }

  // Lowest children are further in the vector, so pop them first
  while (!ToTransform.empty()) {
    MachineRegion *Region = ToTransform.pop_back_val();
    handleRegion(Region);
  }

  for (MachineBasicBlock &MB : MF) {
    // errs() << "Entering Machine Block: " << MB.getName() << "\n";

    for (MachineInstr &MI : MB) {
      // errs() << "Entering Machine Instruction: " << MI << "\n";

      /*
      if (MI.getOpcode() == RISCV::ADDI) {
        MI.setDesc(TII->get(RISCV::PADDI));
      }
      */

      // errs() << "\n";
    }
  }

  errs() << "\nResult: -------------- \n";

  for (MachineBasicBlock &MB : MF) {
    // errs() << MB.getName();
    errs() << MB;
  }

  errs() << "\n --------------------- \n";

  return true;
}

AMiLinearizeRegion::AMiLinearizeRegion() : MachineFunctionPass(ID) {
  initializeAMiLinearizeRegionPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AMiLinearizeRegion, DEBUG_TYPE, "AMi Linearize Region",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(ReachingDefAnalysis)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_END(AMiLinearizeRegion, DEBUG_TYPE, "AMi Linearize Region",
                    false, false)

namespace llvm {

FunctionPass *createAMiLinearizeRegionPass() {
  return new AMiLinearizeRegion();
}

} // namespace llvm

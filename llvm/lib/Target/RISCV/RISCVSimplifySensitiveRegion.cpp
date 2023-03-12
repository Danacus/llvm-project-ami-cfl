#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "RISCV.h"
#include "RISCVSimplifySensitiveRegion.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/MachineSSAUpdater.h"
#include "llvm/CodeGen/PHIEliminationUtils.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/CodeGen/TargetOpcodes.h"
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
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>

using namespace llvm;

#define DEBUG_TYPE "riscv-simplify-sensitive-region"

char RISCVSimplifySensitiveRegion::ID = 0;

MachineBasicBlock *RISCVSimplifySensitiveRegion::createExitingBlock(MachineFunction &MF,
                                                         MachineRegion *MR) {
  // Find the exiting blocks of the if region
  SmallVector<MachineBasicBlock *> Exitings;
  MR->getExitingBlocks(Exitings);

  if (Exitings.size() <= 1)
    return nullptr;

  DebugLoc DL;

  MachineFunction::iterator InsertPoint = MF.end();
  int MaxNumber = 0;

  for (auto *Exiting : Exitings) {
    if (Exiting->getNumber() >= MaxNumber) {
      MaxNumber = Exiting->getNumber();
      InsertPoint = std::next(Exiting->getIterator());
    }
  }

  MachineBasicBlock *EndBlock = MF.CreateMachineBasicBlock();

  LLVM_DEBUG(MF.dump());

  auto *OldExit = MR->getExit();

  for (auto *Exiting : Exitings) {
    LLVM_DEBUG(errs() << "Exiting\n");
    LLVM_DEBUG(Exiting->dump());
    MachineBasicBlock *ETBB;
    MachineBasicBlock *EFBB;
    SmallVector<MachineOperand> ECond;
    TII->analyzeBranch(*Exiting, ETBB, EFBB, ECond);

    MachineBasicBlock *FallThrough = Exiting->getFallThrough(true);

    if (!ETBB)
      ETBB = FallThrough;
    else if (!EFBB && Exiting->getFallThrough())
      EFBB = FallThrough;

    assert(ETBB == MR->getExit() ||
           EFBB == MR->getExit() &&
               "AMi error: exiting block of activating region must jump to "
               "region exit");

    DebugLoc DL;

    if (!TII->removeBranch(*Exiting)) {
      assert(Exiting->getFallThrough() &&
             "AMi error: branchless exiting block needs to have a "
             "fallthrough");
    }

    FallThrough = Exiting->getFallThrough(true);

    if (Exiting->isSuccessor(ETBB))
      Exiting->removeSuccessor(ETBB);

    if (EFBB && Exiting->isSuccessor(EFBB))
      Exiting->removeSuccessor(EFBB);

    MachineBasicBlock *FTarget = nullptr;
    Exiting->addSuccessor(EndBlock);

    if (EFBB == nullptr) {
      // Unconditional branch
      if (FallThrough != EndBlock)
        TII->insertUnconditionalBranch(*Exiting, EndBlock, DL);
    } else {
      // Conditional branch
      if (ETBB == MR->getExit()) {
        if (FallThrough != EFBB)
          FTarget = EFBB;
        TII->insertBranch(*Exiting, EndBlock, FTarget, ECond, DL);
        Exiting->addSuccessor(EFBB);
      } else if (EFBB == MR->getExit()) {
        if (FallThrough != EndBlock)
          FTarget = EndBlock;
        TII->insertBranch(*Exiting, ETBB, FTarget, ECond, DL);
        Exiting->addSuccessor(ETBB);
      }
    }
  }

  MF.insert(InsertPoint, EndBlock);

  EndBlock->addSuccessor(MR->getExit());

  if (EndBlock->getFallThrough(true) != MR->getExit())
    TII->insertUnconditionalBranch(*EndBlock, MR->getExit(), DL);

  MDT->addNewBlock(EndBlock, MR->getEntry());
  MPDT->getBase().addNewBlock(EndBlock, OldExit);
  MDF->addBasicBlock(EndBlock, {OldExit});

  if (!MR->isTopLevelRegion() && MR->getParent()) {
    MRI->setRegionFor(EndBlock, MR);
    MRI->updateStatistics(MR);
  }

  ActivatingRegions.insert(MR);
  LLVM_DEBUG(MF.dump());
  return EndBlock;
}

void RISCVSimplifySensitiveRegion::updatePHIs(MachineFunction &MF, MachineBasicBlock *Exiting) {
  MachineBasicBlock *Exit = Exiting->getSingleSuccessor();

  SmallPtrSet<MachineInstr *, 8> ToRemove;

  for (MachineBasicBlock::iterator I = Exit->begin();
       I != Exit->getFirstNonPHI(); ++I) {
    SmallVector<MachineOperand> MovedOps;
    SmallVector<uint> OpsToRemove;
    uint Counter = 1;
    for (MachineInstr::mop_iterator J = std::next(I->operands_begin());
         J != I->operands_end(); J += 2) {
      MachineBasicBlock *MBB = std::next(J)->getMBB();

      if (MBB->isSuccessor(Exiting)) {
        MovedOps.push_back(*J);
        MovedOps.push_back(*std::next(J));
        OpsToRemove.push_back(Counter);
        OpsToRemove.push_back(Counter + 1);
      }

      Counter += 2;
    }

    for (auto Idx = OpsToRemove.rbegin(); Idx != OpsToRemove.rend(); ++Idx) {
      I->removeOperand(*Idx);
    }

    if (MovedOps.size() > 0) {
      Register NewReg = MF.getRegInfo().createVirtualRegister(&RISCV::GPRRegClass);
      auto NewPHI = BuildMI(*Exiting, Exiting->getFirstNonPHI(), DebugLoc(), TII->get(TargetOpcode::PHI), NewReg);

      for (auto Op : MovedOps)
        NewPHI.add(Op);

      I->addOperand(MachineOperand::CreateReg(NewReg, false));
      I->addOperand(MachineOperand::CreateMBB(Exiting));
    }
    
    if (I->getNumOperands() == 1)
      ToRemove.insert(&*I);
  }

  for (auto *MI : ToRemove) {
    MI->eraseFromParent();
  }
}

void RISCVSimplifySensitiveRegion::createExitingBlocks(MachineFunction &MF) {
  for (auto &Branch : ActivatingBranches) {
    for (auto *Region : Branch->Regions) {
      auto *NewExiting = createExitingBlock(MF, Region);
      if (NewExiting) {
        SRA->insertBranchInBlockMap(NewExiting, *Branch);
        updatePHIs(MF, NewExiting);
      }
    }
  }
}

bool RISCVSimplifySensitiveRegion::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(errs() << "RISCV Simplify Sensitive Regions\n");

  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  LLVM_DEBUG(MF.dump());

  MDT = getAnalysisIfAvailable<MachineDominatorTree>();
  MPDT = getAnalysisIfAvailable<MachinePostDominatorTree>();
  MDF = getAnalysisIfAvailable<MachineDominanceFrontier>();
  SRA = &getAnalysis<SensitiveRegionAnalysis>();
  MRI = SRA->getRegionInfo();

  ActivatingBranches.clear();

  for (auto &B : SRA->sensitive_branches()) {
    ActivatingBranches.push_back(&B);
  }

  std::sort(ActivatingBranches.begin(), ActivatingBranches.end());

  createExitingBlocks(MF);

  LLVM_DEBUG(MF.dump());
  return true;
}

RISCVSimplifySensitiveRegion::RISCVSimplifySensitiveRegion() : MachineFunctionPass(ID) {
  initializeRISCVSimplifySensitiveRegionPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(RISCVSimplifySensitiveRegion, DEBUG_TYPE, "RISCV Simplify Sensitive Region",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(SensitiveRegionAnalysis)
INITIALIZE_PASS_END(RISCVSimplifySensitiveRegion, DEBUG_TYPE, "RISCV Simplify Sensitive Region",
                    false, false)

namespace llvm {

FunctionPass *createRISCVSimplifySensitiveRegionPass() {
  return new RISCVSimplifySensitiveRegion();
}

} // namespace llvm

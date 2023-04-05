#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "RISCV.h"
#include "RISCVLinearizeBranch.h"
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

#define DEBUG_TYPE "riscv-linearize-branch"

char RISCVLinearizeBranch::ID = 0;

MachineBasicBlock *RISCVLinearizeBranch::createFlowBlock(MachineFunction &MF,
                                                         MachineRegion *MR,
                                                         bool ReplaceExit) {
  // Find the exiting blocks of the if region
  SmallVector<MachineBasicBlock *> Exitings;
  MR->getExitingBlocks(Exitings);

  DebugLoc DL;

  MachineFunction::iterator InsertPoint = MF.end();
  int MaxNumber = 0;

  for (auto *Exiting : Exitings) {
    LLVM_DEBUG(Exiting->dump());
    LLVM_DEBUG(errs() << "Number: " << Exiting->getNumber() << "\n");
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

  if (ReplaceExit)
    MR->replaceExitRecursive(EndBlock);
  if (!MR->isTopLevelRegion() && MR->getParent()) {
    MRI->setRegionFor(EndBlock, MR->getParent());
    MRI->updateStatistics(MR->getParent());
  }

  ActivatingRegions.insert(MR);
  LLVM_DEBUG(MF.dump());
  return EndBlock;
}

void RISCVLinearizeBranch::createFlowBlocks(MachineFunction &MF) {
  for (auto &Branch : ActivatingBranches) {
    if (Branch->elseRegion()) {
      createFlowBlock(MF, Branch->ifRegion(), true);
    }
  }
}

void RISCVLinearizeBranch::linearizeBranches(MachineFunction &MF) {
  LLVM_DEBUG(MF.dump());
  SmallPtrSet<MachineBasicBlock *, 8> ToActivate;

  for (auto *B : ActivatingBranches) {
    auto &Branch = *B;
    ToActivate.insert(Branch.MBB);

    auto *OldBranchExit = Branch.ifRegion()->getExit();
    if (Branch.elseRegion())
      OldBranchExit = OldBranchExit->getSingleSuccessor();
    LLVM_DEBUG(errs() << "Old exit:\n");
    LLVM_DEBUG(OldBranchExit->dump());

    DebugLoc DL;

    auto *BranchBlock = Branch.MBB;

    SmallVector<MachineOperand> NewCond;

    for (auto OP : Branch.Cond) {
      if (OP.isReg() && !OP.isDef())
        OP.setIsKill(false);
      NewCond.push_back(OP);
    }

    SmallVector<MachineOperand> CondReversed = NewCond;
    TII->reverseBranchCondition(CondReversed);

    TII->removeBranch(*BranchBlock);
    for (auto *Succ : BranchBlock->successors())
      if (Succ != Branch.ifRegion()->getEntry())
        BranchBlock->removeSuccessor(Succ);

    MachineBasicBlock *Entry = Branch.ifRegion()->getEntry();
    MachineBasicBlock *Target = nullptr;
    if (Entry != BranchBlock->getFallThrough(true))
      Target = Entry;
    TII->insertBranch(*BranchBlock, Branch.ifRegion()->getExit(), Target, NewCond,
                      DL);
    BranchBlock->addSuccessor(Branch.ifRegion()->getExit());

    if (Branch.elseRegion()) {
      LLVM_DEBUG(Branch.elseRegion()->dump());
      LLVM_DEBUG(Branch.elseRegion()->getExit()->dump());

      for (auto OP : Branch.Cond) {
        if (OP.isReg() && OP.isUse() && OP.getReg().isPhysical()) {
          Branch.ifRegion()->getExit()->addLiveIn(OP.getReg().asMCReg());
        }
      }

      Branch.ifRegion()->getExit()->removeSuccessor(OldBranchExit);
      TII->removeBranch(*Branch.ifRegion()->getExit());

      MachineBasicBlock *Entry = Branch.elseRegion()->getEntry();
      Branch.ifRegion()->getExit()->addSuccessor(Entry);

      MachineBasicBlock *Target = nullptr;
      if (Entry != Branch.ifRegion()->getExit()->getFallThrough(true))
        Target = Entry;
      TII->insertBranch(*Branch.ifRegion()->getExit(),
                        OldBranchExit, Target, CondReversed, DL);
      Branch.ifRegion()->getExit()->addSuccessor(OldBranchExit);
      ToActivate.insert(Branch.ifRegion()->getExit());
    }

    Branch.FlowBlock = Branch.ifRegion()->getExit();
  }

  LLVM_DEBUG(MF.dump());
}

bool RISCVLinearizeBranch::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(errs() << "AMi Linearize Branch Pass\n");

  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  MDT = getAnalysisIfAvailable<MachineDominatorTree>();
  MPDT = getAnalysisIfAvailable<MachinePostDominatorTree>();
  MDF = getAnalysisIfAvailable<MachineDominanceFrontier>();
  SRA = &getAnalysis<SensitiveRegionAnalysis>();
  MRI = SRA->getRegionInfo();

  ActivatingRegions.clear();
  ActivatingBranches.clear();

  for (auto &B : SRA->sensitive_branches()) {
    ActivatingBranches.push_back(&B);
  }

  std::sort(ActivatingBranches.begin(), ActivatingBranches.end(),
            [](const SensitiveBranch *Lhs, const SensitiveBranch *Rhs) -> bool {
              return *Lhs > *Rhs;
            });

  createFlowBlocks(MF);
  linearizeBranches(MF);

  LLVM_DEBUG(MF.dump());
  return true;
}

RISCVLinearizeBranch::RISCVLinearizeBranch() : MachineFunctionPass(ID) {
  initializeRISCVLinearizeBranchPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(RISCVLinearizeBranch, DEBUG_TYPE, "RISCV Linearize Branch",
                      false, false)
// INITIALIZE_PASS_DEPENDENCY(SensitiveRegionAnalysis)
INITIALIZE_PASS_END(RISCVLinearizeBranch, DEBUG_TYPE, "RISCV Linearize Branch",
                    false, false)

namespace llvm {

FunctionPass *createRISCVLinearizeBranchPass() {
  return new RISCVLinearizeBranch();
}

} // namespace llvm

#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "RISCV.h"
#include "RISCVAMiLinearizeBranch.h"
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

#define DEBUG_TYPE "ami-linearize-branch"

char AMiLinearizeBranch::ID = 0;

template <RISCV::AMi::Qualifier Q>
void AMiLinearizeBranch::setQualifier(MachineInstr *I) {
  if (RISCV::AMi::hasQualifier<Q>(I->getOpcode()))
    return;

  auto PersistentInstr = RISCV::AMi::getQualified<Q>(I->getOpcode());

  if (PersistentInstr != -1) {
    I->setDesc(TII->get(PersistentInstr));
  } else {
    llvm_unreachable("AMi error: unsupported instruction cannot be qualified!");
  }
}

bool AMiLinearizeBranch::setBranchActivating(MachineBasicBlock &MBB) {
  // If the block has no terminators, it just falls into the block after it.
  MachineBasicBlock::iterator I = MBB.getLastNonDebugInstr();
  if (I == MBB.end() || !TII->isUnpredicatedTerminator(*I))
    return false;

  // Count the number of terminators and find the first unconditional or
  // indirect branch.
  MachineBasicBlock::iterator FirstUncondOrIndirectBr = MBB.end();
  int NumTerminators = 0;
  for (auto J = I.getReverse();
       J != MBB.rend() && TII->isUnpredicatedTerminator(*J); J++) {
    NumTerminators++;
    if (J->getDesc().isUnconditionalBranch() ||
        J->getDesc().isIndirectBranch()) {
      FirstUncondOrIndirectBr = J.getReverse();
    }
  }

  // We can't handle blocks that end in an indirect branch.
  if (I->getDesc().isIndirectBranch())
    return true;

  // We can't handle blocks with more than 2 terminators.
  if (NumTerminators > 2)
    return true;

  // Handle a single unconditional branch.
  if (NumTerminators == 1 && I->getDesc().isUnconditionalBranch()) {
    setQualifier<llvm::RISCV::AMi::Activating>(&*I);
    return false;
  }

  // Handle a single conditional branch.
  if (NumTerminators == 1 && I->getDesc().isConditionalBranch()) {
    setQualifier<llvm::RISCV::AMi::Activating>(&*I);
    return false;
  }

  // Handle a conditional branch followed by an unconditional branch.
  if (NumTerminators == 2 && std::prev(I)->getDesc().isConditionalBranch() &&
      I->getDesc().isUnconditionalBranch()) {
    setQualifier<llvm::RISCV::AMi::Activating>(&*I);
    setQualifier<llvm::RISCV::AMi::Activating>(&*std::prev(I));
    return false;
  }

  // Otherwise, we can't handle this.
  return true;
}

MachineBasicBlock *AMiLinearizeBranch::simplifyRegion(MachineFunction &MF,
                                                      MachineRegion *MR) {
  auto &MRI = getAnalysis<MachineRegionInfoPass>().getRegionInfo();
  // Find the exiting blocks of the if region
  SmallVector<MachineBasicBlock *> Exitings;
  MR->getExitingBlocks(Exitings);

  DebugLoc DL;

  MachineBasicBlock *EndBlock = MF.CreateMachineBasicBlock();
  MF.insert(MF.end(), EndBlock);

  auto *OldExit = MR->getExit();

  for (auto *Exiting : Exitings) {
    errs() << "Exiting\n";
    Exiting->dump();
    MachineBasicBlock *ETBB;
    MachineBasicBlock *EFBB;
    SmallVector<MachineOperand> ECond;
    TII->analyzeBranch(*Exiting, ETBB, EFBB, ECond);

    if (!ETBB)
      ETBB = Exiting->getFallThrough();
    else if (!EFBB && Exiting->getFallThrough())
      EFBB = Exiting->getFallThrough();

    assert(ETBB == MR->getExit() || EFBB == MR->getExit() &&
           "AMi error: exiting block of activating region must jump to "
           "region exit");

    DebugLoc DL;

    if (!TII->removeBranch(*Exiting)) {
      assert(Exiting->getFallThrough() &&
             "AMi error: branchless exiting block needs to have a "
             "fallthrough");
    }

    if (Exiting->isSuccessor(ETBB))
      Exiting->removeSuccessor(ETBB);

    if (EFBB && Exiting->isSuccessor(EFBB))
      Exiting->removeSuccessor(EFBB);

    if (EFBB == nullptr) {
      // Unconditional branch
      TII->insertUnconditionalBranch(*Exiting, EndBlock, DL);
    } else {
      // Conditional branch
      if (ETBB == MR->getExit()) {
        TII->insertBranch(*Exiting, EndBlock, EFBB, ECond, DL);
        Exiting->addSuccessor(EFBB);
      } else if (EFBB == MR->getExit()) {
        TII->insertBranch(*Exiting, ETBB, EndBlock, ECond, DL);
        Exiting->addSuccessor(ETBB);
      }
    }

    Exiting->addSuccessor(EndBlock);
  }

  // BuildMI(*EndBlock, EndBlock->end(), DL,
  // TII->get(TargetOpcode::BRANCH_TARGET))
  //     .addMBB(EndBlock);

  TII->insertUnconditionalBranch(*EndBlock, MR->getExit(), DL);
  EndBlock->addSuccessor(MR->getExit());

  MDT->addNewBlock(EndBlock, MR->getEntry());
  MPDT->getBase().addNewBlock(EndBlock, OldExit);
  MDF->addBasicBlock(EndBlock, { OldExit });

  MR->replaceExitRecursive(EndBlock);
  if (!MR->isTopLevelRegion() && MR->getParent()) {
    MRI.setRegionFor(EndBlock, MR->getParent());
    MRI.updateStatistics(MR->getParent());
  }

  ActivatingRegions.insert(MR);
  return EndBlock;
}

void AMiLinearizeBranch::simplifyBranchRegions(MachineFunction &MF) {
  for (auto &Branch : ActivatingBranches) {
    simplifyRegion(MF, Branch.IfRegion);

    if (Branch.ElseRegion) {
      simplifyRegion(MF, Branch.ElseRegion);
    }
  }
}

void AMiLinearizeBranch::linearizeBranches(MachineFunction &MF) {
  SmallPtrSet<MachineBasicBlock *, 8> ToActivate;

  for (auto &Branch : ActivatingBranches) {
    ToActivate.insert(Branch.MBB);

    auto *OldBranchExit = Branch.IfRegion->getExit()->getSingleSuccessor();

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
      if (Succ != Branch.IfRegion->getEntry())
        BranchBlock->removeSuccessor(Succ);
    TII->insertBranch(*BranchBlock, Branch.IfRegion->getExit(),
                      Branch.IfRegion->getEntry(), NewCond, DL);

    if (Branch.ElseRegion) {
      Branch.ElseRegion->dump();
      assert(Branch.ElseRegion->getExit()->getSingleSuccessor() ==
                 OldBranchExit &&
             "if and else should exit to the same block");

      for (auto OP : Branch.Cond) {
        if (OP.isReg() && OP.isUse()) {
          Branch.IfRegion->getExit()->addLiveIn(OP.getReg().asMCReg());
        }
      }

      Branch.IfRegion->getExit()->removeSuccessor(OldBranchExit);
      TII->removeBranch(*Branch.IfRegion->getExit());

      TII->insertBranch(*Branch.IfRegion->getExit(),
                        Branch.ElseRegion->getExit(),
                        Branch.ElseRegion->getEntry(), CondReversed, DL);
      Branch.IfRegion->getExit()->addSuccessor(Branch.ElseRegion->getEntry());
      ToActivate.insert(Branch.IfRegion->getExit());
    }
  }

  for (auto *MBB : ToActivate) {
    setBranchActivating(*MBB);
  }
}

void AMiLinearizeBranch::removePseudos(MachineFunction &MF) {
  // SmallPtrSet<MachineInstr *, 8> ToRemove;

  // for (MachineBasicBlock &MB : MF) {
  //   for (MachineInstr &MI : MB) {
  //     if (MI.getOpcode() == TargetOpcode::SECRET)
  //       ToRemove.insert(&MI);
  //   }
  // }

  // for (auto *MI : ToRemove) {
  //   MI->eraseFromParent();
  // }
}

bool AMiLinearizeBranch::runOnMachineFunction(MachineFunction &MF) {
  errs() << "AMi Linearize Branch Pass\n";

  // auto &MRI = getAnalysis<MachineRegionInfoPass>().getRegionInfo();

  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  removePseudos(MF);
  // MRI.dump();
  MF.dump();

  // findActivatingBranches();
  MRI = &getAnalysisIfAvailable<MachineRegionInfoPass>()->getRegionInfo();
  MDT = getAnalysisIfAvailable<MachineDominatorTree>();
  MPDT = getAnalysisIfAvailable<MachinePostDominatorTree>();
  MDF = getAnalysisIfAvailable<MachineDominanceFrontier>();
  SRA = &getAnalysis<SensitiveRegionAnalysis>();
  ActivatingBranches = SmallVector<SensitiveBranch>(SRA->sensitive_branches());

  std::sort(ActivatingBranches.begin(), ActivatingBranches.end(), std::greater<SensitiveBranch>());
  // std::sort(ActivatingBranches.begin(), ActivatingBranches.end());

  for (auto &B : ActivatingBranches) {
    errs() << "Activating branch: " << B.MBB->getFullName();
    errs() << "if region:\n";
    B.IfRegion->dump();

    if (B.ElseRegion) {
      errs() << "else region:\n";
      B.ElseRegion->dump();
    }
  }

  simplifyBranchRegions(MF);
  linearizeBranches(MF);

  MF.dump();
  return true;
}

AMiLinearizeBranch::AMiLinearizeBranch() : MachineFunctionPass(ID) {
  initializeAMiLinearizeBranchPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AMiLinearizeBranch, DEBUG_TYPE, "AMi Linearize Branch",
                      false, false)
// INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
// INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysisVirtReg)
INITIALIZE_PASS_DEPENDENCY(SensitiveRegionAnalysis)
INITIALIZE_PASS_END(AMiLinearizeBranch, DEBUG_TYPE, "AMi Linearize Branch",
                    false, false)

namespace llvm {

FunctionPass *createAMiLinearizeBranchPass() {
  return new AMiLinearizeBranch();
}

} // namespace llvm

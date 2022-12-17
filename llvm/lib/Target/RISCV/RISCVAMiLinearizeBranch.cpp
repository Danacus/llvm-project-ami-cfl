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
  // Find the exiting blocks of the if region
  SmallVector<MachineBasicBlock *> Exitings;
  MR->getExitingBlocks(Exitings);

  DebugLoc DL;

  MachineBasicBlock *EndBlock = MF.CreateMachineBasicBlock();

  for (auto *Exiting : Exitings) {
    errs() << "Exiting\n";
    Exiting->dump();
    MachineBasicBlock *ETBB;
    MachineBasicBlock *EFBB;
    SmallVector<MachineOperand> ECond;
    TII->analyzeBranch(*Exiting, ETBB, EFBB, ECond);

    if (!ETBB)
      ETBB = Exiting->getFallThrough();

    assert(ETBB == MR->getExit() &&
           "AMi error: exiting block of activating region must jump to "
           "region exit");

    DebugLoc DL;

    if (!TII->removeBranch(*Exiting)) {
      assert(Exiting->getFallThrough() &&
             "AMi error: branchless exiting block needs to have a "
             "fallthrough");
    }

    Exiting->removeSuccessor(ETBB);

    MachineBasicBlock *Target = EndBlock;

    if (EFBB == nullptr) {
      // Unconditional branch
      TII->insertUnconditionalBranch(*Exiting, Target, DL);
    } else {
      // Conditional branch
      if (ETBB == MR->getExit()) {
        TII->insertBranch(*Exiting, Target, EFBB, ECond, DL);
      } else if (EFBB == MR->getExit()) {
        TII->insertBranch(*Exiting, ETBB, Target, ECond, DL);
      }
    }

    Exiting->addSuccessor(EndBlock);
  }

  BuildMI(*EndBlock, EndBlock->end(), DL, TII->get(TargetOpcode::AMI_BR_TARGET))
      .addMBB(EndBlock);

  auto *OldExit = MR->getExit();
  TII->insertUnconditionalBranch(*EndBlock, MR->getExit(), DL);
  EndBlock->addSuccessor(MR->getExit());
  MR->replaceExitRecursive(EndBlock);
  MF.insert(std::prev(OldExit->getIterator()), EndBlock);

  rewritePHIForRegion(MF, MR);

  ActivatingRegions.insert(MR);
  return EndBlock;
}

void AMiLinearizeBranch::rewritePHIForRegion(MachineFunction &MF,
                                             MachineRegion *MR) {
  MachineBasicBlock *EndBlock = MR->getExit();
  SmallVector<MachineInstr *> PHIToRemove;

  for (auto &MI : *EndBlock->getSingleSuccessor()) {
    if (!MI.isPHI())
      break;

    bool CreatedPHIDef = false;
    auto NewPHI = BuildMI(*EndBlock, EndBlock->begin(), DebugLoc(),
                          TII->get(TargetOpcode::PHI));

    SmallVector<unsigned> ToRemove;
    unsigned NumSources = 0;

    for (unsigned I = 1, E = MI.getNumOperands(); I != E; I += 2) {
      MachineBasicBlock *MBB = MI.getOperand(I + 1).getMBB();

      if (MR->contains(MBB)) {
        if (!CreatedPHIDef) {
          NewPHI.addDef(MF.getRegInfo().createVirtualRegister(
              MF.getRegInfo().getRegClass(MI.getOperand(0).getReg())));
          CreatedPHIDef = true;
        }
        NewPHI.add(MI.getOperand(I));
        NewPHI.add(MI.getOperand(I + 1));
        ToRemove.push_back(I);
        ToRemove.push_back(I + 1);
        NumSources += 1;
      }
    }

    Register SrcReg;
    if (NumSources == 1) {
      SrcReg = NewPHI->getOperand(1).getReg();
    } else {
      SrcReg = NewPHI->getOperand(0).getReg();
    }

    while (!ToRemove.empty()) {
      MI.removeOperand(ToRemove.pop_back_val());
    }

    MI.addOperand(MachineOperand::CreateReg(SrcReg, false));
    MI.addOperand(MachineOperand::CreateMBB(EndBlock));

    if (NumSources == 1) {
      NewPHI->eraseFromParent();
    }

    if (MI.getNumOperands() == 1)
      PHIToRemove.push_back(&MI);
  }

  while (!PHIToRemove.empty())
    PHIToRemove.pop_back_val()->eraseFromParent();
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
    ToActivate.insert(Branch.MI->getParent());

    auto *OldBranchExit = Branch.IfRegion->getExit()->getSingleSuccessor();

    DebugLoc DL;

    auto *BranchBlock = Branch.MI->getParent();

    SmallVector<MachineOperand> NewCond;

    for (auto OP : Branch.Cond) {
      if (OP.isReg() && !OP.isDef())
        OP.setIsKill(false);
      NewCond.push_back(OP);
    }

    SmallVector<MachineOperand> CondReversed = NewCond;
    TII->reverseBranchCondition(CondReversed);

    TII->removeBranch(*BranchBlock);
    BranchBlock->removeSuccessor(Branch.ElseRegion->getEntry());
    TII->insertBranch(*BranchBlock, Branch.IfRegion->getExit(),
                      Branch.IfRegion->getEntry(), NewCond, DL);

    if (Branch.ElseRegion) {
      assert(Branch.ElseRegion->getExit()->getSingleSuccessor() ==
                 OldBranchExit &&
             "if and else should exit to the same block");

      Branch.IfRegion->getExit()->removeSuccessor(OldBranchExit);
      TII->removeBranch(*Branch.IfRegion->getExit());

      TII->insertBranch(*Branch.IfRegion->getExit(),
                        Branch.ElseRegion->getExit(),
                        Branch.ElseRegion->getEntry(), CondReversed, DL);
      Branch.IfRegion->getExit()->addSuccessor(Branch.ElseRegion->getEntry());
      ToActivate.insert(Branch.IfRegion->getExit());
    }

    eliminatePHI(MF, Branch, *OldBranchExit);
  }

  for (auto *MBB : ToActivate) {
    setBranchActivating(*MBB);
  }
}

void AMiLinearizeBranch::eliminatePHI(MachineFunction &MF,
                                      ActivatingBranch &Branch,
                                      MachineBasicBlock &Exit) {
  errs() << "here\n";

  SmallVector<MachineInstr *> PHIToRemove;

  // PHI elimination
  for (auto &MI : Exit) {
    if (!MI.isPHI())
      break;

    SmallVector<unsigned> ToRemove;

    errs() << "eliminating PHI\n";
    MI.dump();

    for (unsigned I = 1, E = MI.getNumOperands(); I != E; I += 2) {
      Register Reg = MI.getOperand(I).getReg();
      unsigned SubReg = MI.getOperand(I).getSubReg();
      MachineBasicBlock *MBB = MI.getOperand(I + 1).getMBB();

      if (Branch.IfRegion->getExit() == MBB ||
          Branch.ElseRegion->getExit() == MBB ||
          Branch.IfRegion->contains(MBB) || Branch.ElseRegion->contains(MBB)) {
        TII->createPHISourceCopy(
            *MBB, findPHICopyInsertPoint(MBB, &Exit, Reg), DebugLoc(),
            Reg, SubReg, MI.getOperand(0).getReg());
        ToRemove.push_back(I);
        ToRemove.push_back(I + 1);
      }
    }

    while (!ToRemove.empty()) {
      MI.removeOperand(ToRemove.pop_back_val());
    }

    if (MI.getNumOperands() == 1)
      PHIToRemove.push_back(&MI);
  }

  while (!PHIToRemove.empty())
    PHIToRemove.pop_back_val()->eraseFromParent();
}

void AMiLinearizeBranch::findActivatingBranches() {
  auto &MRI = getAnalysis<MachineRegionInfoPass>().getRegionInfo();
  auto &Secrets = getAnalysis<TrackSecretsAnalysisVirtReg>().TSA.SecretUses;

  for (auto &Secret : Secrets) {
    if (!(Secret.second.getSecretMask() & 1u))
      continue;

    auto *User = Secret.second.getUser();

    // We still need those registers
    for (auto &MO : User->uses()) {
      if (MO.isReg())
        MO.setIsKill(false);
    }

    if (User->isConditionalBranch()) {
      if (RISCV::AMi::hasQualifier<llvm::RISCV::AMi::Activating>(
              User->getOpcode())) {
        // Already handled this branch
        continue;
      }

      MachineBasicBlock *TBB;
      MachineBasicBlock *FBB;
      SmallVector<MachineOperand> Cond;

      if (TII->analyzeBranch(*User->getParent(), TBB, FBB, Cond))
        llvm_unreachable(
            "AMi error: failed to analyze secret-dependent branch");

      // When there is only a single conditional branch as terminator,
      // FBB will not be set. In this case it is probably safe to assume that
      // FBB is the fallthrough block (at least for RISC-V).
      if (!FBB)
        FBB = User->getParent()->getFallThrough();

      // Get largest region that starts at BB. (See
      // RegionInfoBase::getMaxRegionExit)
      MachineRegion *FR = MRI.getRegionFor(FBB);
      if (auto *Expanded = FR->getExpandedRegion()) {
        // I like large regions, expanded sounds good
        FR = Expanded;
      }
      if (FR->getEntry() != FBB || !FR->getExit())
        llvm_unreachable("AMi error: unable to find activating region for "
                         "secret-dependent branch");
      while (FR && FR->getParent() && FR->getParent()->getEntry() == FBB &&
             FR->getExit())
        FR = FR->getParent();

      FR->dump();
      FR->getExit()->dump();

      // Find the exiting blocks of this region
      SmallVector<MachineBasicBlock *> Exitings;
      FR->getExitingBlocks(Exitings);

      bool HasElseRegion = FR->getExit() != TBB;

      MachineRegion *TR = nullptr;
      if (HasElseRegion) {
        TR = MRI.getRegionFor(TBB);
        if (auto *Expanded = TR->getExpandedRegion()) {
          // I like large regions, expanded sounds good
          TR = Expanded;
        }
        if (TR->getEntry() == TBB) {
          while (TR && TR->getParent() && TR->getParent()->getEntry() == TBB)
            TR = TR->getParent();
        } else {
          llvm_unreachable("AMi error: unable to find activating region for "
                           "secret-dependent branch");
        }
      } else {
        assert(FR->getExit() == TBB && "AMi error: if branch without else "
                                       "region must exit to branch target");
      }

      ActivatingBranches.push_back(ActivatingBranch(User, Cond, TR, FR));
    }
  }
}

void AMiLinearizeBranch::removePseudoSecret(MachineFunction &MF) {
  SmallPtrSet<MachineInstr *, 8> ToRemove;

  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      if (MI.getOpcode() == TargetOpcode::SECRET)
        ToRemove.insert(&MI);
    }
  }

  for (auto *MI : ToRemove) {
    MI->eraseFromParent();
  }
}

bool AMiLinearizeBranch::runOnMachineFunction(MachineFunction &MF) {
  errs() << "AMi Linearize Branch Pass\n";

  auto &MRI = getAnalysis<MachineRegionInfoPass>().getRegionInfo();

  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  MRI.dump();

  findActivatingBranches();

  // std::sort(ActivatingBranches.begin(), ActivatingBranches.end(),
  // std::greater<ActivatingBranch>());
  std::sort(ActivatingBranches.begin(), ActivatingBranches.end());

  for (auto &B : ActivatingBranches) {
    errs() << "Activating branch: " << *B.MI;
    errs() << "if region:\n";
    B.IfRegion->dump();

    if (B.ElseRegion) {
      errs() << "else region:\n";
      B.ElseRegion->dump();
    }
  }

  simplifyBranchRegions(MF);
  linearizeBranches(MF);

  removePseudoSecret(MF);

  MF.dump();
  return true;
}

AMiLinearizeBranch::AMiLinearizeBranch() : MachineFunctionPass(ID) {
  initializeAMiLinearizeBranchPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AMiLinearizeBranch, DEBUG_TYPE, "AMi Linearize Region",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysisVirtReg)
INITIALIZE_PASS_END(AMiLinearizeBranch, DEBUG_TYPE, "AMi Linearize Region",
                    false, false)

namespace llvm {

FunctionPass *createAMiLinearizeBranchPass() {
  return new AMiLinearizeBranch();
}

} // namespace llvm

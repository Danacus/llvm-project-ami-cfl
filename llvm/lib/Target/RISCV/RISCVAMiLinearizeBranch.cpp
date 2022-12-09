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

void AMiLinearizeBranch::linearizeBranches(MachineFunction &MF) {
  for (auto Branch : ActivatingBranches) {
    ActivatingRegions.insert(Branch.IfRegion);
    setBranchActivating(*Branch.MI->getParent());

    if (Branch.ElseRegion) {
      // Find the exiting blocks of the if region
      SmallVector<MachineBasicBlock *> Exitings;
      Branch.IfRegion->getExitingBlocks(Exitings);

      SmallVector<MachineOperand> CondReversed = Branch.Cond;
      TII->reverseBranchCondition(CondReversed);

      DebugLoc DL;

      MachineBasicBlock *EndBlock = MF.CreateMachineBasicBlock();
      MF.insert(std::prev(Branch.IfRegion->getExit()->getIterator()), EndBlock);
      TII->insertBranch(*EndBlock, Branch.IfRegion->getExit(),
                        Branch.ElseRegion->getEntry(), CondReversed, DL);
      EndBlock->addSuccessor(Branch.ElseRegion->getEntry());
      EndBlock->addSuccessor(Branch.IfRegion->getExit());
      setBranchActivating(*EndBlock);

      for (auto *Exiting : Exitings) {
        MachineBasicBlock *ETBB;
        MachineBasicBlock *EFBB;
        SmallVector<MachineOperand> ECond;
        TII->analyzeBranch(*Exiting, ETBB, EFBB, ECond);

        if (!ETBB)
          ETBB = Exiting->getFallThrough();

        assert(ETBB == Branch.IfRegion->getExit() &&
               "AMi error: exiting block of activating region must jump to "
               "region exit");

        DebugLoc DL;

        if (!TII->removeBranch(*Exiting)) {
          assert(Exiting->getFallThrough() &&
                 "AMi error: branchless exiting block needs to have a "
                 "fallthrough");
        }

        // Allow succeeding with the "else" branch
        Exiting->removeSuccessor(ETBB);
        Exiting->addSuccessor(EndBlock);

        MachineBasicBlock *Target = EndBlock;

        if (EFBB == nullptr) {
          // Unconditional branch
          // Don't need a branch if the target is the fallthrough
          if (Target != Exiting->getFallThrough()) {
            TII->insertUnconditionalBranch(*Exiting, Target, DL);
          }
        } else {
          // Conditional branch
          if (ETBB == Branch.IfRegion->getExit()) {
            TII->insertBranch(*Exiting, Target, EFBB, ECond, DL);
          } else if (EFBB == Branch.IfRegion->getExit()) {
            TII->insertBranch(*Exiting, ETBB, Target, ECond, DL);
          }
        }
      }
    }
  }
}

void AMiLinearizeBranch::findActivatingBranches() {
  auto &MRI = getAnalysis<MachineRegionInfoPass>().getRegionInfo();
  auto &Secrets = getAnalysis<TrackSecretsAnalysisVirtReg>().TSA.SecretUses;

  for (auto &Secret : Secrets) {
    if (!(Secret.second.getSecretMask() & 1u))
      continue;

    auto *User = Secret.second.getUser();

    if (User->isConditionalBranch()) {
      if (RISCV::AMi::hasQualifier<llvm::RISCV::AMi::Activating>(
              User->getOpcode())) {
        // Already handled this branch
        continue;
      }

      // We still need those registers
      for (auto &MO : User->uses()) {
        if (MO.isReg())
          MO.setIsKill(false);
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

  std::sort(ActivatingBranches.begin(), ActivatingBranches.end(),
            std::greater<ActivatingBranch>());

  for (auto &B : ActivatingBranches) {
    errs() << "Activating branch: " << *B.MI;
    errs() << "if region:\n";
    B.IfRegion->dump();

    if (B.ElseRegion) {
      errs() << "else region:\n";
      B.ElseRegion->dump();
    }
  }

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

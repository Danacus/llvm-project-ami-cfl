#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "RISCV.h"
#include "RISCVAMiLinearizeRegion.h"
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

#define DEBUG_TYPE "ami-linearize"

char AMiLinearizeRegion::ID = 0;

template <RISCV::AMi::Qualifier Q>
void AMiLinearizeRegion::setQualifier(MachineInstr *I) {
  if (RISCV::AMi::hasQualifier<Q>(I->getOpcode()))
    return;

  auto PersistentInstr = RISCV::AMi::getQualified<Q>(I->getOpcode());

  if (PersistentInstr != -1) {
    I->setDesc(TII->get(PersistentInstr));
  } else {
    errs() << "Unsupported instruction: " << *I;
    // llvm_unreachable(
    //"AMi error: unsupported instruction cannot be qualified!");
  }
}

void AMiLinearizeRegion::handlePersistentInstr(MachineInstr *I) {
  errs() << "Handling always-persistent instruction " << *I << "\n";
  auto &RDA = getAnalysis<ReachingDefAnalysis>();

  SmallVector<MachineInstr *> WorkSet;
  WorkSet.push_back(I);

  while (!WorkSet.empty()) {
    auto *I = WorkSet.pop_back_val();
    setQualifier<RISCV::AMi::Persistent>(I);

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

  if (I->getNumOperands() > 2 && I->getOperand(0).isReg()) {
    BuildMI(*I->getParent(), I->getIterator(), DL, TII->get(RISCV::GLW),
            I->getOperand(0).getReg().asMCReg())
        .add(I->getOperand(1))
        .add(I->getOperand(2));
  } else {
    llvm_unreachable(
        "AMi error: unable to nullify unwanted side-effects in mimicry mode!");
  }
}

void AMiLinearizeRegion::handleRegion(MachineRegion *Region) {
  errs() << "Handling region " << *Region << "\n";

  /*
  // Make the predecessing branch instruction activating
  assert(Region->getEntry()->pred_size() <= 1 &&
         "AMi error: activating region cannot have multiple entry points!");
  for (MachineBasicBlock *Pred : Region->getEntry()->predecessors()) {
    MachineInstr &BranchI = Pred->instr_back();
    if (BranchI.isBranch()) {
      setQualifier<RISCV::AMi::Activating>(&BranchI);
    } else {
      llvm_unreachable(
          "AMi error: unsupported branch instruction for activating region!");
    }
  }
  */

  SmallVector<MachineInstr *> AlwaysPersistent;

  for (auto &MRNode : Region->elements()) {
    // Skip nested activating regions, as we can assume they run in constant
    // time regardless of mimicry mode
    if (MRNode->isSubRegion() &&
        ActivatingRegions.contains(MRNode->getNodeAs<MachineRegion>()))
      continue;

    for (MachineInstr &I : *MRNode->getEntry()) {
      if (RISCV::AMi::getClass(I.getOpcode()) == RISCV::AMi::AlwaysPersistent) {
        AlwaysPersistent.push_back(&I);
      }
    }
  }

  for (auto *PI : AlwaysPersistent) {
    handlePersistentInstr(PI);
  }
}

bool AMiLinearizeRegion::setBranchActivating(MachineBasicBlock &MBB) {
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

void AMiLinearizeRegion::findActivatingRegions() {
  auto &MRI = getAnalysis<MachineRegionInfoPass>().getRegionInfo();
  auto &Secrets = getAnalysis<TrackSecretsAnalysis>().SecretUses;

  for (auto &Secret : Secrets) {
    if (!(Secret.second.getSecretMask() & 1u))
      continue;
    // assert((Secret.second & 1u) && "not a secret value or incorrect mask");
    // assert((!Secret.IsDef) && "not a use of a secret value");

    // auto RegDef = Secret.first.get<SecretRegisterDef>();
    auto *User = Secret.second.getUser();

    if (User->isConditionalBranch()) {
      if (RISCV::AMi::hasQualifier<llvm::RISCV::AMi::Activating>(
              User->getOpcode())) {
        // Already handled this branch
        continue;
      }
      errs() << "Conditional branch: " << *User << "\n";

      MachineBasicBlock *TBB;
      MachineBasicBlock *FBB;
      SmallVector<MachineOperand> Cond;
      SmallVector<MachineOperand> CondReversed;

      if (TII->analyzeBranch(*User->getParent(), TBB, FBB, Cond))
        llvm_unreachable(
            "AMi error: failed to analyze secret-dependent branch");

      CondReversed = Cond;
      TII->reverseBranchCondition(CondReversed);

      // When there is only a single conditional branch as terminator,
      // FBB will not be set. In this case it is probably safe to assume that
      // FBB is the fallthrough block (at least for RISC-V).
      if (!FBB)
        FBB = User->getParent()->getFallThrough();

      // Get largest region that starts at BB. (See
      // RegionInfoBase::getMaxRegionExit)
      MachineRegion *FR = MRI.getRegionFor(FBB);
      if (FR->getEntry() != FBB)
        llvm_unreachable("AMi error: unable to find activating region for "
                         "secret-dependent branch");
      while (FR && FR->getParent() && FR->getParent()->getEntry() == FBB)
        FR = FR->getParent();

      // Find the exiting blocks of this region
      SmallVector<MachineBasicBlock *> Exitings;
      FR->getExitingBlocks(Exitings);

      for (auto *Exiting : Exitings) {
        MachineBasicBlock *ETBB;
        MachineBasicBlock *EFBB;
        SmallVector<MachineOperand> ECond;
        TII->analyzeBranch(*Exiting, ETBB, EFBB, ECond);

        auto Last = Exiting->getLastNonDebugInstr(false);
        if (Last != Exiting->end() && Last->isUnconditionalBranch()) {
          DebugLoc DL;
          if (!TII->removeBranch(*Exiting))
            llvm_unreachable("AMi error: failed to remove branch");

          MachineBasicBlock *Target = TBB;

          // Allow succeeding with the "else" branch
          Exiting->addSuccessor(TBB);

          // Create one-way branch if our target is the fallthrough
          if (Target == Exiting->getFallThrough())
            Target = nullptr;

          TII->insertBranch(*Exiting, ETBB, Target, CondReversed, DL);

          // Revert the change, we don't actually want to change the CFG
          Exiting->removeSuccessor(TBB);

          MachineRegion *TR = MRI.getRegionFor(TBB);
          TR->dump();
          if (TR->getEntry() != TBB)
            llvm_unreachable("AMi error: unable to find activating region for "
                             "secret-dependent branch");
          while (TR && TR->getParent() && TR->getParent()->getEntry() == TBB)
            TR = TR->getParent();

          ActivatingRegions.insert(TR);
          setBranchActivating(*Exiting);
        }
      }

      ActivatingRegions.insert(FR);
      setBranchActivating(*User->getParent());
    }
  }
}

bool AMiLinearizeRegion::runOnMachineFunction(MachineFunction &MF) {
  errs() << "AMi Linearize Region Pass\n";

  auto &MRI = getAnalysis<MachineRegionInfoPass>().getRegionInfo();

  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  findActivatingRegions();
  MF.dump();

  auto *TopRegion = MRI.getTopLevelRegion();

  MRI.dump();

  SmallVector<MachineRegion *> ToTransform;
  SmallVector<MachineRegion *> WorkList;

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

  MF.dump();

  return true;
}

AMiLinearizeRegion::AMiLinearizeRegion() : MachineFunctionPass(ID) {
  initializeAMiLinearizeRegionPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AMiLinearizeRegion, DEBUG_TYPE, "AMi Linearize Region",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(ReachingDefAnalysis)
INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysis)
INITIALIZE_PASS_END(AMiLinearizeRegion, DEBUG_TYPE, "AMi Linearize Region",
                    false, false)

namespace llvm {

FunctionPass *createAMiLinearizeRegionPass() {
  return new AMiLinearizeRegion();
}

} // namespace llvm

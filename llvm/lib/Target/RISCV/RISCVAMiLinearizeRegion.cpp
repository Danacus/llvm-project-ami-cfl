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

void AMiLinearizeRegion::findActivatingRegions() {
  auto &MRI = getAnalysis<MachineRegionInfoPass>().getRegionInfo();
  auto &Secrets = getAnalysis<TrackSecretsAnalysis>().Secrets;

  for (auto &Secret : Secrets) {
    assert((Secret.SecretMask & 1u) && "not a secret value or incorrect mask");
    assert((!Secret.IsDef) && "not a use of a secret value");

    if (Secret.MI->isConditionalBranch()) {
      errs() << "Conditional branch: " << *Secret.MI << "\n";
      MachineBasicBlock *EntryMBB = Secret.MI->getParent()->getFallThrough();

      // Get largest region that starts at BB. (See
      // RegionInfoBase::getMaxRegionExit)
      MachineRegion *R = MRI.getRegionFor(EntryMBB);
      if (R->getEntry() != EntryMBB)
        llvm_unreachable("AMi error: unable to find activating region for "
                         "secret-dependent branch");
      while (R && R->getParent() && R->getParent()->getEntry() == EntryMBB)
        R = R->getParent();

      ActivatingRegions.insert(R);
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

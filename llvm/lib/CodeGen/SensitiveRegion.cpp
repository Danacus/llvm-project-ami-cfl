
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/PassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "sensitive-region"

void SensitiveRegionAnalysis::removeBranch(MachineBasicBlock *MBB) {
  auto Key = SensitiveBranch(MBB);
  BranchSet::iterator ToRemove =
      std::find(SensitiveBranches.begin(), SensitiveBranches.end(), Key);
  if (ToRemove != SensitiveBranches.end()) {
    SensitiveBranches.erase(ToRemove);
    for (auto &Pair : IfBranchMap) {
      BranchSet::iterator ToRemove =
          std::find(Pair.getSecond().begin(), Pair.getSecond().end(), Key);
      if (ToRemove != Pair.getSecond().end())
        Pair.getSecond().erase(ToRemove);
    }
    for (auto &Pair : ElseBranchMap) {
      BranchSet::iterator ToRemove =
          std::find(Pair.getSecond().begin(), Pair.getSecond().end(), Key);
      if (ToRemove != Pair.getSecond().end())
        Pair.getSecond().erase(ToRemove);
    }
  }
}

void SensitiveRegionAnalysis::addBranch(SensitiveBranch Branch) {
  SensitiveBranches.push_back(Branch);
  SensitiveRegions.insert(Branch.IfRegion);

  if (Branch.ElseRegion)
    SensitiveRegions.insert(Branch.ElseRegion);

  if (Branch.ElseRegion) {
    for (auto *MBB : Branch.ElseRegion->blocks()) {
      SensitiveBlocks.set(MBB->getNumber());
      ElseBranchMap[MBB].push_back(Branch);
    }
  }

  for (auto *MBB : Branch.IfRegion->blocks()) {
    SensitiveBlocks.set(MBB->getNumber());
    IfBranchMap[MBB].push_back(Branch);
  }
}

void SensitiveRegionAnalysis::handleBranch(MachineBasicBlock *MBB, MachineRegion *Parent) {
  const auto &ST = MBB->getParent()->getSubtarget();
  const auto *TII = ST.getInstrInfo();
  LLVM_DEBUG(MBB->dump());

  removeBranch(MBB);

  MachineBasicBlock *TBB;
  MachineBasicBlock *FBB;
  SmallVector<MachineOperand> Cond;

  if (TII->analyzeBranch(*MBB, TBB, FBB, Cond))
    llvm_unreachable("AMi error: failed to analyze secret-dependent branch");

  // When there is only a single conditional branch as terminator,
  // FBB will not be set. In this case it is probably safe to assume that
  // FBB is the fallthrough block (at least for RISC-V).
  if (!FBB)
    FBB = MBB->getFallThrough();

  if (FBB->pred_size() > 1) {
    // FBB cannot be a valid SESE region entry, if region must be missing
    // Need to re-order the regions
    TII->removeBranch(*MBB);
    TII->reverseBranchCondition(Cond);
    TII->insertBranch(*MBB, FBB, TBB, Cond, DebugLoc());
    auto *Temp = TBB;
    TBB = FBB;
    FBB = Temp;
  }

  MachineBasicBlock *Exit = MPDT->findNearestCommonDominator(TBB, FBB);
  assert(Exit && "Expected a branch exit");
  MachineRegion *TR = nullptr;
  MachineRegion *FR = nullptr;
  
  if (Exit != TBB) {
    TR = new MachineRegion(TBB, Exit, MRI, MDT);
    MRI->setRegionFor(TBB, TR);
    Parent->addSubRegion(TR);
    handleRegion(TR);
  }

  if (Exit != FBB) {
    FR = new MachineRegion(FBB, Exit, MRI, MDT);
    MRI->setRegionFor(FBB, FR);
    Parent->addSubRegion(FR);
    handleRegion(FR);
  }

  addBranch(SensitiveBranch(MBB, Cond, TR, FR));
}

void SensitiveRegionAnalysis::handleRegion(MachineRegion *MR) {
  SparseBitVector<128> HandledBlocks;
  SmallVector<MachineBasicBlock *> WorkList;
  WorkList.push_back(MR->getEntry());
  HandledBlocks.set(MR->getEntry()->getNumber());

  while (!WorkList.empty()) {
    MachineBasicBlock *MBB = WorkList.pop_back_val();

    if (SensitiveBranchBlocks.test(MBB->getNumber()) && !SensitiveBlocks.test(MBB->getNumber())) {
      handleBranch(MBB, MR);
    }

    if (!SensitiveBlocks.test(MBB->getNumber()))
      MRI->setRegionFor(MBB, MR);

    for (auto *Succ : MBB->successors()) {
      if (!HandledBlocks.test(Succ->getNumber()) && !SensitiveBlocks.test(MBB->getNumber())) {
        HandledBlocks.set(Succ->getNumber());
        WorkList.push_back(Succ);
      }
    }
  }
}

bool SensitiveRegionAnalysis::runOnMachineFunction(MachineFunction &MF) {
  if (MRI)
    delete MRI;
  TSA = &getAnalysis<TrackSecretsAnalysis>();
  MDT = &getAnalysis<MachineDominatorTree>();
  MPDT = &getAnalysis<MachinePostDominatorTree>();
  MDF = &getAnalysis<MachineDominanceFrontier>();
  MRI = new MachineRegionInfo();
  MRI->init(MF, MDT, MPDT, MDF);

  SensitiveRegions.clear();
  SensitiveBlocks.clear();
  SensitiveBranchBlocks.clear();
  IfBranchMap.clear();
  ElseBranchMap.clear();
  SensitiveBranches.clear();

  auto &Secrets = TSA->SecretUses;

  SmallPtrSet<MachineBasicBlock *, 16> HandledBranches;

  // Mark blocks with secret dependent branches
  for (auto &Secret : Secrets) {
    if (!(Secret.second.getSecretMask() & 1u))
      continue;

    auto *User = Secret.second.getUser();

    // We still need those registers
    // TODO: Does this code belong here? Can is be removed?
    for (auto &MO : User->uses()) {
      if (MO.isReg())
        MO.setIsKill(false);
    }

    if (User->isConditionalBranch()) {
      SensitiveBranchBlocks.set(User->getParent()->getNumber());
    }
  }

  MachineRegion *TopLevelRegion = MRI->getTopLevelRegion();
  LLVM_DEBUG(TopLevelRegion->dump());

  // Remove garbage from MachineRegionInfo, I don't want it
  for (auto &Child : *TopLevelRegion) {
    TopLevelRegion->removeSubRegion(&*Child);
  }

  // Construct tree of sensitive regions
  handleRegion(TopLevelRegion);

  LLVM_DEBUG(MRI->dump());

  for (auto &B : SensitiveBranches) {
    LLVM_DEBUG(errs() << "Sensitive branch: " << B.MBB->getFullName() << "\n");
    LLVM_DEBUG(errs() << "if region:\n");
    LLVM_DEBUG(B.IfRegion->dump());

    if (B.ElseRegion) {
      LLVM_DEBUG(errs() << "else region:\n");
      LLVM_DEBUG(B.ElseRegion->dump());
    }
  }


  return false;
}

char SensitiveRegionAnalysis::ID = 0;
char &llvm::SensitiveRegionAnalysisID = SensitiveRegionAnalysis::ID;

SensitiveRegionAnalysis::SensitiveRegionAnalysis(bool IsSSA)
    : MachineFunctionPass(ID), IsSSA(IsSSA) {
  initializeSensitiveRegionAnalysisPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(SensitiveRegionAnalysis, "sensitive-region",
                      "Sensitive Region Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysis)
INITIALIZE_PASS_END(SensitiveRegionAnalysis, "sensitive-region",
                    "Sensitive Region Analysis", true, true)

namespace llvm {

FunctionPass *createSensitiveRegionAnalysisPass(bool IsSSA) {
  return new SensitiveRegionAnalysis(IsSSA);
}

} // namespace llvm

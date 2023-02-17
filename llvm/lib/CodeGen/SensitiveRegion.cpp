
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
  if (SensitiveBranches.contains(MBB)) {
    SensitiveBranches.erase(Key);
    for (auto &Pair : IfBranchMap)
      if (Pair.getSecond().contains(Key))
        Pair.getSecond().erase(Key);
    for (auto &Pair : ElseBranchMap)
      if (Pair.getSecond().contains(Key))
        Pair.getSecond().erase(Key);
  }
}

void SensitiveRegionAnalysis::handleBranch(MachineBasicBlock *MBB) {
  const auto &ST = MBB->getParent()->getSubtarget();
  const auto *TII = ST.getInstrInfo();
  MBB->dump();

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

  MachineRegion *FR = getMaxRegionFor(FBB);

  errs() << "FR\n";
  FR->dump();
  // FR->getExit()->dump();

  // Find the exiting blocks of this region
  SmallVector<MachineBasicBlock *> Exitings;
  FR->getExitingBlocks(Exitings);

  bool HasElseRegion = FR->getExit() != TBB;

  MachineRegion *TR = nullptr;
  if (HasElseRegion) {
    TR = getMaxRegionFor(TBB);
  } else {
    assert(FR->getExit() == TBB && "AMi error: if branch without else "
                                   "region must exit to branch target");
  }

  SensitiveRegions.insert(TR);
  SensitiveRegions.insert(FR);
  auto Branch = SensitiveBranch(MBB, Cond, TR, FR);
  SensitiveBranches.insert(Branch);

  if (TR) {
    errs() << "TR\n";
    TR->dump();
    for (auto *MBB : TR->blocks()) {
      SensitiveBlocks.set(MBB->getNumber());
      ElseBranchMap[MBB].insert(Branch);
    }
  }

  for (auto *MBB : FR->blocks()) {
    SensitiveBlocks.set(MBB->getNumber());
    IfBranchMap[MBB].insert(Branch);
  }
}

bool SensitiveRegionAnalysis::runOnMachineFunction(MachineFunction &MF) {
  // MRI = &getAnalysis<MachineRegionInfoPass>().getRegionInfo();
  // auto &Secrets =
  // getAnalysis<TrackSecretsAnalysisVirtReg>().getSecrets().SecretUses;
  MRI = &getAnalysis<MachineRegionInfoPass>().getRegionInfo();
  TSA = &getAnalysis<TrackSecretsAnalysis>();

  MRI->dump();

  auto &Secrets = TSA->SecretUses;

  SmallPtrSet<MachineBasicBlock *, 16> HandledBranches;

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
      if (HandledBranches.contains(User->getParent())) {
        // Already handled this branch
        continue;
      }

      handleBranch(User->getParent());
      HandledBranches.insert(User->getParent());
    }
  }

  for (auto &B : SensitiveBranches) {
    errs() << "Sensitive branch: " << B.MBB->getFullName() << "\n";
    errs() << "if region:\n";
    B.IfRegion->dump();

    if (B.ElseRegion) {
      errs() << "else region:\n";
      B.ElseRegion->dump();
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

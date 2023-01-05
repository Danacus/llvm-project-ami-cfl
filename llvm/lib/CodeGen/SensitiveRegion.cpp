
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/PassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "sensitive-region"

void SensitiveRegionAnalysisImpl::removeBranch(MachineBasicBlock *MBB) {
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

void SensitiveRegionAnalysisImpl::handleBranch(MachineBasicBlock *MBB) {
  const auto &ST = MBB->getParent()->getSubtarget();
  const auto *TII = ST.getInstrInfo();

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

  MachineRegion *FR = getMaxRegionFor(FBB);

  FR->dump();
  FR->getExit()->dump();

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

bool SensitiveRegionAnalysisImpl::run(MachineFunction &MF,
                                      MachineRegionInfo *MRI,
                                      TrackSecretsAnalysisImpl *TSA) {
  // MRI = &getAnalysis<MachineRegionInfoPass>().getRegionInfo();
  // auto &Secrets =
  // getAnalysis<TrackSecretsAnalysisVirtReg>().getSecrets().SecretUses;
  this->MRI = MRI;
  this->TSA = TSA;
  auto &Secrets = TSA->SecretUses;

  SmallPtrSet<MachineBasicBlock *, 16> HandledBranches;

  for (auto &Secret : Secrets) {
    if (!(Secret.second.getSecretMask() & 1u))
      continue;

    auto *User = Secret.second.getUser();

    // We still need those registers
    // for (auto &MO : User->uses()) {
    //   if (MO.isReg())
    //     MO.setIsKill(false);
    // }

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

char SensitiveRegionAnalysisVirtReg::ID = 0;
char &llvm::SensitiveRegionAnalysisVirtRegID = SensitiveRegionAnalysisVirtReg::ID;

char SensitiveRegionAnalysisPhysReg::ID = 0;
char &llvm::SensitiveRegionAnalysisPhysRegID = SensitiveRegionAnalysisPhysReg::ID;

SensitiveRegionAnalysisVirtReg::SensitiveRegionAnalysisVirtReg()
    : MachineFunctionPass(ID) {
  initializeSensitiveRegionAnalysisVirtRegPass(*PassRegistry::getPassRegistry());
}

SensitiveRegionAnalysisPhysReg::SensitiveRegionAnalysisPhysReg()
    : MachineFunctionPass(ID) {
  initializeSensitiveRegionAnalysisPhysRegPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(SensitiveRegionAnalysisVirtReg, "sensitive-region-vreg",
                      "Sensitive Region Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysisVirtReg)
INITIALIZE_PASS_END(SensitiveRegionAnalysisVirtReg, "sensitive-region-vreg",
                    "Sensitive Region Analysis", true, true)

INITIALIZE_PASS_BEGIN(SensitiveRegionAnalysisPhysReg, "sensitive-region-physreg",
                      "Sensitive Region Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysisPhysReg)
INITIALIZE_PASS_END(SensitiveRegionAnalysisPhysReg, "sensitive-region-physreg",
                    "Sensitive Region Analysis", true, true)

namespace llvm {

FunctionPass *createSensitiveRegionVirtRegPass() {
  return new SensitiveRegionAnalysisVirtReg();
}

FunctionPass *createSensitiveRegionPhysRegPass() {
  return new SensitiveRegionAnalysisPhysReg();
}

} // namespace llvm

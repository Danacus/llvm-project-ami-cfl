
#include "llvm/InitializePasses.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "sensitive-region"

bool SensitiveRegionAnalysisPass::runOnMachineFunction(MachineFunction &MF) {
  auto &MRI = getAnalysis<MachineRegionInfoPass>().getRegionInfo();
  auto &Secrets = getAnalysis<TrackSecretsAnalysisVirtReg>().TSA.SecretUses;

  const auto &ST = MF.getSubtarget();
  const auto *TII = ST.getInstrInfo();

  SmallPtrSet<MachineInstr *, 16> HandledBranches;

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
      if (HandledBranches.contains(User)) {
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
      while (auto *Expanded = FR->getExpandedRegion()) {
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
        while (auto *Expanded = TR->getExpandedRegion()) {
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

      HandledBranches.insert(User);
      SensitiveRegions.insert(TR);
      SensitiveRegions.insert(FR);
      SensitiveBranches.push_back(SensitiveBranch(User, Cond, TR, FR));
    }
  }

  for (auto &B : SensitiveBranches) {
    errs() << "Sensitive branch: " << *B.MI;
    errs() << "if region:\n";
    B.IfRegion->dump();

    if (B.ElseRegion) {
      errs() << "else region:\n";
      B.ElseRegion->dump();
    }
  }
  
  return false;
}

char SensitiveRegionAnalysisPass::ID = 0;
char &llvm::SensitiveRegionAnalysisPassID = SensitiveRegionAnalysisPass::ID;

SensitiveRegionAnalysisPass::SensitiveRegionAnalysisPass() : MachineFunctionPass(ID) {
  initializeSensitiveRegionAnalysisPassPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(SensitiveRegionAnalysisPass, DEBUG_TYPE, "Sensitive Region Analysis",
                      true, true)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysisVirtReg)
INITIALIZE_PASS_END(SensitiveRegionAnalysisPass, DEBUG_TYPE, "Sensitive Region Analysis",
                    true, true)

namespace llvm {

FunctionPass *createSensitiveRegionPass() {
  return new SensitiveRegionAnalysisPass();
}

} // namespace llvm

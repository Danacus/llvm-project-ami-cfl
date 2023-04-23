
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/CodeGen/AMiLinearizationAnalysisSESE.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/PassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

#define DEBUG_TYPE "ami-linearization-analysis"

MachineBasicBlock *LinearizationAnalysisSESE::chooseUnconditionalSuccessor(
    MachineBasicBlock *MBB,
    iterator_range<std::vector<MachineBasicBlock *>::iterator> Choices) {
  // 1. Always use ghost edges if possible
  // 2. Don't pick a post-dominator
  // 3. Use the fallthrough if possible
  // 4. Prefer blocks with lower number

  SmallVector<MachineBasicBlock *> FilteredChoices;

  for (MachineBasicBlock *Choice : Choices) {
    if (Result.GhostEdges.contains({MBB, Choice})) {
      // Always pick the ghost edge if possible, it's already linearized in that
      // case, so we don't need to do anything else
      return Choice;
    }

    // Never pick a post-dominator as unconditional successor,
    // unless this block only has a single successor (should never happen, but
    // would be fine)
    if (MPDT->dominates(Choice, MBB) && MBB->succ_size() > 1)
      continue;

    FilteredChoices.push_back(Choice);
  }

  if (FilteredChoices.size() == 0)
    return nullptr;

  // Check if we can use the fallthrough
  auto *Fallthrough = MBB->getFallThrough(true);
  if (MBB->canFallThrough() && Fallthrough) {
    if (std::find(FilteredChoices.begin(), FilteredChoices.end(),
                  Fallthrough) != FilteredChoices.end()) {
      return Fallthrough;
    }
  }

  // Basic heuristic
  std::sort(FilteredChoices.begin(), FilteredChoices.end(),
            [](MachineBasicBlock *A, MachineBasicBlock *B) {
              return A->getNumber() < B->getNumber();
            });

  return *FilteredChoices.begin();
}

void LinearizationAnalysisSESE::linearizeBranch(
    MachineBasicBlock *MBB, MachineBasicBlock *UncondSucc,
    MachineBasicBlock *Target) {
  LLVM_DEBUG(errs() << "Linearize Branch ");
  LLVM_DEBUG(MBB->printAsOperand(errs()));
  LLVM_DEBUG(errs() << "\n");

  LLVM_DEBUG(errs() << "  UncondSucc ");
  LLVM_DEBUG(UncondSucc->printAsOperand(errs()));
  LLVM_DEBUG(errs() << "\n");

  assert(MBB->isSuccessor(UncondSucc) &&
         "Expected UncondSucc to be a successor of MBB");

  if (!Result.ActivatingEdges.contains({MBB, Target}))
    Result.ActivatingEdges.insert({MBB, Target});

  LLVM_DEBUG(errs() << "  Target ");
  LLVM_DEBUG(Target->printAsOperand(errs()));
  LLVM_DEBUG(errs() << "\n");

  SmallVector<MachineBasicBlock *, 4> Exitings;
  auto *Region = SRA->getSensitiveRegion(UncondSucc);
  assert(Region && "No sensitive region found for secret-dependent branch");
  Region->getExitingBlocks(Exitings);

  for (auto *Exiting : Exitings) {
    LLVM_DEBUG(errs() << "  Exiting ");
    LLVM_DEBUG(Exiting->printAsOperand(errs()));
    LLVM_DEBUG(errs() << "\n");

    MachineBasicBlock *NewSucc = nullptr;

    if (Exiting->succ_size() == 2) {
      llvm_unreachable("Only simple SESE regions are supported");
      // assert(!GhostEdges.contains({Exiting, Region->getExit()}) &&
      //        "Relinearziation not supported by SESE version");
      // for (auto *Succ : Exiting->successors()) {
      //   if (Succ != Region->getExit()) {
      //     // If Succ is not the exit, it must be another block within the SESE region
      //     NewSucc = Succ;
      //   }
      // }
    } else if (Exiting->succ_size() == 1) {
      // Add Ghost Edge
      if (!Exiting->isSuccessor(Target) &&
          !Result.GhostEdges.contains({Exiting, Target})) {
        Result.GhostEdges.insert({Exiting, Target});
        Exiting->addSuccessor(Target);
      }

      // Remove Activating Edge from the CFG, since they don't count as control flow edge
      if (MBB->isSuccessor(Target))
        MBB->removeSuccessor(Target);

      // Update Dominator Trees
      MDT->calculate(*MF);
      MPDT->getBase().recalculate(*MF);
      NewSucc = Target;
    }

    if (NewSucc != Region->getExit())
      linearizeBranch(Exiting, NewSucc, Region->getExit());
  }

  if (MBB->isSuccessor(Target)) {
    MBB->removeSuccessor(Target);
    MDT->calculate(*MF);
    MPDT->getBase().recalculate(*MF);
  }

  // UncondEdges.insert({MBB, UncondSucc});

  LLVM_DEBUG(errs() << "Linearize Branch end\n");
}

void LinearizationAnalysisSESE::linearize() {  
  SmallVector<MachineBasicBlock *> ToLinearize;

  for (auto &Node : make_range(df_begin(MDT), df_end(MDT))) {
    auto *MBB = Node->getBlock();
    if (Result.SensitiveBranchBlocks.test(MBB->getNumber())) {
      ToLinearize.push_back(MBB);
    }
  }

  for (auto *MBB : ToLinearize) {
    auto *UncondSucc = chooseUnconditionalSuccessor(MBB, MBB->successors());
    assert(UncondSucc &&
           "Cannot linearize branch: no valid unconditional successor");
    for (auto *Succ : MBB->successors()) {
      if (Succ != UncondSucc)
        linearizeBranch(MBB, UncondSucc, Succ);
    }
  }
}
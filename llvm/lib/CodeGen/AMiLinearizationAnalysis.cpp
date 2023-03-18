
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/CodeGen/AMiLinearizationAnalysis.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/PassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "ami-linearization-analysis"

void AMiLinearizationAnalysis::findActivatingRegionExitings(
    MachineBasicBlock *Entry, MachineBasicBlock *Target,
    SmallVectorImpl<MachineBasicBlock *> &Exitings,
    SmallPtrSetImpl<MachineBasicBlock *> &RegionBlocks) {
  SmallVector<MachineDomTreeNode *, 8> WorkList;

  auto *EntryNode = MDT->getNode(Entry);
  WorkList.push_back(EntryNode);

  while (!WorkList.empty()) {
    auto *Node = WorkList.pop_back_val();

    bool IsExiting = false;

    SmallVector<MachineBasicBlock *, 8> Succs;
    realSuccessors(Node->getBlock(), Succs);

    for (auto *Succ : Succs) {
      if (!MDT->dominates(EntryNode->getBlock(), Succ)) {
        IsExiting = true;
      }    
    }

    if (IsExiting)
      Exitings.push_back(Node->getBlock());

    for (auto *Child : Node->children()) {
      LLVM_DEBUG(errs() << "Child ");
      LLVM_DEBUG(Child->getBlock()->printAsOperand(errs()));
      LLVM_DEBUG(errs() << "\n");
      WorkList.push_back(Child); 
    }

    RegionBlocks.insert(Node->getBlock());
  }
}

MachineBasicBlock *AMiLinearizationAnalysis::chooseUnconditionalSuccessor(
    MachineBasicBlock *MBB, iterator_range<std::vector<MachineBasicBlock *>::iterator> Choices) {
  // 1. Prefer ghost edges if possible
  // 2. other heuristics (TODO)

  MachineBasicBlock *Best = *Choices.begin();

  for (MachineBasicBlock *Choice : Choices) {
    if (GhostEdges.contains({MBB, Choice})) {
      Best = Choice;
    }
  }

  return Best;
}

void AMiLinearizationAnalysis::linearizeBranch(MachineBasicBlock *MBB,
                                               MachineBasicBlock *UncondSucc) {
  LLVM_DEBUG(errs() << "Linearize Branch ");
  LLVM_DEBUG(MBB->printAsOperand(errs()));
  LLVM_DEBUG(errs() << "\n");

  LLVM_DEBUG(errs() << "UncondSucc ");
  LLVM_DEBUG(UncondSucc->printAsOperand(errs()));
  LLVM_DEBUG(errs() << "\n");

  SmallVector<MachineBasicBlock *, 8> Succs;
  realSuccessors(MBB, Succs);

  for (auto *Succ : Succs) {
    if (Succ != UncondSucc) {
      ActivatingEdges.insert({MBB, Succ});
    }
  }

  // MDT->calculate(*MF);

  for (auto *Target : Succs) {
    LLVM_DEBUG(errs() << "Target ");
    LLVM_DEBUG(Target->printAsOperand(errs()));
    LLVM_DEBUG(errs() << "\n");

    if (Target != UncondSucc) {
      SmallVector<MachineBasicBlock *, 4> Exitings;
      SmallPtrSet<MachineBasicBlock *, 8> RegionBlocks;
      findActivatingRegionExitings(UncondSucc, Target, Exitings, RegionBlocks);

      SmallVector<std::pair<MachineBasicBlock *, SmallVector<MachineBasicBlock *>>, 4> ToLinearize;

      for (auto *Exiting : Exitings) {
        LLVM_DEBUG(errs() << "Exiting ");
        LLVM_DEBUG(Exiting->printAsOperand(errs()));
        LLVM_DEBUG(errs() << "\n");

        SmallVector<MachineBasicBlock *> InternalSuccessors;
        bool IsUnconditionalExiting = true;

        for (auto *ESucc : Exiting->successors()) {
          if (MDT->dominates(UncondSucc, ESucc)) {
            // ESucc is not an exit of the activating region
            IsUnconditionalExiting = false;
            InternalSuccessors.push_back(ESucc);
          }
        }

        if (!IsUnconditionalExiting) {
          ToLinearize.push_back({ Exiting, std::move(InternalSuccessors) });
        } else {
          if (!Exiting->isSuccessor(Target) &&
              !GhostEdges.contains({Exiting, Target})) {
            GhostEdges.insert({Exiting, Target});
            Exiting->addSuccessor(Target);
          }
          MBB->removeSuccessor(Target);
          MDT->calculate(*MF);
          linearizeBranch(Exiting, Target);
        }
      }

      for (auto &Pair : ToLinearize) {
        auto *Succ = chooseUnconditionalSuccessor(Pair.first, Pair.second);
        linearizeBranch(Pair.first, Succ);
      }

      // ActivatingRegions.insert(
      //     {{MBB, Target}, ActivatingRegion()});
    }
  }

  LLVM_DEBUG(errs() << "Linearize Branch end\n");
}

bool AMiLinearizationAnalysis::runOnMachineFunction(MachineFunction &MF) {
  TSA = &getAnalysis<TrackSecretsAnalysis>();
  MDT = &getAnalysis<MachineDominatorTree>();
  MPDT = &getAnalysis<MachinePostDominatorTree>();
  MDF = &getAnalysis<MachineDominanceFrontier>();
  this->MF = &MF;

  HandledBlocks.clear();
  SensitiveRegions.clear();
  SensitiveBlocks.clear();
  SensitiveBranchBlocks.clear();
  RegionMap.clear();

  auto &Secrets = TSA->SecretUses;

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

    LLVM_DEBUG(User->dump());

    if (User->isConditionalBranch() || User->isIndirectBranch()) {
      SensitiveBranchBlocks.set(User->getParent()->getNumber());
    }
  }

  for (auto &MBB : MF) {
    if (SensitiveBranchBlocks.test(MBB.getNumber())) {
      SmallVector<MachineBasicBlock *, 8> Succs;
      realSuccessors(&MBB, Succs);
      auto *Succ = chooseUnconditionalSuccessor(&MBB, Succs);
      linearizeBranch(&MBB, Succ);
    }
  }

  for (auto &Edge : ActivatingEdges) {
    auto *Entry = Edge.first->getSingleSuccessor();
    auto *Exit = Edge.second;

    SmallPtrSet<MachineBasicBlock *, 8> Blocks;
    for (auto *Node : regionDomTreeIterator(Entry, Exit)) {
      Blocks.insert(Node->getBlock());
    }

    ActivatingRegions.insert({Edge, ActivatingRegion(Entry, Exit, Blocks)});
  }

  LLVM_DEBUG(MF.dump());
  LLVM_DEBUG(MDT->dump());

  LLVM_DEBUG(errs() << "Ghost edges:\n");

  for (auto &Edge : GhostEdges) {
    LLVM_DEBUG(errs() << "<");
    LLVM_DEBUG(Edge.first->printAsOperand(errs()));
    LLVM_DEBUG(errs() << " " << Edge.first->getName());
    LLVM_DEBUG(errs() << ", ");
    LLVM_DEBUG(Edge.second->printAsOperand(errs()));
    LLVM_DEBUG(errs() << " " << Edge.second->getName());
    LLVM_DEBUG(errs() << ">\n");
  }

  LLVM_DEBUG(errs() << "----------------------\n");

  LLVM_DEBUG(errs() << "Activating regions:\n");

  for (auto &Region : ActivatingRegions) {
    LLVM_DEBUG(errs() << "<");
    LLVM_DEBUG(Region.getFirst().first->printAsOperand(errs()));
    LLVM_DEBUG(errs() << " " << Region.getFirst().first->getName());
    LLVM_DEBUG(errs() << ", ");
    LLVM_DEBUG(Region.getFirst().second->printAsOperand(errs()));
    LLVM_DEBUG(errs() << " " << Region.getFirst().second->getName());
    LLVM_DEBUG(errs() << ">\n");

    LLVM_DEBUG(errs() << "Region blocks:\n");

    for (auto *Block : Region.getSecond().Blocks) {
      LLVM_DEBUG(Block->printAsOperand(errs()));
      LLVM_DEBUG(errs() << " " << Block->getName());
      LLVM_DEBUG(errs() << "\n");
    }

    LLVM_DEBUG(errs() << "------------\n");
  }

  if (IsSSA) {
    for (auto &Edge : GhostEdges) {
      Edge.first->removeSuccessor(Edge.second);
    }

    for (auto &Edge : ActivatingEdges) {
      Edge.first->addSuccessor(Edge.second);
    }

    MDT->calculate(MF);
  }

  return false;
}

char AMiLinearizationAnalysis::ID = 0;
char &llvm::AMiLinearizationAnalysisID = AMiLinearizationAnalysis::ID;

AMiLinearizationAnalysis::AMiLinearizationAnalysis(bool IsSSA)
    : MachineFunctionPass(ID), IsSSA(IsSSA) {
  initializeAMiLinearizationAnalysisPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AMiLinearizationAnalysis, DEBUG_TYPE,
                      "Sensitive Region Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysis)
INITIALIZE_PASS_END(AMiLinearizationAnalysis, DEBUG_TYPE,
                    "Sensitive Region Analysis", true, true)

namespace llvm {

FunctionPass *createAMiLinearizationAnalysisPass(bool IsSSA) {
  return new AMiLinearizationAnalysis(IsSSA);
}

} // namespace llvm

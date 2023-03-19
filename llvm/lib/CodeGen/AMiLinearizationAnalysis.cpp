
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
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

#define DEBUG_TYPE "ami-linearization-analysis"

void AMiLinearizationAnalysis::findActivatingRegionExitings(
    MachineBasicBlock *Entry, MachineBasicBlock *Target,
    SmallVectorImpl<MachineBasicBlock *> &Exitings) {
  auto *EntryNode = MDT->getNode(Entry);
  SmallVector<MachineDomTreeNode *, 8> WorkList;
  WorkList.push_back(EntryNode);

  while (!WorkList.empty()) {
    auto *Node = WorkList.pop_back_val();

    bool IsExiting = false;

    for (auto *Succ : Node->getBlock()->successors()) {
      // TODO: This is not very precise, but might be good enough
      if (!MDT->dominates(EntryNode->getBlock(), Succ)) {
        IsExiting = true;
      }
    }

    for (auto *Child : Node->children()) {
      if (Child->getBlock() == Target) {
        IsExiting = true;
        continue;
      }
      LLVM_DEBUG(errs() << "Child ");
      LLVM_DEBUG(Child->getBlock()->printAsOperand(errs()));
      LLVM_DEBUG(errs() << "\n");
      WorkList.push_back(Child);
    }

    if (IsExiting)
      Exitings.push_back(Node->getBlock());
  }
}

MachineBasicBlock *AMiLinearizationAnalysis::chooseUnconditionalSuccessor(
    MachineBasicBlock *MBB,
    iterator_range<std::vector<MachineBasicBlock *>::iterator> Choices) {
  // 1. Always use ghost edges if possible
  // 2. Don't pick a post-dominator
  // 3. Use the fallthrough if possible
  // 4. Prefer blocks with lower number

  SmallVector<MachineBasicBlock *> FilteredChoices;

  for (MachineBasicBlock *Choice : Choices) {
    if (GhostEdges.contains({MBB, Choice})) {
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

void AMiLinearizationAnalysis::linearizeBranch(MachineBasicBlock *MBB,
                                               MachineBasicBlock *UncondSucc,
                                               MachineBasicBlock *Target) {
  LLVM_DEBUG(errs() << "Linearize Branch ");
  LLVM_DEBUG(MBB->printAsOperand(errs()));
  LLVM_DEBUG(errs() << "\n");

  LLVM_DEBUG(errs() << "  UncondSucc ");
  LLVM_DEBUG(UncondSucc->printAsOperand(errs()));
  LLVM_DEBUG(errs() << "\n");

  assert(MBB->isSuccessor(UncondSucc) &&
         "Expected UncondSucc to be a successor of MBB");

  // If there is only one successor, this branch has likely
  // been linearized before.
  if (MBB->succ_size() == 1) {
    if (MBB->getSingleSuccessor() == UncondSucc) {
      // If it was previously linearized with the same unconditional successor,
      // there is no need to do anything.
      return;
    }

    if (!MPDT->dominates(MBB->getSingleSuccessor(), UncondSucc)) {
      // If the original unconditional successor post-dominates the new one,
      // we can safely relinearize to the new one, otherwise we give up.
      llvm_unreachable("Failed to linearize");
    } else {
      if (GhostEdges.contains({MBB, MBB->getSingleSuccessor()})) {
        GhostEdges.erase({MBB, MBB->getSingleSuccessor()});
      }
    }
  }

  for (auto *Succ : MBB->successors()) {
    if (Succ != UncondSucc) {
      if (GhostEdges.contains({MBB, Succ})) {
        GhostEdges.erase({MBB, Succ});
        MBB->removeSuccessor(Succ);
        MDT->calculate(*MF);
        MPDT->getBase().recalculate(*MF);

        if (Target == Succ)
          // This was a ghost edge, so don't turn it into an activating edge
          return;
      }

      if (GhostEdges.contains({MBB, Succ}) || UncondEdges.contains({MBB, Succ}))
        assert(MPDT->dominates(Succ, UncondSucc) &&
               "Cannot relinearize to new UncondSucc");
    }
  }

  if (!ActivatingEdges.contains({MBB, Target}))
    ActivatingEdges.insert({MBB, Target});

  // for (auto *Target : TargetsToLinearize) {
  LLVM_DEBUG(errs() << "  Target ");
  LLVM_DEBUG(Target->printAsOperand(errs()));
  LLVM_DEBUG(errs() << "\n");

  SmallVector<MachineBasicBlock *, 4> Exitings;
  findActivatingRegionExitings(UncondSucc, Target, Exitings);

  SmallVector<std::pair<MachineBasicBlock *,
                        std::pair<SmallPtrSet<MachineBasicBlock *, 4>,
                                  SmallPtrSet<MachineBasicBlock *, 4>>>,
              4>
      ToLinearize;

  bool CreatedExit = false;

  for (auto *Exiting : Exitings) {
    LLVM_DEBUG(errs() << "  Exiting ");
    LLVM_DEBUG(Exiting->printAsOperand(errs()));
    LLVM_DEBUG(errs() << "\n");

    SmallPtrSet<MachineBasicBlock *, 4> Exits;
    SmallPtrSet<MachineBasicBlock *, 4> InternalSuccessors;

    for (auto *ESucc : Exiting->successors()) {
      if (MDT->dominates(UncondSucc, ESucc)) {
        // ESucc is not an exit of the activating region
        LLVM_DEBUG(errs() << "    Internal ");
        InternalSuccessors.insert(ESucc);
      } else {
        LLVM_DEBUG(errs() << "    Exit ");
        Exits.insert(ESucc);
      }
      LLVM_DEBUG(ESucc->printAsOperand(errs()));
      LLVM_DEBUG(errs() << "\n");
    }

    if (InternalSuccessors.size() > 0) {
      // Postpone linearization until after all other exiting blocks have been
      // considered.
      ToLinearize.push_back(
          {Exiting, {std::move(InternalSuccessors), std::move(Exits)}});
    } else {
      ToLinearize.insert(
          ToLinearize.begin(),
          {Exiting, {std::move(InternalSuccessors), std::move(Exits)}});
    }
  }

  SmallVector<MachineBasicBlock *, 8> FinishedExitings;
  SmallVector<MachineBasicBlock *, 8> Choices;

  for (auto &Pair : ToLinearize) {
    auto *Exiting = Pair.first;
    if (Exiting == Target) {
      CreatedExit = true;
      continue;
    }
    auto *Succ = Target;
    Choices.clear();

    if (CreatedExit) {
      for (auto *MBB : FinishedExitings)
        if (Exiting->isSuccessor(MBB))
          Choices.push_back(MBB);
    }

    if (Choices.size() > 0) {
      Succ = chooseUnconditionalSuccessor(Exiting, Choices);
      assert(Succ &&
             "Cannot linearize branch: no valid unconditional successor");
    } else {
      if (!Exiting->isSuccessor(Target) &&
          !GhostEdges.contains({Exiting, Target})) {
        GhostEdges.insert({Exiting, Target});
        Exiting->addSuccessor(Target);
      }
      if (MBB->isSuccessor(Target))
        MBB->removeSuccessor(Target);
      MDT->calculate(*MF);
      MPDT->getBase().recalculate(*MF);
      FinishedExitings.push_back(Exiting);
      CreatedExit = true;
    }
    for (auto *E : Pair.second.second)
      if (!MPDT->dominates(Succ, Exiting))
        linearizeBranch(Exiting, Succ, E);
  }

  assert(CreatedExit && "Unable to exit activating region");

  if (MBB->isSuccessor(Target)) {
    MBB->removeSuccessor(Target);
    MDT->calculate(*MF);
    MPDT->getBase().recalculate(*MF);
  }

  UncondEdges.insert({MBB, UncondSucc});

  LLVM_DEBUG(errs() << "Linearize Branch end\n");
}

void AMiLinearizationAnalysis::findSecretDependentBranches() {
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
}

void AMiLinearizationAnalysis::createActivatingRegions() {
  LLVM_DEBUG(MDT->dump());
  LLVM_DEBUG(MPDT->dump());
  
  SmallPtrSet<MachineBasicBlock *, 8> Blocks;
  for (auto &MBB : *MF)
    Blocks.insert(&MBB);
  Edge Edge = {MDT->getRoot(), nullptr};
  ActivatingRegions.insert(
      {Edge, ActivatingRegion(nullptr, MDT->getRoot(), nullptr, Blocks)});

  for (auto &Edge : ActivatingEdges) {
    auto *Branch = Edge.first;
    auto *Entry = Edge.first->getSingleSuccessor();
    auto *Exit = Edge.second;

    Blocks.clear();
    for (auto *Node : regionDomTreeIterator(Entry, Exit)) {
      Blocks.insert(Node->getBlock());
    }

    ActivatingRegions.insert(
        {Edge, ActivatingRegion(Branch, Entry, Exit, Blocks)});
    ActivatingRegion *AR = &ActivatingRegions.find(Edge)->getSecond();
    for (auto *MBB : Blocks) {
      RegionMap[MBB].insert(AR);
    }

    // Check that activating region is SESE
    // - Entry dominates exiting block by construction
    // - Exit should post-dominate entry
    // - Every cycle containing Entry contains Exit: assuming there is no return
    //   within the region
    for (auto *Exiting : Exit->predecessors()) {
      if (MDT->dominates(Entry, Exiting))
        assert(MPDT->dominates(Exit, Entry) && "Activating region not SESE");
    }
  }
}

void AMiLinearizationAnalysis::undoCFGChanges() {
  for (auto &Edge : GhostEdges) {
    Edge.first->removeSuccessor(Edge.second);
  }

  for (auto &Edge : ActivatingEdges) {
    Edge.first->addSuccessor(Edge.second);
  }

  MDT->calculate(*MF);
  MPDT->getBase().recalculate(*MF);
}

bool AMiLinearizationAnalysis::runOnMachineFunction(MachineFunction &MF) {
  TSA = &getAnalysis<TrackSecretsAnalysis>();
  MDT = &getAnalysis<MachineDominatorTree>();
  MPDT = &getAnalysis<MachinePostDominatorTree>();
  MDF = &getAnalysis<MachineDominanceFrontier>();
  this->MF = &MF;

  LLVM_DEBUG(MF.dump());

  SensitiveBranchBlocks.clear();
  GhostEdges.clear();
  UncondEdges.clear();
  ActivatingEdges.clear();
  ActivatingRegions.clear();
  RegionMap.clear();

  findSecretDependentBranches();

  SmallVector<MachineBasicBlock *> ToLinearize;

  for (auto &Node : make_range(df_begin(MDT), df_end(MDT))) {
    auto *MBB = Node->getBlock();
    if (SensitiveBranchBlocks.test(MBB->getNumber())) {
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

  createActivatingRegions();

  LLVM_DEBUG(MF.dump());
  LLVM_DEBUG(MDT->dump());
  LLVM_DEBUG(dump());

  if (AnalysisOnly) {
    undoCFGChanges();
    return false;
  }

  return true;
}

void AMiLinearizationAnalysis::print(raw_ostream &OS, const Module *) const {
  OS << "Ghost edges:\n";

  for (auto &Edge : GhostEdges) {
    OS << "<";
    Edge.first->printAsOperand(OS);
    OS << " " << Edge.first->getName();
    OS << ", ";
    Edge.second->printAsOperand(OS);
    OS << " " << Edge.second->getName();
    OS << ">\n";
  }

  OS << "----------------------\n";

  OS << "Activating regions:\n";

  for (auto &Pair : ActivatingRegions) {
    Pair.getSecond().print(OS);
    OS << "------------\n";
  }
}

char AMiLinearizationAnalysis::ID = 0;
char &llvm::AMiLinearizationAnalysisID = AMiLinearizationAnalysis::ID;

AMiLinearizationAnalysis::AMiLinearizationAnalysis(bool IsSSA)
    : MachineFunctionPass(ID), AnalysisOnly(IsSSA) {
  initializeAMiLinearizationAnalysisPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AMiLinearizationAnalysis, DEBUG_TYPE,
                      "AMi Linearization Analysis", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysis)
INITIALIZE_PASS_END(AMiLinearizationAnalysis, DEBUG_TYPE,
                    "AMi Linearization Analysis", false, false)

namespace llvm {

FunctionPass *createAMiLinearizationAnalysisPass(bool IsSSA) {
  return new AMiLinearizationAnalysis(IsSSA);
}

} // namespace llvm

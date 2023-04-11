
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

#define DEBUG_TYPE "ami-linearization-analysis-sese"

MachineBasicBlock *AMiLinearizationAnalysisSESE::chooseUnconditionalSuccessor(
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

void AMiLinearizationAnalysisSESE::linearizeBranch(
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

  if (!ActivatingEdges.contains({MBB, Target}))
    ActivatingEdges.insert({MBB, Target});

  LLVM_DEBUG(errs() << "  Target ");
  LLVM_DEBUG(Target->printAsOperand(errs()));
  LLVM_DEBUG(errs() << "\n");

  SmallVector<MachineBasicBlock *, 4> Exitings;
  auto &SRA = getAnalysis<SensitiveRegionAnalysis>();
  auto *Region = SRA.getSensitiveRegion(UncondSucc);
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
          !GhostEdges.contains({Exiting, Target})) {
        GhostEdges.insert({Exiting, Target});
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

  UncondEdges.insert({MBB, UncondSucc});

  LLVM_DEBUG(errs() << "Linearize Branch end\n");
}

void AMiLinearizationAnalysisSESE::findSecretDependentBranches() {
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

void AMiLinearizationAnalysisSESE::createActivatingRegions() {
  LLVM_DEBUG(MF->dump());
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

void AMiLinearizationAnalysisSESE::undoCFGChanges() {
  for (auto &Edge : GhostEdges) {
    Edge.first->removeSuccessor(Edge.second);
  }

  for (auto &Edge : ActivatingEdges) {
    Edge.first->addSuccessor(Edge.second);
  }

  MDT->calculate(*MF);
  MPDT->getBase().recalculate(*MF);
}

bool AMiLinearizationAnalysisSESE::runOnMachineFunction(MachineFunction &MF) {
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

void AMiLinearizationAnalysisSESE::print(raw_ostream &OS,
                                         const Module *) const {
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

char AMiLinearizationAnalysisSESE::ID = 0;
char &llvm::AMiLinearizationAnalysisSESEID = AMiLinearizationAnalysisSESE::ID;

AMiLinearizationAnalysisSESE::AMiLinearizationAnalysisSESE(bool AnalysisOnly)
    : MachineFunctionPass(ID), AnalysisOnly(AnalysisOnly) {
  initializeAMiLinearizationAnalysisSESEPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AMiLinearizationAnalysisSESE, DEBUG_TYPE,
                      "AMi Linearization Analysis (SESE)", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysis)
INITIALIZE_PASS_END(AMiLinearizationAnalysisSESE, DEBUG_TYPE,
                    "AMi Linearization Analysis (SESE)", false, false)

namespace llvm {

FunctionPass *createAMiLinearizationAnalysisSESEPass(bool AnalysisOnly) {
  return new AMiLinearizationAnalysisSESE(AnalysisOnly);
}

} // namespace llvm

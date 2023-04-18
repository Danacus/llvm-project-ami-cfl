
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

MachineBasicBlock *AMiLinearizationAnalysis::nearestDeferral(MachineBasicBlock *MBB) {
  unsigned int ClosestIndex = Blocks.size();
  MachineBasicBlock *Closest = nullptr;

  for (auto &Pair : DeferralEdges) {
    if (Pair.first == MBB) {
      if (BlockIndex[Pair.second] < ClosestIndex) {
        Closest = Pair.second;
        ClosestIndex = BlockIndex[Pair.second];
      }
    }
  }
  
  return Closest;
}

MachineBasicBlock *AMiLinearizationAnalysis::nearestSuccessor(MachineBasicBlock *MBB) {
  unsigned int ClosestIndex = Blocks.size();
  MachineBasicBlock *Closest = nullptr;

  for (auto *Succ : MBB->successors()) {
    if (BlockIndex[Succ] < BlockIndex[MBB])
      // Skip backedges
      continue;
    if (BlockIndex[Succ] < ClosestIndex) {
      Closest = Succ;
      ClosestIndex = BlockIndex[Succ];
    }
  }
  
  return Closest;
}

void AMiLinearizationAnalysis::linearize() {
  for (auto *MBB : Blocks) {
    LLVM_DEBUG(MBB->dump());
    
    if (!SensitiveBranchBlocks.test(MBB->getNumber())) {
      // Not a secret-dependent branch
      for (auto *Succ : MBB->successors()) {
        if (BlockIndex[Succ] < BlockIndex[MBB])
          // Skip backedges
          continue;
        auto *Next = Succ;
        auto *NextD = nearestDeferral(MBB);
        if (NextD && BlockIndex[NextD] < BlockIndex[Next]) {
          Next = NextD;
          GhostEdges.insert({ MBB, Next });
          ActivatingEdges.insert({ MBB, Succ });
        }

        if (Succ != Next) {
          DeferralEdges.insert({ Next, Succ });
        }

        for (auto &Pair : DeferralEdges) {
          if (Pair.first == MBB && Pair.second != Next) {
            DeferralEdges.insert({ Next, Pair.second });
          }
        }
      }
    } else {
      // Secret-dependent branch
      auto *Next = nearestSuccessor(MBB);
      auto *NextD = nearestDeferral(MBB);
      if (NextD && BlockIndex[NextD] < BlockIndex[Next]) {
        Next = NextD;
        GhostEdges.insert({ MBB, Next });
      }
      assert(Next && "Expected successor");
      LLVM_DEBUG(Next->dump());

      for (auto *Succ : MBB->successors()) {
        if (BlockIndex[Succ] < BlockIndex[MBB])
          // Skip backedges
          continue;
        if (Succ != Next) {
          ActivatingEdges.insert({ MBB, Succ });
          DeferralEdges.insert({ Next, Succ });
        }
      }

      for (auto &Pair : DeferralEdges) {
        if (Pair.first == MBB && Pair.second != Next) {
          DeferralEdges.insert({ Next, Pair.second });
        }
      }
    }

    SmallSet<Edge, 4> ToRemove;
    for (auto &Pair : DeferralEdges) {
      if (Pair.first == MBB)
        ToRemove.insert(Pair);
    }
    for (auto &Pair : ToRemove)
      DeferralEdges.erase(Pair);

    LLVM_DEBUG(dump());
  }

  assert(DeferralEdges.empty() && "Unexpected failure");
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
  for (auto &Edge : GhostEdges) {
    Edge.first->addSuccessor(Edge.second);
  }

  for (auto &Edge : ActivatingEdges) {
    Edge.first->removeSuccessor(Edge.second);
  }

  MDT->calculate(*MF);
  MPDT->getBase().recalculate(*MF);

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
    MachineBasicBlock *Entry = nullptr;
    for (auto *Succ : Branch->successors()) {
      if (!ActivatingEdges.contains({ Edge.first, Succ })) {
        Entry = Succ;
      }
    }
    assert(Entry && "Expected an entry");
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
  DeferralEdges.clear();
  ActivatingRegions.clear();
  RegionMap.clear();
  Blocks.clear();
  BlockIndex.clear();

  auto &CO = getAnalysis<CompactOrder>();
  unsigned int Index = 0;
  // for (auto &Node : make_range(df_begin(MDT), df_end(MDT))) {
  // TODO: use proper compact order
  // for (auto &MBB : MF) {
  for (auto &Node : CO.Order) {
    if (Node->IsLoop)
      continue;
    auto *MBB = Node->getBlock();
    BlockIndex.insert({ MBB, Index++ });
    Blocks.push_back(MBB);
  }

  findSecretDependentBranches();

  linearize();

  LLVM_DEBUG(dump());

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

  OS << "Activating edges:\n";

  for (auto &Edge : ActivatingEdges) {
    OS << "<";
    Edge.first->printAsOperand(OS);
    OS << " " << Edge.first->getName();
    OS << ", ";
    Edge.second->printAsOperand(OS);
    OS << " " << Edge.second->getName();
    OS << ">\n";
  }

  OS << "Deferral edges:\n";

  for (auto &Edge : DeferralEdges) {
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

AMiLinearizationAnalysis::AMiLinearizationAnalysis(bool AnalysisOnly)
    : MachineFunctionPass(ID), AnalysisOnly(AnalysisOnly) {
  initializeAMiLinearizationAnalysisPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AMiLinearizationAnalysis, DEBUG_TYPE,
                      "AMi Linearization Analysis", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysis)
INITIALIZE_PASS_END(AMiLinearizationAnalysis, DEBUG_TYPE,
                    "AMi Linearization Analysis", false, false)

namespace llvm {

FunctionPass *createAMiLinearizationAnalysisPass(bool AnalysisOnly) {
  return new AMiLinearizationAnalysis(AnalysisOnly);
}

} // namespace llvm

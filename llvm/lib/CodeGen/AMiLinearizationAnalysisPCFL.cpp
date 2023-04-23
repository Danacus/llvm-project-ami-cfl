
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/CodeGen/AMiLinearizationAnalysisPCFL.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/PassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/Target/TargetMachine.h"

#define DEBUG_TYPE "ami-linearization-analysis"

using namespace llvm;

MachineBasicBlock *
LinearizationAnalysisPCFL::nearestDeferral(MachineBasicBlock *MBB) {
  unsigned int ClosestIndex = Blocks.size();
  MachineBasicBlock *Closest = nullptr;

  for (auto &Pair : Result.DeferralEdges) {
    if (Pair.first == MBB) {
      if (BlockIndex[Pair.second] < ClosestIndex) {
        Closest = Pair.second;
        ClosestIndex = BlockIndex[Pair.second];
      }
    }
  }

  return Closest;
}

MachineBasicBlock *
LinearizationAnalysisPCFL::nearestSuccessor(MachineBasicBlock *MBB) {
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

void LinearizationAnalysisPCFL::linearize() {
  unsigned int Index = 0;
  for (auto &Node : CO->Order) {
    if (Node->IsLoop)
      continue;
    auto *MBB = Node->getBlock();
    BlockIndex.insert({MBB, Index++});
    Blocks.push_back(MBB);
  }

  for (auto *MBB : Blocks) {
    LLVM_DEBUG(MBB->dump());

    if (!Result.SensitiveBranchBlocks.test(MBB->getNumber())) {
      // Not a secret-dependent branch
      for (auto *Succ : MBB->successors()) {
        if (BlockIndex[Succ] < BlockIndex[MBB])
          // Skip backedges
          continue;
        auto *Next = Succ;
        auto *NextD = nearestDeferral(MBB);
        if (NextD && BlockIndex[NextD] < BlockIndex[Next]) {
          Next = NextD;
          Result.GhostEdges.insert({MBB, Next});
          Result.ActivatingEdges.insert({MBB, Succ});
        }

        if (Succ != Next) {
          Result.DeferralEdges.insert({Next, Succ});
        }

        for (auto &Pair : Result.DeferralEdges) {
          if (Pair.first == MBB && Pair.second != Next) {
            Result.DeferralEdges.insert({Next, Pair.second});
          }
        }
      }
    } else {
      // Secret-dependent branch
      auto *Next = nearestSuccessor(MBB);
      auto *NextD = nearestDeferral(MBB);
      if (NextD && BlockIndex[NextD] < BlockIndex[Next]) {
        Next = NextD;
        Result.GhostEdges.insert({MBB, Next});
      }
      assert(Next && "Expected successor");
      LLVM_DEBUG(Next->dump());

      for (auto *Succ : MBB->successors()) {
        if (BlockIndex[Succ] < BlockIndex[MBB])
          // Skip backedges
          continue;
        if (Succ != Next) {
          Result.ActivatingEdges.insert({MBB, Succ});
          Result.DeferralEdges.insert({Next, Succ});
        }
      }

      for (auto &Pair : Result.DeferralEdges) {
        if (Pair.first == MBB && Pair.second != Next) {
          Result.DeferralEdges.insert({Next, Pair.second});
        }
      }
    }

    SmallSet<Edge, 4> ToRemove;
    for (auto &Pair : Result.DeferralEdges) {
      if (Pair.first == MBB)
        ToRemove.insert(Pair);
    }
    for (auto &Pair : ToRemove)
      Result.DeferralEdges.erase(Pair);

    LLVM_DEBUG(dump());
  }

  assert(Result.DeferralEdges.empty() && "Unexpected failure");
}
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/CompactOrder.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "compact-order"

char CompactOrder::ID = 0;
char &llvm::CompactOrderPassID = CompactOrder::ID;

SmallVector<CompactNode *, 4> CompactOrder::getSuccessors(CompactNode *Node) const {
  SmallVector<CompactNode *, 4> Successors;

  if (!Node->IsLoop) {
    const auto *MBB = Node->getBlock();

    auto *L = MLI->getLoopFor(MBB);
    MachineBasicBlock *Header = nullptr;
    SmallVector<MachineBasicBlock *, 4> Tmp;

    if (L) {
      Header = L->getHeader();
      L->getExitBlocks(Tmp);
    }

    DenseSet<MachineBasicBlock *> ExitBlocks(Tmp.begin(), Tmp.end());

    for (auto *Succ : MBB->successors()) {
      // If we're inside a loop and succ is an exit block, we
      // ignore this edge, since this edge will appear at the
      // loop node:
      if (L && ExitBlocks.contains(Succ))
        continue;
      // If we're visiting a backedge, we also ignore it:
      if (L && L->isLoopLatch(MBB) && Header == Succ)
        continue;
      Successors.insert(Successors.begin(), new CompactNode(Succ));
    }
  } else {
    const auto *L = Node->getLoop();
    SmallVector<MachineBasicBlock *, 4> ExitBlocks;
    L->getExitBlocks(ExitBlocks);

    for (auto *Ex : ExitBlocks) {
      Successors.push_back(new CompactNode(Ex));
    }
  }

  return Successors;
}

SmallVector<CompactNode *, 4> CompactOrder::postOrder(CompactNode *Entry) {
  std::function<void(CompactNode *, DenseSet<const MachineBasicBlock *> &,
                     SmallVectorImpl<CompactNode *> &)>
      Go = [&](auto M, auto &Visited, auto &Nodes) {
        Visited.insert(M->asBlock());

        for (auto N : getSuccessors(M)) {
          auto BB = N->asBlock();
          if (!Visited.contains(BB))
            Go(N, Visited, Nodes);
        }

        Nodes.push_back(M);
      };

  DenseSet<const MachineBasicBlock *> Visited;
  SmallVector<CompactNode *, 8> Nodes;

  Go(Entry, Visited, Nodes);
  return Nodes;
}

SmallVector<CompactNode *, 4> CompactOrder::compactOrder(CompactNode *Entry) {
  auto &DT = getAnalysis<MachineDominatorTree>();

  // A loop L is the immediate dominator of a node N (in the CCFG)
  // if N's immediate dominator (in the CFG) is part of L.
  auto IsIDom = [&](auto N, auto M) {
    bool IsLoop = N->IsLoop;

    auto BBN = N->asBlock();
    auto BBM = M->asBlock();
    auto IDom = DT.getNode(BBM)->getIDom();

    auto LN = MLI->getLoopFor(BBN);
    auto LIDom = MLI->getLoopFor(IDom->getBlock());

    return (IsLoop && LIDom && LN->contains(LIDom)) ||
           (IDom == DT.getNode(BBN));
  };

  // Topological sort is just reverse post-order.
  auto PostOrder = postOrder(Entry);
  auto It = PostOrder.rbegin();
  auto End = PostOrder.rend();

  std::function<void(SmallVector<CompactNode *>::reverse_iterator,
                     SmallVector<CompactNode *> &)>
      Go = [&](auto It, auto &Nodes) {
        auto N = *It;
        Nodes.push_back(N);

        if (N->IsLoop) {
          auto *H = new CompactNode(N->asBlock());
          auto LNodes = compactOrder(H);
          Nodes.insert(Nodes.end(), LNodes.begin(), LNodes.end());
        }

        ++It;
        while (It != End) {
          auto M = *It;
          if (IsIDom(N, M))
            Go(It, Nodes);
          ++It;
        }
      };

  SmallVector<CompactNode *> Nodes;
  Go(It, Nodes);

  return Nodes;
}

bool CompactOrder::runOnMachineFunction(MachineFunction &MF) {
  MLI = &getAnalysis<MachineLoopInfo>();
  Order.clear();
  LLVM_DEBUG(errs() << "Post order:\n");
  Order = postOrder(new CompactNode(&*MF.begin()));
  LLVM_DEBUG(dump());
  LLVM_DEBUG(errs() << "Compact order:\n");
  Order = compactOrder(new CompactNode(&*MF.begin()));
  LLVM_DEBUG(dump());
  return false;
}

void CompactOrder::print(raw_ostream &OS, const Module *) const {
  auto PrintNode = [](CompactNode *N) {
    auto *BB = N->asBlock();
    std::pair<std::string, std::string> Shape;

    if (N->IsLoop)
      Shape = {"(", ")"};
    else
      Shape = {"[", "]"};

    std::string BBName;
    llvm::raw_string_ostream OS(BBName);
    BB->printAsOperand(OS, false);

    llvm::errs() << Shape.first << BBName << Shape.second;
  };

  for (auto *M : Order) {
    PrintNode(M);
    llvm::errs() << ": { ";

    auto Succs = getSuccessors(M);
    for (size_t Idx = 0; Idx < Succs.size(); Idx++) {
      auto *N = Succs[Idx];
      PrintNode(N);
      if (Idx < Succs.size() - 1)
        llvm::errs() << ", ";
    }

    llvm::errs() << " }\n";
  }

  llvm::errs() << "\n";
}

CompactOrder::CompactOrder() : MachineFunctionPass(ID) {
  initializeCompactOrderPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(CompactOrder, DEBUG_TYPE, "Compact Order", true, true)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfo)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTree)
INITIALIZE_PASS_END(CompactOrder, DEBUG_TYPE, "Compact Order", true, true)

namespace llvm {

FunctionPass *createCompactOrderPass() { return new CompactOrder(); }

} // namespace llvm

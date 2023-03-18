#ifndef LLVM_CODEGEN_AMI_LINEARIZATION_H
#define LLVM_CODEGEN_AMI_LINEARIZATION_H

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineDominanceFrontier.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/Passes.h"

using namespace llvm;

namespace llvm {

struct ActivatingRegion {
  MachineBasicBlock *Entry;
  MachineBasicBlock *Exit;
  SmallPtrSet<MachineBasicBlock *, 8> Blocks;

  ActivatingRegion(MachineBasicBlock *Entry, MachineBasicBlock *Exit,
                   SmallPtrSet<MachineBasicBlock *, 8> Blocks)
      : Entry(Entry), Exit(Exit), Blocks(std::move(Blocks)) {}
};

class region_domtree_iterator
    : public df_iterator<DomTreeNodeBase<MachineBasicBlock> *> {
  using super = df_iterator<DomTreeNodeBase<MachineBasicBlock> *>;

public:
  using Self = region_domtree_iterator;
  using value_type = typename super::value_type;

  // Construct the begin iterator.
  region_domtree_iterator(MachineDominatorTree *MDT, MachineBasicBlock *Entry, MachineBasicBlock *Exit)
      : super(df_begin(MDT->getNode(Entry))) {
    // Mark the exit of the region as visited, so that the children of the
    // exit and the exit itself, i.e. the block outside the region will never
    // be visited.
    super::Visited.insert(MDT->getNode(Exit));
  }

  // Construct the end iterator.
  region_domtree_iterator()
      : super(
            df_end<value_type>((DomTreeNodeBase<MachineBasicBlock> *)nullptr)) {
  }

  /*implicit*/ region_domtree_iterator(super I) : super(I) {}
};

class AMiLinearizationAnalysis : public MachineFunctionPass {
public:
  using RegionSet = SmallPtrSet<MachineRegion *, 16>;
  using Edge = std::pair<MachineBasicBlock *, MachineBasicBlock *>;
  using EdgeSet = SmallSet<Edge, 16>;

private:
  RegionSet SensitiveRegions;
  SparseBitVector<128> HandledBlocks;
  SparseBitVector<128> SensitiveBlocks;
  SparseBitVector<128> SensitiveBranchBlocks;

  DenseMap<MachineBasicBlock *, RegionSet> RegionMap;
  EdgeSet GhostEdges;
  EdgeSet ActivatingEdges;
  DenseMap<Edge, ActivatingRegion> ActivatingRegions;

  TrackSecretsAnalysis *TSA;
  MachineDominatorTree *MDT;
  MachinePostDominatorTree *MPDT;
  MachineDominanceFrontier *MDF;
  MachineFunction *MF;
  bool IsSSA;

public:
  static char ID;

  AMiLinearizationAnalysis(bool IsSSA = true);

  void findActivatingRegionExitings(
      MachineBasicBlock *Entry, MachineBasicBlock *Target,
      SmallVectorImpl<MachineBasicBlock *> &Exitings,
      SmallPtrSetImpl<MachineBasicBlock *> &RegionBlocks);
  MachineBasicBlock *chooseUnconditionalSuccessor(
      MachineBasicBlock *MBB,
      iterator_range<std::vector<MachineBasicBlock *>::iterator> Choices);
  void linearizeBranch(MachineBasicBlock *MBB, MachineBasicBlock *UncondSucc);

  bool isRealSuccessor(MachineBasicBlock *From, MachineBasicBlock *To) {
    return (From->isSuccessor(To) && !ActivatingEdges.contains({From, To})) ||
           GhostEdges.contains({From, To});
  }

  void realSuccessors(MachineBasicBlock *MBB, SmallVectorImpl<MachineBasicBlock *> &Succs) {
    for (auto *Succ : MBB->successors()) {
      if (!ActivatingEdges.contains({MBB, Succ}))
        Succs.push_back(Succ);
    }

    for (auto &Ghost : GhostEdges) {
      if (Ghost.first == MBB)
        Succs.push_back(Ghost.second);
    }
  }

  iterator_range<region_domtree_iterator>
  regionDomTreeIterator(MachineBasicBlock *Entry, MachineBasicBlock *Exit) {
    return iterator_range<region_domtree_iterator>(
        region_domtree_iterator(MDT, Entry, Exit), region_domtree_iterator());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TrackSecretsAnalysis>();
    AU.addPreserved<TrackSecretsAnalysis>();
    AU.addRequired<MachineDominatorTree>();
    AU.addPreserved<MachineDominatorTree>();
    AU.addRequired<MachinePostDominatorTree>();
    AU.addPreserved<MachinePostDominatorTree>();
    AU.addRequired<MachineDominanceFrontier>();
    AU.addPreserved<MachineDominanceFrontier>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_AMI_LINEARIZATIOn_H

#ifndef LLVM_CODEGEN_SENSITIVE_REGION_H
#define LLVM_CODEGEN_SENSITIVE_REGION_H

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

struct SensitiveBranch {
  MachineBasicBlock *MBB = nullptr;
  MachineBasicBlock *FlowBlock = nullptr;
  SmallVector<MachineOperand> Cond;
  MachineRegion *ElseRegion = nullptr;
  MachineRegion *IfRegion = nullptr;

  SensitiveBranch(MachineBasicBlock *MBB) : MBB(MBB) {}

  SensitiveBranch(MachineBasicBlock *MBB, SmallVector<MachineOperand> Cond,
                  MachineRegion *TR, MachineRegion *FR)
      : MBB(MBB), Cond(Cond), ElseRegion(TR), IfRegion(FR) {}

  bool operator<(const SensitiveBranch &Other) const {
    return IfRegion->getDepth() < Other.IfRegion->getDepth();
  }

  bool operator>(const SensitiveBranch &Other) const {
    return IfRegion->getDepth() > Other.IfRegion->getDepth();
  }

  bool operator==(const SensitiveBranch &Other) const {
    return MBB == Other.MBB;
  }
};

template <> struct DenseMapInfo<SensitiveBranch> {
  static inline SensitiveBranch getEmptyKey() {
    return SensitiveBranch(DenseMapInfo<MachineBasicBlock *>::getEmptyKey(),
                           SmallVector<MachineOperand>(), nullptr, nullptr);
  }

  static inline SensitiveBranch getTombstoneKey() {
    return SensitiveBranch(DenseMapInfo<MachineBasicBlock *>::getTombstoneKey(),
                           SmallVector<MachineOperand>(), nullptr, nullptr);
  }

  static unsigned getHashValue(const SensitiveBranch &Key) {
    return DenseMapInfo<MachineBasicBlock *>().getHashValue(Key.MBB);
  }

  static bool isEqual(const SensitiveBranch &LHS, const SensitiveBranch &RHS) {
    return DenseMapInfo<MachineBasicBlock *>().isEqual(LHS.MBB, RHS.MBB);
  }
};

class region_domtree_iterator
    : public df_iterator<DomTreeNodeBase<MachineBasicBlock> *> {
  using super =
      df_iterator<DomTreeNodeBase<MachineBasicBlock> *>;

public:
  using Self = region_domtree_iterator;
  using value_type = typename super::value_type;

  // Construct the begin iterator.
  region_domtree_iterator(MachineDominatorTree *MDT, MachineRegion *MR)
      : super(df_begin(MDT->getNode(MR->getEntry()))) {
    // Mark the exit of the region as visited, so that the children of the
    // exit and the exit itself, i.e. the block outside the region will never
    // be visited.
    super::Visited.insert(MDT->getNode(MR->getExit()));
  }

  // Construct the end iterator.
  region_domtree_iterator() : super(df_end<value_type>((DomTreeNodeBase<MachineBasicBlock> *)nullptr)) {}

  /*implicit*/ region_domtree_iterator(super I) : super(I) {}
};

class SensitiveRegionAnalysis : public MachineFunctionPass {
public:
  using BranchSet = SmallVector<SensitiveBranch, 16>;
  using RegionSet = SmallPtrSet<MachineRegion *, 16>;

private:
  RegionSet SensitiveRegions;
  SparseBitVector<128> HandledBlocks;
  SparseBitVector<128> SensitiveBlocks;
  SparseBitVector<128> SensitiveBranchBlocks;
  DenseMap<MachineBasicBlock *, BranchSet> IfBranchMap;
  DenseMap<MachineBasicBlock *, BranchSet> ElseBranchMap;
  BranchSet SensitiveBranches;
  MachineRegionInfo *MRI = nullptr;
  TrackSecretsAnalysis *TSA;
  MachineDominatorTree *MDT;
  MachinePostDominatorTree *MPDT;
  MachineDominanceFrontier *MDF;
  bool IsSSA;

public:
  static char ID;

  MachineRegionInfo *getRegionInfo() { return MRI; }

  iterator_range<BranchSet::iterator> sensitive_branches() {
    return make_range(SensitiveBranches.begin(), SensitiveBranches.end());
  }

  iterator_range<BranchSet::iterator> sensitive_branches(MachineBasicBlock *MBB,
                                                         bool InElseRegion) {
    BranchSet *Branches;

    if (InElseRegion) {
      Branches = &ElseBranchMap[MBB];
    } else {
      Branches = &IfBranchMap[MBB];
    }

    return make_range(Branches->begin(), Branches->end());
  }

  void insertBranchInBlockMap(MachineBasicBlock *MBB, SensitiveBranch &Branch,
                              bool InElseRegion) {
    if (!InElseRegion) {
      IfBranchMap[MBB].push_back(Branch);
    } else {
      ElseBranchMap[MBB].push_back(Branch);
    }
  }

  MachineRegion *getSensitiveRegion(MachineBasicBlock *MBB) {
    MachineRegion *Current = nullptr;
    unsigned int CurrentDepth = 0;

    for (auto &Branch : ElseBranchMap[MBB]) {
      if (Branch.ElseRegion->getDepth() > CurrentDepth) {
        CurrentDepth = Branch.ElseRegion->getDepth();
        Current = Branch.ElseRegion;
      }
    }

    for (auto &Branch : IfBranchMap[MBB]) {
      if (Branch.IfRegion->getDepth() > CurrentDepth) {
        CurrentDepth = Branch.IfRegion->getDepth();
        Current = Branch.IfRegion;
      }
    }

    return Current;
  }

  iterator_range<RegionSet::iterator> sensitive_regions() {
    return make_range(SensitiveRegions.begin(), SensitiveRegions.end());
  }

  bool isSensitive(const MachineRegion *MR) const {
    return SensitiveRegions.contains(MR);
  }

  bool isSensitive(const MachineBasicBlock *MBB) const {
    return SensitiveBlocks.test(MBB->getNumber());
  }

  iterator_range<region_domtree_iterator> regionDomTreeIterator(MachineRegion *MR) {
    return iterator_range<region_domtree_iterator>(region_domtree_iterator(MDT, MR), region_domtree_iterator());
  }

  SensitiveRegionAnalysis(bool IsSSA = true);

  void addBranch(SensitiveBranch Branch);
  void removeBranch(MachineBasicBlock *MBB);
  void handleRegion(MachineRegion *MR);
  void handleBranch(MachineBasicBlock *MBB, MachineRegion *Parent = nullptr);

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

#endif // LLVM_CODEGEN_SENSITIVE_REGION_H

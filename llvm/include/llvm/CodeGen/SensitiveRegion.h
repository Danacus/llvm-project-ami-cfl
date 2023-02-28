#ifndef LLVM_CODEGEN_SENSITIVE_REGION_H
#define LLVM_CODEGEN_SENSITIVE_REGION_H

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
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

class SensitiveRegionAnalysis : public MachineFunctionPass {
public:
  using BranchSet = SmallVector<SensitiveBranch, 16>;
  using RegionSet = SmallPtrSet<MachineRegion *, 16>;

private:
  RegionSet SensitiveRegions;
  SparseBitVector<128> SensitiveBlocks;
  DenseMap<MachineBasicBlock *, BranchSet> IfBranchMap;
  DenseMap<MachineBasicBlock *, BranchSet> ElseBranchMap;
  BranchSet SensitiveBranches;
  MachineRegionInfo *MRI;
  TrackSecretsAnalysis *TSA;
  bool IsSSA;

public:
  static char ID;

  iterator_range<BranchSet::iterator> sensitive_branches() {
    return make_range(SensitiveBranches.begin(), SensitiveBranches.end());
  }

  iterator_range<BranchSet::iterator>
  sensitive_branches(MachineBasicBlock *MBB, bool InElseRegion) {
    BranchSet *Branches;

    if (InElseRegion) {
      Branches = &ElseBranchMap[MBB];
    } else {
      Branches = &IfBranchMap[MBB];
    }

    return make_range(Branches->begin(), Branches->end());
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

  SensitiveRegionAnalysis(bool IsSSA = true);

  MachineRegion *getMaxRegionFor(MachineBasicBlock *MBB) const {
    // Get largest region that starts at BB. (See
    // RegionInfoBase::getMaxRegionExit)
    MachineRegion *FR = MRI->getRegionFor(MBB);
    assert(FR && "AMi error: no region for given block");
    while (auto *Expanded = FR->getExpandedRegion()) {
      FR = Expanded;
    }
    if (FR->getEntry() != MBB || !FR->getExit())
      llvm_unreachable("AMi error: unable to find activating region for "
                       "secret-dependent branch");
    while (FR && FR->getParent() && FR->getParent()->getEntry() == MBB &&
           FR->getExit())
      FR = FR->getParent();
    return FR;
  }

  void addBranch(SensitiveBranch Branch);
  void removeBranch(MachineBasicBlock *MBB);
  void handleBranch(MachineBasicBlock *MBB);

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineRegionInfoPass>();
    // AU.addPreserved<MachineRegionInfoPass>();
    AU.addRequired<TrackSecretsAnalysis>();
    // AU.addPreserved<TrackSecretsAnalysis>();
    // AU.setPreservesCFG();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_SENSITIVE_REGION_H

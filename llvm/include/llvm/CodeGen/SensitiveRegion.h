#ifndef LLVM_CODEGEN_SENSITIVE_REGION_H
#define LLVM_CODEGEN_SENSITIVE_REGION_H

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegionInfo.h"

using namespace llvm;

namespace llvm {

struct SensitiveBranch {
  MachineInstr *MI;
  SmallVector<MachineOperand> Cond;
  MachineRegion *ElseRegion;
  MachineRegion *IfRegion;

  SensitiveBranch(MachineInstr *MI, SmallVector<MachineOperand> Cond,
                  MachineRegion *TR, MachineRegion *FR)
      : MI(MI), Cond(Cond), ElseRegion(TR), IfRegion(FR) {}

  bool operator<(const SensitiveBranch &Other) const {
    return IfRegion->getDepth() < Other.IfRegion->getDepth();
  }

  bool operator>(const SensitiveBranch &Other) const {
    return IfRegion->getDepth() > Other.IfRegion->getDepth();
  }

  bool operator==(const SensitiveBranch &Other) const {
    return MI == Other.MI && ElseRegion == Other.ElseRegion && IfRegion == Other.IfRegion;
  }
};

template <> struct DenseMapInfo<SensitiveBranch> {
  using Tuple = std::tuple<MachineInstr *, MachineRegion *, MachineRegion *>;

  static inline SensitiveBranch getEmptyKey() {
    return SensitiveBranch(DenseMapInfo<MachineInstr *>::getEmptyKey(),
                           SmallVector<MachineOperand>(), nullptr, nullptr);
  }

  static inline SensitiveBranch getTombstoneKey() {
    return SensitiveBranch(DenseMapInfo<MachineInstr *>::getTombstoneKey(),
                           SmallVector<MachineOperand>(), nullptr, nullptr);
  }

  static unsigned getHashValue(const SensitiveBranch &Key) {
    return DenseMapInfo<Tuple>().getHashValue(
        {Key.MI, Key.IfRegion, Key.ElseRegion});
  }

  static bool isEqual(const SensitiveBranch &LHS, const SensitiveBranch &RHS) {
    return DenseMapInfo<Tuple>().isEqual(
        {LHS.MI, LHS.IfRegion, LHS.ElseRegion},
        {LHS.MI, LHS.IfRegion, LHS.ElseRegion});
  }
};

class SensitiveRegionAnalysisPass : public MachineFunctionPass {
public:
  using BranchSet = SmallSet<SensitiveBranch, 16>;
  using RegionSet = SmallPtrSet<MachineRegion *, 16>;

private:
  RegionSet SensitiveRegions;
  SparseBitVector<128> SensitiveBlocks;
  DenseMap<MachineBasicBlock *, BranchSet> IfBranchMap;
  DenseMap<MachineBasicBlock *, BranchSet> ElseBranchMap;
  BranchSet SensitiveBranches;
  MachineRegionInfo *MRI;

public:
  static char ID;

  // iterator_range<BranchVec::const_iterator> sensitive_branches() const {
  //   return make_range(SensitiveBranches.begin(), SensitiveBranches.end());
  // }

  iterator_range<BranchSet::const_iterator> sensitive_branches() {
    return make_range(SensitiveBranches.begin(), SensitiveBranches.end());
  }

  iterator_range<BranchSet::const_iterator>
  sensitive_branches(MachineBasicBlock *MBB, bool InElseRegion) {
    BranchSet Branches;

    if (InElseRegion) {
      Branches = ElseBranchMap[MBB];
    } else {
      Branches = IfBranchMap[MBB];
    }

    return make_range(Branches.begin(), Branches.end());
  }

  // iterator_range<RegionSet::const_iterator> sensitive_regions() const {
  //   return make_range(SensitiveRegions.begin(), SensitiveRegions.end());
  // }

  iterator_range<RegionSet::iterator> sensitive_regions() {
    return make_range(SensitiveRegions.begin(), SensitiveRegions.end());
  }

  bool isSensitive(const MachineRegion *MR) const {
    return SensitiveRegions.contains(MR);
  }

  bool isSensitive(const MachineBasicBlock *MBB) const {
    return SensitiveBlocks.test(MBB->getNumber());
  }

  SensitiveRegionAnalysisPass();

  MachineRegion *getMaxRegionFor(MachineBasicBlock *MBB) const {
    // Get largest region that starts at BB. (See
    // RegionInfoBase::getMaxRegionExit)
    MachineRegion *FR = MRI->getRegionFor(MBB);
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

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineRegionInfoPass>();
    AU.addRequiredTransitive<TrackSecretsAnalysisVirtReg>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_SENSITIVE_REGION_H

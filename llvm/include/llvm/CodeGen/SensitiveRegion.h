#ifndef LLVM_CODEGEN_SENSITIVE_REGION_H
#define LLVM_CODEGEN_SENSITIVE_REGION_H

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
};

class SensitiveRegionAnalysisPass : public MachineFunctionPass {
public:
  using BranchVec = SmallVector<SensitiveBranch>;
  using RegionSet = SmallPtrSet<MachineRegion *, 16>;
  
private:
  RegionSet SensitiveRegions;
  BranchVec SensitiveBranches;

public:
  static char ID;

  // iterator_range<BranchVec::const_iterator> sensitive_branches() const {
  //   return make_range(SensitiveBranches.begin(), SensitiveBranches.end());
  // }

  iterator_range<BranchVec::iterator> sensitive_branches() {
    return make_range(SensitiveBranches.begin(), SensitiveBranches.end());
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

  SensitiveRegionAnalysisPass();

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

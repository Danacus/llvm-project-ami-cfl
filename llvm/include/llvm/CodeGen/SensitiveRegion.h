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
private:
  SmallVector<SensitiveBranch> SensitiveBranches;

public:
  static char ID;

  const SmallVector<SensitiveBranch> &getSensitiveBranches() const {
    return SensitiveBranches;
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

#ifndef LLVM_CODEGEN_PERSISTENCY_ANALYSIS_H
#define LLVM_CODEGEN_PERSISTENCY_ANALYSIS_H

#include "llvm/CodeGen/AMiLinearizationAnalysisPCFL.h"
#include "llvm/CodeGen/MachineDominanceFrontier.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/SensitiveRegion.h"

using namespace llvm;

namespace llvm {

class PersistencyAnalysisPass : public MachineFunctionPass {
public:
  using RegionInstrMap =
      DenseMap<const ActivatingRegion *, SmallPtrSet<MachineInstr *, 16>>;

private:
  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;
  LinearizationResult *ALA;
  ReachingDefAnalysis *RDA;

  RegionInstrMap PersistentStores;
  RegionInstrMap CallInstructions;
  RegionInstrMap PersistentInstructions;
  RegionInstrMap PersistentRegionInputMap;

  bool IsSSA = true;

public:
  static char ID;

  PersistencyAnalysisPass(bool IsSSA = true);

  SmallPtrSet<MachineInstr *, 16>
  getPersistentInstructions(const ActivatingRegion *MR) {
    return PersistentInstructions[MR];
  }

  SmallPtrSet<MachineInstr *, 16> getPersistentStores(const ActivatingRegion *MR) {
    return PersistentStores[MR];
  }

  SmallPtrSet<MachineInstr *, 16> getCallInstrs(const ActivatingRegion *MR) {
    return CallInstructions[MR];
  }

  void propagatePersistency(const MachineFunction &MF, MachineInstr &MI,
                            const MachineOperand &MO,
                            const ActivatingRegion *MR);
  void analyzeRegion(const MachineFunction &MF, const ActivatingRegion *MR);

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AMiLinearizationAnalysis>();
    if (!IsSSA) {
      AU.addRequired<ReachingDefAnalysis>();
    }
    // AU.addRequiredTransitive<MachineRegionInfoPass>();
    AU.addRequiredTransitive<MachineDominatorTree>();
    AU.addRequiredTransitive<MachinePostDominatorTree>();
    AU.addRequiredTransitive<MachineDominanceFrontier>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_PERISTENCY_ANALYSIS_H

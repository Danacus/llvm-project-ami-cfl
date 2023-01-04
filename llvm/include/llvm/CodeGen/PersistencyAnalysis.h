#ifndef LLVM_CODEGEN_PERSISTENCY_ANALYSIS_H
#define LLVM_CODEGEN_PERSISTENCY_ANALYSIS_H

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/SensitiveRegion.h"

using namespace llvm;

namespace llvm {

class PersistencyAnalysisPass : public MachineFunctionPass {
public:
  using RegionInstrMap =
      DenseMap<const MachineRegion *, SmallPtrSet<MachineInstr *, 16>>;

private:
  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;
  SensitiveRegionAnalysisImpl *SRA;

  RegionInstrMap PersistentStores;
  RegionInstrMap PersistentInstructions;
  RegionInstrMap PersistentRegionInputMap;

public:
  static char ID;

  PersistencyAnalysisPass();

  SmallPtrSet<MachineInstr *, 16>
  getPersistentInstructions(const MachineRegion *MR) {
    return PersistentInstructions[MR];
  }

  SmallPtrSet<MachineInstr *, 16>
  getPersistentStores(const MachineRegion *MR) {
    return PersistentStores[MR];
  }

  void
  propagatePersistency(const MachineFunction &MF, const MachineInstr &MI,
                       const MachineOperand &MO, const MachineRegion &MR,
                       SmallPtrSet<MachineInstr *, 16> &PersistentDefs);
  void analyzeRegion(const MachineFunction &MF, const MachineRegion &MR,
                     const MachineRegion &Scope);
  void analyzeRegion(const MachineFunction &MF, const MachineRegion &MR) {
    analyzeRegion(MF, MR, MR);
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<SensitiveRegionAnalysisVirtReg>();
    AU.addRequiredTransitive<MachineRegionInfoPass>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_PERISTENCY_ANALYSIS_H

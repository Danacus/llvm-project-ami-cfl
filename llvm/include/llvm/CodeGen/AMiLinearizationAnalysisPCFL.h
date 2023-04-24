#ifndef LLVM_CODEGEN_AMI_LINEARIZATION_PCFL_H
#define LLVM_CODEGEN_AMI_LINEARIZATION_PCFL_H

#include "llvm/CodeGen/AMiLinearizationAnalysis.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/CompactOrder.h"
#include "llvm/CodeGen/TrackSecrets.h"
#include "llvm/CodeGen/MachineDominanceFrontier.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/Passes.h"

namespace llvm {

class LinearizationAnalysisPCFL : public LinearizationAnalysisBase {
public:
  using RegionSet = LinearizationResult::RegionSet;
  using Edge = LinearizationResult::Edge;
  using EdgeSet = LinearizationResult::EdgeSet;
    
private:
  MachineBasicBlock *nearestSuccessor(MachineBasicBlock *MBB);
  MachineBasicBlock *nearestDeferral(MachineBasicBlock *MBB);
  CompactOrder *CO;

public:
  void linearize() override;
  LinearizationAnalysisPCFL(CompactOrder *CO, TrackSecretsAnalysis *TSA,
                            MachineDominatorTree *MDT,
                            MachinePostDominatorTree *MPDT,
                            MachineDominanceFrontier *MDF, MachineFunction *MF,
                            bool AnalysisOnly)
      : LinearizationAnalysisBase(TSA, MDT, MPDT, MDF, MF, AnalysisOnly), CO(CO) {}
  ~LinearizationAnalysisPCFL() = default;
};

} // namespace llvm

#endif // LLVM_CODEGEN_AMI_LINEARIZATION_PCFL_H

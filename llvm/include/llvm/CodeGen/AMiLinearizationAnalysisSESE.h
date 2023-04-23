#ifndef LLVM_CODEGEN_AMI_LINEARIZATION_SESE_H
#define LLVM_CODEGEN_AMI_LINEARIZATION_SESE_H

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/AMiLinearizationAnalysis.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineDominanceFrontier.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/SensitiveRegion.h"

using namespace llvm;

namespace llvm {

class LinearizationAnalysisSESE : public LinearizationAnalysisBase {
public:
  using RegionSet = LinearizationResult::RegionSet;
  using Edge = LinearizationResult::Edge;
  using EdgeSet = LinearizationResult::EdgeSet;

private:
  void
  findActivatingRegionExitings(MachineBasicBlock *Entry,
                               MachineBasicBlock *Target,
                               SmallVectorImpl<MachineBasicBlock *> &Exitings);
  MachineBasicBlock *chooseUnconditionalSuccessor(
      MachineBasicBlock *MBB,
      iterator_range<std::vector<MachineBasicBlock *>::iterator> Choices);
  void linearizeBranch(MachineBasicBlock *MBB, MachineBasicBlock *UncondSucc,
                       MachineBasicBlock *Target);
  SensitiveRegionAnalysis *SRA;

public:
  void linearize() override;
  LinearizationAnalysisSESE(SensitiveRegionAnalysis *SRA, TrackSecretsAnalysis *TSA,
                            MachineDominatorTree *MDT,
                            MachinePostDominatorTree *MPDT,
                            MachineDominanceFrontier *MDF, MachineFunction *MF,
                            bool AnalysisOnly)
      : LinearizationAnalysisBase(TSA, MDT, MPDT, MDF, MF, AnalysisOnly), SRA(SRA) {}
  ~LinearizationAnalysisSESE() = default;
};

} // namespace llvm

#endif // LLVM_CODEGEN_AMI_LINEARIZATIOn_SESE_H

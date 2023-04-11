#ifndef LLVM_CODEGEN_AMI_LINEARIZATION_SESE_H
#define LLVM_CODEGEN_AMI_LINEARIZATION_SESE_H

#include "llvm/CodeGen/AMiLinearizationAnalysis.h"
#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/SparseBitVector.h"
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

class AMiLinearizationAnalysisSESE : public MachineFunctionPass {
public:
  using RegionSet = SmallPtrSet<ActivatingRegion *, 16>;
  using Edge = std::pair<MachineBasicBlock *, MachineBasicBlock *>;
  using EdgeSet = SmallSet<Edge, 16>;

private:
  TrackSecretsAnalysis *TSA;
  MachineDominatorTree *MDT;
  MachinePostDominatorTree *MPDT;
  MachineDominanceFrontier *MDF;
  MachineFunction *MF;
  bool AnalysisOnly;

public:
  SparseBitVector<128> SensitiveBranchBlocks;
  EdgeSet GhostEdges;
  EdgeSet UncondEdges;
  EdgeSet ActivatingEdges;
  DenseMap<Edge, ActivatingRegion> ActivatingRegions;
  DenseMap<MachineBasicBlock *, RegionSet> RegionMap;

  static char ID;

  AMiLinearizationAnalysisSESE(bool AnalysisOnly = true);

  void undoCFGChanges();
  void findSecretDependentBranches();
  void createActivatingRegions();
  void
  findActivatingRegionExitings(MachineBasicBlock *Entry,
                               MachineBasicBlock *Target,
                               SmallVectorImpl<MachineBasicBlock *> &Exitings);
  MachineBasicBlock *chooseUnconditionalSuccessor(
      MachineBasicBlock *MBB,
      iterator_range<std::vector<MachineBasicBlock *>::iterator> Choices);
  void linearizeBranch(MachineBasicBlock *MBB, MachineBasicBlock *UncondSucc, MachineBasicBlock *Target);

  iterator_range<bounded_domtree_iterator>
  regionDomTreeIterator(MachineBasicBlock *Entry, MachineBasicBlock *Exit) {
    return iterator_range<bounded_domtree_iterator>(
        bounded_domtree_iterator(MDT, Entry, Exit), bounded_domtree_iterator());
  }

  void print(raw_ostream &OS, const Module *) const override;

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
    AU.addRequired<SensitiveRegionAnalysis>();
    AU.addPreserved<SensitiveRegionAnalysis>();
    if (AnalysisOnly)
      AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_AMI_LINEARIZATIOn_SESE_H

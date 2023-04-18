#ifndef LLVM_CODEGEN_AMI_LINEARIZATION_H
#define LLVM_CODEGEN_AMI_LINEARIZATION_H

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/ADT/SparseBitVector.h"
#include "llvm/CodeGen/CompactOrder.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineDominanceFrontier.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/Passes.h"

using namespace llvm;

namespace llvm {

struct ActivatingRegion {
  using BlockSet = SmallPtrSet<MachineBasicBlock *, 8>;
  using iterator = BlockSet::iterator;
  using const_iterator = BlockSet::const_iterator;

  MachineBasicBlock *Branch;
  MachineBasicBlock *Entry;
  MachineBasicBlock *Exit;
  BlockSet Blocks;

  ActivatingRegion(const ActivatingRegion &) = delete;
  ActivatingRegion(ActivatingRegion &&) = default;
  ActivatingRegion &operator=(const ActivatingRegion &) = delete;
  ActivatingRegion &operator=(ActivatingRegion &&) = delete;
  ActivatingRegion(MachineBasicBlock *Branch, MachineBasicBlock *Entry,
                   MachineBasicBlock *Exit,
                   SmallPtrSet<MachineBasicBlock *, 8> Blocks)
      : Branch(Branch), Entry(Entry), Exit(Exit), Blocks(std::move(Blocks)) {}

  iterator blocks_begin() { return Blocks.begin(); }
  iterator blocks_end() { return Blocks.end(); }
  iterator_range<iterator> blocks() {
    return make_range(blocks_begin(), blocks_end());
  }

  const_iterator blocks_begin() const { return Blocks.begin(); }
  const_iterator blocks_end() const { return Blocks.end(); }
  iterator_range<const_iterator> blocks() const {
    return make_range(blocks_begin(), blocks_end());
  }

  bool contains(MachineBasicBlock *MBB) const { return Blocks.contains(MBB); }

  bool contains(MachineInstr *MI) const { return contains(MI->getParent()); }

  void print(raw_ostream &OS) const {
    OS << "<";
    if (Branch) {
      Branch->printAsOperand(OS);
      OS << " " << Branch->getName();
    } else {
      OS << "entry";
    }
    OS << ", ";
    if (Exit) {
      Exit->printAsOperand(OS);
      OS << " " << Exit->getName();
    } else {
      OS << "exit";
    }
    OS << ">\n";

    for (auto *Block : Blocks) {
      Block->printAsOperand(OS);
      OS << " " << Block->getName();
      OS << "\n";
    }
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { print(dbgs()); }
#endif
};

class bounded_domtree_iterator
    : public df_iterator<DomTreeNodeBase<MachineBasicBlock> *> {
  using super = df_iterator<DomTreeNodeBase<MachineBasicBlock> *>;

public:
  using Self = bounded_domtree_iterator;
  using value_type = typename super::value_type;

  // Construct the begin iterator.
  bounded_domtree_iterator(MachineDominatorTree *MDT, MachineBasicBlock *Entry,
                           MachineBasicBlock *Exit)
      : super(df_begin(MDT->getNode(Entry))) {
    // Mark the exit of the region as visited, so that the children of the
    // exit and the exit itself, i.e. the block outside the region will never
    // be visited.
    super::Visited.insert(MDT->getNode(Exit));
  }

  // Construct the end iterator.
  bounded_domtree_iterator()
      : super(
            df_end<value_type>((DomTreeNodeBase<MachineBasicBlock> *)nullptr)) {
  }

  /*implicit*/ bounded_domtree_iterator(super I) : super(I) {}
};

class AMiLinearizationAnalysis : public MachineFunctionPass {
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
  DenseMap<MachineBasicBlock *, unsigned int> BlockIndex;
  SmallVector<MachineBasicBlock *> Blocks;

public:
  SparseBitVector<128> SensitiveBranchBlocks;
  EdgeSet GhostEdges;
  EdgeSet DeferralEdges;
  EdgeSet UncondEdges;
  EdgeSet ActivatingEdges;
  DenseMap<Edge, ActivatingRegion> ActivatingRegions;
  DenseMap<MachineBasicBlock *, RegionSet> RegionMap;

  static char ID;

  AMiLinearizationAnalysis(bool AnalysisOnly = true);

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
  void linearize();
  MachineBasicBlock *nearestSuccessor(MachineBasicBlock *MBB);
  MachineBasicBlock *nearestDeferral(MachineBasicBlock *MBB);

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
    AU.addRequired<CompactOrder>();
    if (AnalysisOnly)
      AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_AMI_LINEARIZATIOn_H

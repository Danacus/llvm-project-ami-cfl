#ifndef LLVM_CODEGEN_AMI_LINEARIZATION_H
#define LLVM_CODEGEN_AMI_LINEARIZATION_H

#include "llvm/CodeGen/CompactOrder.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominanceFrontier.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/Support/GenericDomTreeConstruction.h"


using namespace llvm;

namespace llvm {

enum LinearizationMethod {
  ALM_PCFL = 0,    
  ALM_SESE,    
};

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

struct LinearizationResult {
  using RegionSet = SmallPtrSet<ActivatingRegion *, 16>;
  using Edge = std::pair<MachineBasicBlock *, MachineBasicBlock *>;
  using EdgeSet = SmallSet<Edge, 16>;
  using BlockSet = SmallPtrSet<MachineBasicBlock *, 16>;

  SparseBitVector<128> SensitiveBranchBlocks;
  EdgeSet GhostEdges;
  EdgeSet DeferralEdges;
  EdgeSet ActivatingEdges;
  DenseMap<Edge, ActivatingRegion> ActivatingRegions;
  DenseMap<MachineBasicBlock *, RegionSet> RegionMap;
  DenseMap<MachineBasicBlock *, BlockSet> OutgoingActivatingEdges;
  DenseMap<MachineBasicBlock *, BlockSet> OutgoingGhostEdges;

  void clear() {
    SensitiveBranchBlocks.clear();
    GhostEdges.clear();
    DeferralEdges.clear();
    ActivatingEdges.clear();
    ActivatingRegions.clear();
    RegionMap.clear();
    OutgoingActivatingEdges.clear();
    OutgoingGhostEdges.clear();
  }

  void print(raw_ostream &OS) const {
    OS << "Ghost edges:\n";

    for (auto &Edge : GhostEdges) {
      OS << "<";
      Edge.first->printAsOperand(OS);
      OS << " " << Edge.first->getName();
      OS << ", ";
      Edge.second->printAsOperand(OS);
      OS << " " << Edge.second->getName();
      OS << ">\n";
    }

    OS << "Activating edges:\n";

    for (auto &Edge : ActivatingEdges) {
      OS << "<";
      Edge.first->printAsOperand(OS);
      OS << " " << Edge.first->getName();
      OS << ", ";
      Edge.second->printAsOperand(OS);
      OS << " " << Edge.second->getName();
      OS << ">\n";
    }

    if (DeferralEdges.size() > 0) {
      OS << "Deferral edges:\n";

      for (auto &Edge : DeferralEdges) {
        OS << "<";
        Edge.first->printAsOperand(OS);
        OS << " " << Edge.first->getName();
        OS << ", ";
        Edge.second->printAsOperand(OS);
        OS << " " << Edge.second->getName();
        OS << ">\n";
      }
    }

    OS << "----------------------\n";

    OS << "Activating regions:\n";

    for (auto &Pair : ActivatingRegions) {
      Pair.getSecond().print(OS);
      OS << "------------\n";
    }
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { print(dbgs()); }
#endif
};

} // namespace llvm

class LinearizationAnalysisBase {
protected:
  TrackSecretsAnalysis *TSA;
  MachineDominatorTree *MDT;
  MachinePostDominatorTree *MPDT;
  MachineDominanceFrontier *MDF;
  MachineFunction *MF;
  bool AnalysisOnly;
  DenseMap<MachineBasicBlock *, unsigned int> BlockIndex;
  SmallVector<MachineBasicBlock *> Blocks;
  LinearizationResult Result;

private:
  void undoCFGChanges();
  void findSecretDependentBranches();
  void createActivatingRegions();
  void updateEdgeMaps();

protected:
  iterator_range<bounded_domtree_iterator>
  regionDomTreeIterator(MachineBasicBlock *Entry, MachineBasicBlock *Exit) {
    return iterator_range<bounded_domtree_iterator>(
        bounded_domtree_iterator(MDT, Entry, Exit), bounded_domtree_iterator());
  }

  virtual void linearize() = 0;

public:
  LinearizationAnalysisBase(const LinearizationAnalysisBase &) = default;
  LinearizationAnalysisBase(LinearizationAnalysisBase &&) = default;
  LinearizationAnalysisBase &
  operator=(const LinearizationAnalysisBase &) = default;
  LinearizationAnalysisBase &operator=(LinearizationAnalysisBase &&) = default;
  LinearizationAnalysisBase(TrackSecretsAnalysis *TSA,
                            MachineDominatorTree *MDT,
                            MachinePostDominatorTree *MPDT,
                            MachineDominanceFrontier *MDF, MachineFunction *MF,
                            bool AnalysisOnly)
      : TSA(TSA), MDT(MDT), MPDT(MPDT), MDF(MDF), MF(MF),
        AnalysisOnly(AnalysisOnly) {}
  virtual ~LinearizationAnalysisBase() = default;

  LinearizationResult &getResult() {
    return Result;
  }

  bool run();

  void print(raw_ostream &OS) const {
    Result.print(OS);
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { print(dbgs()); }
#endif
};

class AMiLinearizationAnalysis : public MachineFunctionPass {
public:
  using RegionSet = LinearizationResult::RegionSet;
  using Edge = LinearizationResult::Edge;
  using EdgeSet = LinearizationResult::EdgeSet;

private:
  LinearizationAnalysisBase *Analysis = nullptr;
  bool AnalysisOnly;
  LinearizationMethod Method;

public:
  static char ID;

  AMiLinearizationAnalysis(bool AnalysisOnly = true, LinearizationMethod Method = ALM_PCFL);

  LinearizationResult &getResult() {
    return Analysis->getResult();
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
    if (Method == ALM_PCFL)
      AU.addRequired<CompactOrder>();
    if (Method == ALM_SESE)
      AU.addRequired<SensitiveRegionAnalysis>();
    if (AnalysisOnly)
      AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

#endif

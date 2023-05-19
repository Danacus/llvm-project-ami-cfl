#ifndef LLVM_CODEGEN_TRACKSECRETS_H
#define LLVM_CODEGEN_TRACKSECRETS_H

#include "llvm/ADT/iterator_range.h"
#include "llvm/CodeGen/ControlDependenceGraph.h"
#include "llvm/CodeGen/LiveIntervals.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/Support/DOTGraphTraits.h"

using namespace llvm;

namespace llvm {

class FlowGraphNodeInner {
public:
  enum NodeKind {
    FGNK_Argument,
    FGNK_Global,
    FGNK_SecretRegisterDef,
    FGNK_SecretRegisterUse,
    FGNK_SecretGlobalUse,
    FGNK_ControlDep,
    FGNK_Root,
    FGNK_Empty,
    FGNK_Tombstone,
  };

private:
  GlobalVariable *GlobalVar;
  Register Reg;
  MachineInstr *MI;
  NodeKind Kind;

public:
  FlowGraphNodeInner(NodeKind Kind) : Kind(Kind) {}

  static FlowGraphNodeInner CreateRoot() {
    auto Def = FlowGraphNodeInner(FGNK_Root);
    return Def;
  }

  static FlowGraphNodeInner CreateArgument(Register Reg) {
    auto Def = FlowGraphNodeInner(FGNK_Argument);
    Def.setReg(Reg);
    return Def;
  }

  static FlowGraphNodeInner CreateGlobal(GlobalVariable *GV) {
    auto Def = FlowGraphNodeInner(FGNK_Global);
    Def.setGlobalVariable(GV);
    return Def;
  }

  static FlowGraphNodeInner CreateRegisterDef(Register Reg, MachineInstr *MI) {
    auto Def = FlowGraphNodeInner(FGNK_SecretRegisterDef);
    Def.setReg(Reg);
    Def.setMI(MI);
    return Def;
  }

  static FlowGraphNodeInner CreateRegisterUse(Register Reg, MachineInstr *MI) {
    auto Def = FlowGraphNodeInner(FGNK_SecretRegisterUse);
    Def.setReg(Reg);
    Def.setMI(MI);
    return Def;
  }

  static FlowGraphNodeInner CreateGlobalUse(GlobalVariable *GV,
                                            MachineInstr *MI) {
    auto Def = FlowGraphNodeInner(FGNK_SecretGlobalUse);
    Def.setGlobalVariable(GV);
    Def.setMI(MI);
    return Def;
  }

  static FlowGraphNodeInner CreateControlDep(Register Reg, MachineInstr *MI) {
    auto Def = FlowGraphNodeInner(FGNK_ControlDep);
    Def.setReg(Reg);
    Def.setMI(MI);
    return Def;
  }

  NodeKind getKind() const { return Kind; }

  bool operator==(const FlowGraphNodeInner &Other) const {
    if (getKind() != Other.getKind())
      return false;

    switch (getKind()) {
    case FGNK_Argument:
      return getReg() == Other.getReg();
    case FGNK_Global:
      return getGlobalVariable() == Other.getGlobalVariable();
    case FGNK_SecretRegisterDef:
    case FGNK_SecretRegisterUse:
    case FGNK_ControlDep:
      return getReg() == Other.getReg() && getMI() == Other.getMI();
    case FGNK_SecretGlobalUse:
      return getGlobalVariable() == Other.getGlobalVariable() &&
             Other.getMI() == getMI();
    default:
      return true;
    }
  }

  bool operator<(const FlowGraphNodeInner &Other) const {
    return std::tie(GlobalVar, Reg, MI) <
           std::tie(Other.GlobalVar, Other.Reg, Other.MI);
  }

  bool hasReg() const {
    return getKind() == FGNK_Argument || getKind() == FGNK_SecretRegisterDef ||
           getKind() == FGNK_SecretRegisterUse || getKind() == FGNK_ControlDep;
  }

  Register getReg() const {
    assert(hasReg());
    return Reg;
  }

  MachineInstr *getMI() const {
    assert(isRegisterDef() || isRegisterUse() || isGlobalUse() ||
           isControlDep());
    return MI;
  }

  GlobalVariable *getGlobalVariable() const {
    assert(isGlobal() || isGlobalUse());
    return GlobalVar;
  }

  void setReg(Register R) {
    assert(hasReg());
    Reg = R;
  }

  void setMI(MachineInstr *I) {
    assert(isRegisterDef() || isRegisterUse() || isGlobalUse() ||
           isControlDep());
    MI = I;
  }

  void setGlobalVariable(GlobalVariable *GV) {
    assert(isGlobal() || isGlobalUse());
    GlobalVar = GV;
  }

  bool isArgument() const { return getKind() == FGNK_Argument; }

  bool isGlobal() const { return getKind() == FGNK_Global; }

  bool isRegisterDef() const { return getKind() == FGNK_SecretRegisterDef; }

  bool isRegisterUse() const { return getKind() == FGNK_SecretRegisterUse; }

  bool isGlobalUse() const { return getKind() == FGNK_SecretGlobalUse; }

  bool isControlDep() const { return getKind() == FGNK_ControlDep; }

  unsigned getHashValue() const {
    switch (getKind()) {
    case FGNK_Argument:
      return hash_combine(getKind(), getReg());
    case FGNK_SecretRegisterDef:
    case FGNK_SecretRegisterUse:
    case FGNK_ControlDep:
      return hash_combine(getKind(), getReg(), getMI());
    case FGNK_Global:
      return hash_combine(getKind(), getGlobalVariable());
    case FGNK_SecretGlobalUse:
      return hash_combine(getKind(), getGlobalVariable(), getMI());
    default:
      return hash_combine(getKind());
    }
  }

  StringRef getKindLabel() const {
    switch (getKind()) {
    case FGNK_Root:
      return "Root";
    case FGNK_Argument:
      return "Argument";
    case FGNK_SecretRegisterDef:
      return "RegisterDef";
    case FGNK_SecretRegisterUse:
      return "RegisterUse";
    case FGNK_SecretGlobalUse:
      return "GlobalUse";
    case FGNK_ControlDep:
      return "ControlDep";
    case FGNK_Global:
      return "Global";
    default:
      return "UNKNOWN";
    }
  }

  void print(raw_ostream &OS) const {
    OS << getKindLabel();

    OS << " ";

    switch (getKind()) {
    case FGNK_Argument:
      OS << printReg(getReg());
      break;
    case FGNK_SecretRegisterDef:
    case FGNK_SecretRegisterUse:
    case FGNK_ControlDep:
      OS << printReg(getReg());
      OS << "\n";
      getMI()->print(OS, true, false, false, false);
      break;
    case FGNK_SecretGlobalUse:
      OS << getGlobalVariable();
      OS << "\n";
      getMI()->print(OS, true, false, false, false);
      break;
    case FGNK_Global:
      OS << getGlobalVariable();
      break;
    default:
      break;
    }
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { print(dbgs()); }
#endif
};

class FlowGraphNode {
public:
  typedef SmallSet<FlowGraphNode *, 4>::iterator node_iterator;
  typedef SmallSet<FlowGraphNode *, 4>::const_iterator const_node_iterator;

private:
  FlowGraphNodeInner Inner;
  bool Visited = false;

  SmallSet<FlowGraphNode *, 4> Preds;
  SmallSet<FlowGraphNode *, 4> Succs;

public:
  FlowGraphNode(FlowGraphNodeInner Inner) : Inner(Inner) {}

  bool isVisited() { return Visited; }

  void setVisited(bool Visited = true) { this->Visited = Visited; }

  void clearSuccs() { Succs.clear(); }

  void clearPreds() { Preds.clear(); }

  FlowGraphNodeInner &inner() { return Inner; }

  node_iterator begin() { return Succs.begin(); }
  node_iterator end() { return Succs.end(); }
  iterator_range<node_iterator> successors() {
    return make_range(begin(), end());
  }

  node_iterator pred_begin() { return Preds.begin(); }
  node_iterator pred_end() { return Preds.end(); }
  const_node_iterator pred_begin() const { return Preds.begin(); }
  const_node_iterator pred_end() const { return Preds.end(); }

  void addSucc(FlowGraphNode *Succ) { Succs.insert(Succ); }

  void addPred(FlowGraphNode *Pred) { Preds.insert(Pred); }

  static FlowGraphNode *CreateRoot() {
    auto *Def = new FlowGraphNode(FlowGraphNodeInner::CreateRoot());
    return Def;
  }

  static FlowGraphNode *CreateArgument(Register Reg) {
    auto *Def = new FlowGraphNode(FlowGraphNodeInner::CreateArgument(Reg));
    return Def;
  }

  static FlowGraphNode *CreateGlobal(GlobalVariable *GV) {
    auto *Def = new FlowGraphNode(FlowGraphNodeInner::CreateGlobal(GV));
    return Def;
  }

  static FlowGraphNode *CreateRegisterDef(Register Reg, MachineInstr *MI) {
    auto *Def =
        new FlowGraphNode(FlowGraphNodeInner::CreateRegisterDef(Reg, MI));
    return Def;
  }

  static FlowGraphNode *CreateRegisterUse(Register Reg, MachineInstr *MI) {
    auto *Def =
        new FlowGraphNode(FlowGraphNodeInner::CreateRegisterUse(Reg, MI));
    return Def;
  }

  static FlowGraphNode *CreateGlobalUse(GlobalVariable *GV, MachineInstr *MI) {
    auto *Def = new FlowGraphNode(FlowGraphNodeInner::CreateGlobalUse(GV, MI));
    return Def;
  }

  static FlowGraphNode *CreateControlDep(Register Reg, MachineInstr *MI) {
    auto *Def =
        new FlowGraphNode(FlowGraphNodeInner::CreateControlDep(Reg, MI));
    return Def;
  }

  void print(raw_ostream &OS) const { Inner.print(OS); }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { print(dbgs()); }
#endif
};

template <> struct GraphTraits<FlowGraphNode *> {
  using NodeRef = FlowGraphNode *;
  typedef FlowGraphNode::node_iterator ChildIteratorType;

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static inline ChildIteratorType child_begin(NodeRef N) { return N->begin(); }
  static inline ChildIteratorType child_end(NodeRef N) { return N->end(); }

  typedef df_iterator<FlowGraphNode *> nodes_iterator;

  static nodes_iterator nodes_begin(NodeRef N) {
    return df_begin(getEntryNode(N));
  }
  static nodes_iterator nodes_end(NodeRef N) { return df_end(getEntryNode(N)); }
};

template <> struct DenseMapInfo<FlowGraphNodeInner> {
  static inline FlowGraphNodeInner getEmptyKey() {
    return FlowGraphNodeInner(FlowGraphNodeInner::FGNK_Empty);
  }

  static inline FlowGraphNodeInner getTombstoneKey() {
    return FlowGraphNodeInner(FlowGraphNodeInner::FGNK_Tombstone);
  }

  static unsigned getHashValue(const FlowGraphNodeInner &Key) {
    return Key.getHashValue();
  }

  static bool isEqual(const FlowGraphNodeInner &LHS,
                      const FlowGraphNodeInner &RHS) {
    return LHS == RHS;
  }
};

class FlowGraph {
public:
  using const_iterator = SmallSet<FlowGraphNode *, 4>::const_iterator;
  using Key = FlowGraphNodeInner;
  DenseMap<Key, uint64_t> SecretMasks;

private:
  DenseMap<Key, FlowGraphNode *> Nodes;
  FlowGraphNode *Root;

  void createEdge(FlowGraphNode *From, FlowGraphNode *To) {
    From->addSucc(To);
    To->addPred(From);
  }

  bool isLiveAt(Register Reg, MachineBasicBlock *MBB, LiveVariables *LV) {
    if (LV) {
      // auto &LI = LIS->getInterval(Reg);
      // LI.dump();
      // auto IsLive = LIS->isLiveInToMBB(LI, MBB);
      auto IsLive = Reg.isVirtual() && LV->isLiveIn(Reg, *MBB);
      return IsLive;
    }

    assert(Reg.isPhysical() &&
           "expected Reg to be physical is LIS is unavailable");
    return MBB->isLiveIn(Reg.asMCReg());
  }

  void handleControlDep(MachineInstr &BranchMI, ControlDependenceGraph *CDG,
                        MachinePostDominatorTree *MPDT, LiveVariables *LV,
                        const TargetInstrInfo *TII, Register DepReg,
                        SmallSet<FlowGraphNode *, 8> &Nodes);

public:
  FlowGraph(MachineFunction &MF, ReachingDefAnalysis *RDA = nullptr,
            MachineDominatorTree *MDT = nullptr,
            MachinePostDominatorTree *MPDT = nullptr,
            ControlDependenceGraph *CFG = nullptr,
            LiveVariables *LV = nullptr);
  ~FlowGraph();

  DenseMap<Key, uint64_t> &compute(const TargetInstrInfo *TII,
                                   const TargetRegisterInfo *TRI);
  void getSecretUses(SmallPtrSetImpl<MachineInstr *> &Uses);
  FlowGraphNode *getOrInsert(FlowGraphNode *&&Node) {
    if (Nodes.count(Node->inner())) {
      // TODO: Fix this?
      // if (Nodes[Node->inner()] != Node)
      //   delete Node;
      return Nodes[Node->inner()];
    } else {
      Nodes[Node->inner()] = Node;
      return Node;
    }
  }
  void getSources(MachineFunction &MF, ReachingDefAnalysis *RDA,
                  SmallSet<FlowGraphNode *, 8> &SecretDefs,
                  DenseMap<FlowGraph::Key, uint64_t> &SecretMasks);

  const_iterator succ_begin(Key Node) { return Nodes[Node]->begin(); }
  const_iterator succ_end(Key Node) { return Nodes[Node]->end(); }
  iterator_range<const_iterator> successors(Key Node) {
    return make_range(succ_begin(Node), succ_end(Node));
  }

  FlowGraphNode *getRoot() { return Root; }

  void setRoot(FlowGraphNode *Root) { this->Root = Root; }

  void print(raw_ostream &OS) const {
    OS << "FlowGraph:\n";
    for (auto &Pair : Nodes) {
      Pair.getFirst().print(OS);
      OS << " -> {\n";
      for (auto *T : Pair.getSecond()->successors()) {
        OS << "\t";
        T->print(OS);
        OS << "\n";
      }
      OS << "}\n";
    }

    OS << "Secret Masks:\n";
    for (auto &Pair : SecretMasks) {
      Pair.getFirst().print(OS);
      OS << ": ";
      OS << Pair.getSecond();
      OS << "\n";
    }
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { print(dbgs()); }
#endif
};

template <>
struct GraphTraits<FlowGraph *> : public GraphTraits<FlowGraphNode *> {
  static NodeRef getEntryNode(FlowGraph *FG) { return FG->getRoot(); }

  static nodes_iterator nodes_begin(FlowGraph *FG) {
    if (getEntryNode(FG))
      return df_begin(getEntryNode(FG));
    else
      return df_end(getEntryNode(FG));
  }

  static nodes_iterator nodes_end(FlowGraph *FG) {
    return df_end(getEntryNode(FG));
  }
};

template <> struct DOTGraphTraits<FlowGraph *> : public DefaultDOTGraphTraits {
  DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getGraphName(FlowGraph *Graph) { return "FlowGraph"; }

  std::string getNodeLabel(FlowGraphNode *Node, FlowGraph *Graph) {
    std::string Label;
    raw_string_ostream OS = raw_string_ostream(Label);
    Node->print(OS);
    return Label;
  }

  std::string getNodeAttributes(FlowGraphNode *Node, FlowGraph *Graph) {
    if (Graph->SecretMasks[Node->inner()]) {
      std::string Attrs = "style=filled, fillcolor=\"red\"";
      return Attrs;
    } else {
      return DefaultDOTGraphTraits::getNodeAttributes(Node, Graph);
    }
  }
};

class TrackSecretsAnalysis : public MachineFunctionPass {
public:
  static char ID;

  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;

  SmallPtrSet<MachineInstr *, 8> SecretUses;

  FlowGraph *getGraph() { return Graph; }

  TrackSecretsAnalysis(bool IsSSA = true);
  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    if (!IsSSA) {
      AU.addRequired<ReachingDefAnalysis>();
      AU.addRequired<MachineDominatorTree>();
    } else {
      AU.addRequired<LiveVariables>();
    }
    AU.addRequired<MachinePostDominatorTree>();
    AU.addRequired<ControlDependenceGraph>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  FlowGraph *Graph = nullptr;
  bool IsSSA;
};

class FlowGraphPrinter : public MachineFunctionPass {
public:
  static char ID;

  FlowGraphPrinter();

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TrackSecretsAnalysis>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif

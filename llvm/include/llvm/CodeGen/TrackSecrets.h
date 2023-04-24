#ifndef LLVM_CODEGEN_TRACKSECRETS_H
#define LLVM_CODEGEN_TRACKSECRETS_H

#include "llvm/ADT/iterator_range.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"

using namespace llvm;

namespace llvm {

class FlowGraphNode {
public:
  enum NodeKind {
    FGNK_Argument,
    FGNK_Global,
    FGNK_SecretRegisterDef,
    FGNK_SecretRegisterUse,
    FGNK_SecretGlobalUse,
    FGNK_ControlDep,
    FGNK_Empty,
    FGNK_Tombstone,
  };

private:
  GlobalVariable *GlobalVar;
  Register Reg;
  MachineInstr *MI;
  NodeKind Kind;

public:
  FlowGraphNode(NodeKind Kind) : Kind(Kind) {}

  static FlowGraphNode CreateArgument(Register Reg) {
    auto Def = FlowGraphNode(FGNK_Argument);
    Def.setReg(Reg);
    return Def;
  }

  static FlowGraphNode CreateGlobal(GlobalVariable *GV) {
    auto Def = FlowGraphNode(FGNK_Global);
    Def.setGlobalVariable(GV);
    return Def;
  }

  static FlowGraphNode CreateRegisterDef(Register Reg, MachineInstr *MI) {
    auto Def = FlowGraphNode(FGNK_SecretRegisterDef);
    Def.setReg(Reg);
    Def.setMI(MI);
    return Def;
  }

  static FlowGraphNode CreateRegisterUse(Register Reg, MachineInstr *MI) {
    auto Def = FlowGraphNode(FGNK_SecretRegisterUse);
    Def.setReg(Reg);
    Def.setMI(MI);
    return Def;
  }

  static FlowGraphNode CreateGlobalUse(GlobalVariable *GV, MachineInstr *MI) {
    auto Def = FlowGraphNode(FGNK_SecretGlobalUse);
    Def.setGlobalVariable(GV);
    Def.setMI(MI);
    return Def;
  }

  static FlowGraphNode CreateControlDep(Register Reg, MachineInstr *MI) {
    auto Def = FlowGraphNode(FGNK_ControlDep);
    Def.setReg(Reg);
    Def.setMI(MI);
    return Def;
  }

  NodeKind getKind() const { return Kind; }

  bool operator==(const FlowGraphNode &Other) const {
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
      return getGlobalVariable() == Other.getGlobalVariable() && Other.getMI() == getMI();
    default:
      return true;
    }
  }

  bool operator<(const FlowGraphNode &Other) const {
    return std::tie(GlobalVar, Reg, MI) <
           std::tie(Other.GlobalVar, Other.Reg, Other.MI);
  }

  bool hasReg() const {
    return getKind() == FGNK_Argument || getKind() == FGNK_SecretRegisterDef || getKind() == FGNK_SecretRegisterUse || getKind() == FGNK_ControlDep;
  }

  Register getReg() const {
    assert(hasReg());
    return Reg;
  }

  MachineInstr *getMI() const {
    assert(isRegisterDef() || isRegisterUse() || isGlobalUse() || isControlDep());
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
    assert(isRegisterDef() || isRegisterUse() || isGlobalUse() || isControlDep());
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

  void print(raw_ostream &OS) const {
    OS << "(";
      
    switch (getKind()) {
    case FGNK_Argument:
      OS << "Argument";
      break;
    case FGNK_SecretRegisterDef:
      OS << "RegisterDef";
      break;
    case FGNK_SecretRegisterUse:
      OS << "RegisterUse";
      break;
    case FGNK_SecretGlobalUse:
      OS << "GlobalUse";
      break;
    case FGNK_ControlDep:
      OS << "ControlDep";
      break;
    case FGNK_Global:
      OS << "Global";
      break;
    default:
      OS << "UNKNOWN";
    }

    OS << " ";

    switch (getKind()) {
    case FGNK_Argument:
      OS << printReg(getReg());
      break;
    case FGNK_SecretRegisterDef:
    case FGNK_SecretRegisterUse:
    case FGNK_ControlDep:
      OS << printReg(getReg());
      OS << " at ";
      getMI()->print(OS, true, false, false, false);
      break;
    case FGNK_SecretGlobalUse:
      OS << getGlobalVariable();
      OS << " at ";
      getMI()->print(OS, true, false, false, false);
      break;
    case FGNK_Global:
      OS << getGlobalVariable();
      break;
    default:
      break;
    }

    OS << ")";
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { print(dbgs()); }
#endif
};

template <> struct DenseMapInfo<FlowGraphNode> {
  static inline FlowGraphNode getEmptyKey() {
    return FlowGraphNode(FlowGraphNode::FGNK_Empty);
  }

  static inline FlowGraphNode getTombstoneKey() {
    return FlowGraphNode(FlowGraphNode::FGNK_Tombstone);
  }

  static unsigned getHashValue(const FlowGraphNode &Key) {
    return Key.getHashValue();
  }

  static bool isEqual(const FlowGraphNode &LHS, const FlowGraphNode &RHS) {
    return LHS == RHS;
  }
};

class FlowGraph {
  using const_iterator = SmallSet<FlowGraphNode, 4>::const_iterator;

private:
  DenseMap<FlowGraphNode, uint64_t> SecretMasks;
  DenseMap<FlowGraphNode, SmallSet<FlowGraphNode, 4>> Nodes;
  DenseMap<FlowGraphNode, SmallSet<FlowGraphNode, 4>> Pred;

  void createEdge(FlowGraphNode From, FlowGraphNode To) {
    Nodes[From].insert(To);
    Pred[To].insert(From);
  }

public:
  FlowGraph(MachineFunction &MF, ReachingDefAnalysis *RDA = nullptr,
            MachineDominatorTree *MDT = nullptr,
            MachinePostDominatorTree *MPDT = nullptr);
  ~FlowGraph() = default;

  DenseMap<FlowGraphNode, uint64_t> &compute(const TargetInstrInfo *TII, const TargetRegisterInfo *TRI);
  void getSecretUses(SmallPtrSetImpl<MachineInstr *> &Uses);

  const_iterator succ_begin(FlowGraphNode Node) { return Nodes[Node].begin(); }
  const_iterator succ_end(FlowGraphNode Node) { return Nodes[Node].end(); }
  iterator_range<const_iterator> successors(FlowGraphNode Node) {
    return make_range(succ_begin(Node), succ_end(Node));
  }

  void print(raw_ostream &OS) const {
    OS << "FlowGraph:\n";
    for (auto &Pair : Nodes) {
      Pair.getFirst().print(OS);
      OS << " -> {\n";
      for (auto &T : Pair.getSecond()) {
        OS << "\t";
        T.print(OS);
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

class TrackSecretsAnalysis : public MachineFunctionPass {
public:
  static char ID;

  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;

  SmallPtrSet<MachineInstr *, 8> SecretUses;
    
  TrackSecretsAnalysis(bool IsSSA = true);
  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    if (!IsSSA) {
      AU.addRequired<ReachingDefAnalysis>();
      AU.addRequired<MachineDominatorTree>();
    }
    AU.addRequired<MachinePostDominatorTree>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  FlowGraph *Graph = nullptr;
  bool IsSSA;
};

} // namespace llvm

#endif

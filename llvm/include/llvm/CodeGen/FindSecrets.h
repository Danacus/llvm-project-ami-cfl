#ifndef LLVM_CODEGEN_FINDSECRETS_H
#define LLVM_CODEGEN_FINDSECRETS_H

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/CodeGen/Register.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"

using namespace llvm;

namespace llvm {

class SecretDef {
public:
  enum DefKind {
    SDK_Argument,
    SDK_Global,
    SDK_SecretRegisterDef,
    SDK_Empty,
    SDK_Tombstone,
  };

private:
  GlobalVariable *GlobalVar;
  Register Reg;
  MachineInstr *MI;
  DefKind Kind;

public:
  SecretDef(DefKind Kind) : Kind(Kind) {}

  static SecretDef CreateArgument(Register Reg) {
    auto Def = SecretDef(SDK_Argument);
    Def.setReg(Reg);
    return Def;
  }

  static SecretDef CreateGlobal(GlobalVariable *GV) {
    auto Def = SecretDef(SDK_Global);
    Def.setGlobalVariable(GV);
    return Def;
  }

  static SecretDef CreateRegisterDef(Register Reg, MachineInstr *MI) {
    auto Def = SecretDef(SDK_SecretRegisterDef);
    Def.setReg(Reg);
    Def.setMI(MI);
    return Def;
  }

  DefKind getKind() const { return Kind; }

  bool operator==(const SecretDef &Other) const {
    if (getKind() != Other.getKind())
      return false;

    switch (getKind()) {
    case SDK_Argument:
      return getReg() == Other.getReg();
    case SDK_Global:
      return getGlobalVariable() == Other.getGlobalVariable();
    case SDK_SecretRegisterDef:
      return getReg() == Other.getReg() && getMI() == Other.getMI();
    default:
      return true;
    }
  }

  bool operator<(const SecretDef &Other) const {
    return std::tie(GlobalVar, Reg, MI) <
           std::tie(Other.GlobalVar, Other.Reg, Other.MI);
  }

  bool hasReg() const {
    return getKind() == SDK_Argument || getKind() == SDK_SecretRegisterDef;
  }

  Register getReg() const {
    assert(hasReg());
    return Reg;
  }

  MachineInstr *getMI() const {
    assert(isRegister());
    return MI;
  }

  GlobalVariable *getGlobalVariable() const {
    assert(isGlobal());
    return GlobalVar;
  }

  void setReg(Register R) {
    assert(hasReg());
    Reg = R;
  }

  void setMI(MachineInstr *I) {
    assert(isRegister());
    MI = I;
  }

  void setGlobalVariable(GlobalVariable *GV) {
    assert(isGlobal());
    GlobalVar = GV;
  }

  bool isArgument() const { return getKind() == SDK_Argument; }

  bool isGlobal() const { return getKind() == SDK_Global; }

  bool isRegister() const { return getKind() == SDK_SecretRegisterDef; }

  unsigned getHashValue() const {
    switch (getKind()) {
    case SDK_Argument:
      return hash_combine(getKind(), getReg());
    case SDK_SecretRegisterDef:
      return hash_combine(getKind(), getReg(), getMI());
    case SDK_Global:
      return hash_combine(getKind(), getGlobalVariable());
    default:
      return hash_combine(getKind());
    }
  }

  /// Find all operands in the given instruction that define or use this
  /// SecretDef
  void findOperands(MachineInstr *MI, SmallVector<MachineOperand, 8> &Ops,
                    TargetRegisterInfo *TRI = nullptr) const;
};

template <> struct DenseMapInfo<SecretDef> {
  static inline SecretDef getEmptyKey() {
    return SecretDef(SecretDef::SDK_Empty);
  }

  static inline SecretDef getTombstoneKey() {
    return SecretDef(SecretDef::SDK_Tombstone);
  }

  static unsigned getHashValue(const SecretDef &Key) {
    return Key.getHashValue();
  }

  static bool isEqual(const SecretDef &LHS, const SecretDef &RHS) {
    return LHS == RHS;
  }
};

class FlowGraph {
  MachineFunction &MF;
  MachineDominatorTree *MDT;
  MachinePostDominatorTree *MPDT;
  ReachingDefAnalysis *RDA;
  MachineRegisterInfo &MRI;

  // DenseMap<MachineBasicBlock *, SmallPtrSet<MachineInstr *, 8>> ControlDeps;
  DenseMap<MachineInstr *, SmallPtrSet<MachineBasicBlock *, 8>> ControlDeps;

public:
  FlowGraph(MachineFunction &MF, ReachingDefAnalysis *RDA = nullptr,
            MachineDominatorTree *MDT = nullptr, MachinePostDominatorTree *MPDT = nullptr);
  void getSources(SmallSet<SecretDef, 8> &Defs,
                  DenseMap<SecretDef, uint64_t> &Secrets) const;
  void getUses(SecretDef &SD, SmallPtrSet<MachineInstr *, 8> &Uses);
};

class SecretUse {
public:
  using OperandsSet = SmallVector<MachineOperand, 8>;

private:
  SecretDef Def;
  MachineInstr *User;
  OperandsSet Operands;
  uint64_t SecretMask;

public:
  SecretUse()
      : Def(SecretDef(SecretDef::SDK_Empty)), User(nullptr),
        Operands(OperandsSet()), SecretMask(0) {}
  SecretUse(SecretDef Def, MachineInstr *User, OperandsSet Operands,
            uint64_t SecretMask)
      : Def(Def), User(User), Operands(Operands), SecretMask(SecretMask) {}
  SecretUse(SecretDef Def, MachineInstr *User, uint64_t SecretMask)
      : Def(Def), User(User), Operands(OperandsSet()), SecretMask(SecretMask) {
    Def.findOperands(User, Operands);
  }

  uint64_t getSecretMask() { return SecretMask; }
  SecretDef &getDef() { return Def; };
  const SecretDef &getDef() const { return Def; };
  MachineInstr *getUser() { return User; }
  const MachineInstr *getUser() const { return User; }

  iterator_range<OperandsSet::const_iterator> operands() const {
    return Operands;
  }
};

class TrackSecretsAnalysis : public MachineFunctionPass {
public:
  static char ID;

  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;

  using SecretsSet = SmallSet<SecretDef, 8>;
  using SecretsMap = DenseMap<SecretDef, uint64_t>;
  using SecretsUseMap =
      DenseMap<std::pair<MachineInstr *, SecretDef>, SecretUse>;

  SecretsUseMap SecretUses;

  TrackSecretsAnalysis(bool IsSSA = true);

  void handleUse(MachineInstr &UseInst, MachineOperand &MO, uint64_t SecretMask,
                 SecretsSet &WorkSet, SecretsMap &SecretDefs);

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
  SecretsMap Secrets;
  bool IsSSA;
};

} // namespace llvm

#endif // LLVM_CODEGEN_FINDSECRETS_H

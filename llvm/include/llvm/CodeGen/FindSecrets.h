#ifndef LLVM_CODEGEN_FINDSECRETS_H
#define LLVM_CODEGEN_FINDSECRETS_H

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
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

class SecretArgument {
private:
  Register Reg;

public:
  SecretArgument(Register R) : Reg(R) {}
  SecretArgument(const SecretArgument &O) = default;
  Register getReg() const { return Reg; }

  bool operator==(const SecretArgument &Other) const {
    return Other.Reg == Reg;
  }
  bool operator<(const SecretArgument &Other) const { return Reg < Other.Reg; }
};

template <> struct DenseMapInfo<SecretArgument> {
  static inline SecretArgument getEmptyKey() {
    return SecretArgument(DenseMapInfo<Register>::getEmptyKey());
  }

  static inline SecretArgument getTombstoneKey() {
    return SecretArgument(DenseMapInfo<Register>::getTombstoneKey());
  }

  static unsigned getHashValue(const SecretArgument &Key) {
    return DenseMapInfo<Register>::getHashValue(Key.getReg());
  }
};

class SecretGlobal {
private:
  GlobalVariable *GlobalVar;

public:
  SecretGlobal(GlobalVariable *GV) : GlobalVar(GV) {}
  SecretGlobal(const SecretGlobal &O) = default;
  const GlobalVariable *getGlobalVar() const { return GlobalVar; }

  bool operator==(const SecretGlobal &Other) const {
    return Other.GlobalVar == GlobalVar;
  }
  bool operator<(const SecretGlobal &Other) const {
    return GlobalVar < Other.GlobalVar;
  }
};

template <> struct DenseMapInfo<SecretGlobal> {
  static inline SecretGlobal getEmptyKey() {
    return SecretGlobal(DenseMapInfo<GlobalVariable *>::getEmptyKey());
  }

  static inline SecretGlobal getTombstoneKey() {
    return SecretGlobal(DenseMapInfo<GlobalVariable *>::getTombstoneKey());
  }

  static unsigned getHashValue(const SecretGlobal &Key) {
    return DenseMapInfo<GlobalVariable *>::getHashValue(Key.getGlobalVar());
  }
};

class SecretRegisterDef {
private:
  Register Reg;
  MachineInstr *MI;

public:
  SecretRegisterDef(Register R, MachineInstr *MI) : Reg(R), MI(MI) {}
  SecretRegisterDef(const SecretRegisterDef &O) = default;
  Register getReg() const { return Reg; }
  MachineInstr *getMI() const { return MI; }

  bool operator==(const SecretRegisterDef &Other) const {
    return Other.Reg == Reg && Other.MI == MI;
  }
  bool operator<(const SecretRegisterDef &Other) const {
    return std::tie(Reg, MI) < std::tie(Other.Reg, Other.MI);
  }
};

template <> struct DenseMapInfo<SecretRegisterDef> {
  static inline SecretRegisterDef getEmptyKey() {
    return SecretRegisterDef(DenseMapInfo<Register>::getEmptyKey(),
                             DenseMapInfo<MachineInstr *>::getEmptyKey());
  }

  static inline SecretRegisterDef getTombstoneKey() {
    return SecretRegisterDef(DenseMapInfo<Register>::getTombstoneKey(),
                             DenseMapInfo<MachineInstr *>::getTombstoneKey());
  }

  static unsigned getHashValue(const SecretRegisterDef &Key) {
    return DenseMapInfo<
        std::pair<Register, const MachineInstr *>>::getHashValue({Key.getReg(),
                                                                  Key.getMI()});
  }
};

// Poor men's Algebraic Data Type
class SecretDef {
public:
  using Variant = std::variant<SecretArgument, SecretGlobal, SecretRegisterDef>;

private:
  Variant Var;

public:
  enum DefKind {
    SDK_Argument,
    SDK_Global,
    SDK_SecretRegisterDef,
  };

  SecretDef(Variant &&V) : Var(V) {}

  static SecretDef argument(Register Reg) {
    return SecretDef(SecretArgument(Reg));
  }

  static SecretDef global(GlobalVariable *G) {
    return SecretDef(SecretGlobal(G));
  }

  static SecretDef registerDef(Register Reg, MachineInstr *MI) {
    return SecretDef(SecretRegisterDef(Reg, MI));
  }

  DefKind getKind() const { return static_cast<DefKind>(Var.index()); }

  template <typename T> T get() { return std::get<T>(Var); }
  template <typename T> T get() const { return std::get<T>(Var); }

  template <typename T> T get(DefKind K) {
    return std::get<T>(static_cast<uint>(K));
  }
  template <typename T> T get(DefKind K) const {
    return std::get<T>(static_cast<uint>(K));
  }

  template <typename T> T *get_if() { return std::get_if<T>(&Var); }
  template <typename T> const T *get_if() const { return std::get_if<T>(&Var); }

  Variant getVariant() const { return Var; }

  bool operator==(const SecretDef &Other) const {
    return Other.getVariant() == getVariant();
  }
  bool operator<(const SecretDef &Other) const {
    return Other.getVariant() < getVariant();
  }

  bool hasReg() const {
    return getKind() == SDK_Argument || getKind() == SDK_SecretRegisterDef;
  }

  Register getReg() const {
    switch (getKind()) {
    case SDK_Argument:
      return get<SecretArgument>().getReg();
    case SDK_SecretRegisterDef:
      return get<SecretRegisterDef>().getReg();
    default:
      llvm_unreachable("secret def does not have a reg");
    }
  }

  /// Find all operands in the given instruction that define or use this
  /// SecretDef
  void findOperands(MachineInstr *MI, SmallVector<MachineOperand, 8> &Ops,
                    TargetRegisterInfo *TRI = nullptr) const;
};

template <> struct DenseMapInfo<SecretDef> {
  static inline SecretDef getEmptyKey() {
    return SecretDef(DenseMapInfo<SecretDef::Variant>().getEmptyKey());
  }

  static inline SecretDef getTombstoneKey() {
    return SecretDef(DenseMapInfo<SecretDef::Variant>().getTombstoneKey());
  }

  static unsigned getHashValue(const SecretDef &Key) {
    return DenseMapInfo<SecretDef::Variant>().getHashValue(Key.getVariant());
  }

  static bool isEqual(const SecretDef &LHS, const SecretDef &RHS) {
    return DenseMapInfo<SecretDef::Variant>().isEqual(LHS.getVariant(),
                                                      RHS.getVariant());
  }
};

class GraphDataPhysReg {
public:
  ReachingDefAnalysis &RDA;

  GraphDataPhysReg(ReachingDefAnalysis &RDA) : RDA(RDA) {}
};

class GraphDataVirtReg {
public:
  const MachineRegisterInfo &MRI;

  GraphDataVirtReg(const MachineRegisterInfo &MRI) : MRI(MRI) {}
};

enum GraphType {
  GT_PhysReg,
  GT_VirtReg,
};

template <class GraphDataT, GraphType GT> class FlowGraph {
private:
  GraphDataT Data;
  MachineFunction &MF;

public:
  FlowGraph(GraphDataT Data, MachineFunction &MF) : Data(Data), MF(MF) {}
  void getSources(SmallSet<SecretDef, 8> &Defs,
                  DenseMap<SecretDef, uint64_t> &Secrets) const;
  void getUses(SecretDef &SD, SmallPtrSet<MachineInstr *, 8> &Uses) const;
  void getReachingDefs(SecretDef &SD,
                       SmallPtrSet<MachineInstr *, 8> &Defs) const;
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
      : Def(SecretDef::argument(0)), User(nullptr), Operands(OperandsSet()),
        SecretMask(0) {}
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

template <class GraphDataT, GraphType GT> class TrackSecretsAnalysis {
public:
  static char ID;
  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;

  using SecretsSet = SmallSet<SecretDef, 8>;
  using SecretsMap = DenseMap<SecretDef, uint64_t>;
  using SecretsUseMap =
      DenseMap<std::pair<MachineInstr *, SecretDef>, SecretUse>;

  SecretsUseMap SecretUses;

  TrackSecretsAnalysis() {}

  void handleUse(MachineInstr &UseInst, MachineOperand &MO, uint64_t SecretMask,
                 SecretsSet &WorkSet, SecretsMap &SecretDefs);

  bool run(MachineFunction &MF, FlowGraph<GraphDataT, GT> Graph);

private:
  SecretsMap Secrets;
};

class TrackSecretsAnalysisVirtReg : public MachineFunctionPass {
public:
  static char ID;

  TrackSecretsAnalysisVirtReg();

  bool runOnMachineFunction(MachineFunction &MF) override {
    return TSA.run(
        MF, FlowGraph<GraphDataVirtReg, GT_VirtReg>(
                GraphDataVirtReg(MF.getRegInfo()), MF));
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  TrackSecretsAnalysis<GraphDataVirtReg, GT_VirtReg> TSA;
};

class TrackSecretsAnalysisPhysReg : public MachineFunctionPass {
public:
  static char ID;

  TrackSecretsAnalysisPhysReg();

  bool runOnMachineFunction(MachineFunction &MF) override {
    return TSA.run(
        MF, FlowGraph<GraphDataPhysReg, GT_PhysReg>(
                GraphDataPhysReg(getAnalysis<ReachingDefAnalysis>()), MF));
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<ReachingDefAnalysis>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  TrackSecretsAnalysis<GraphDataPhysReg, GT_PhysReg> TSA;
};

} // namespace llvm

#endif // LLVM_CODEGEN_FINDSECRETS_H

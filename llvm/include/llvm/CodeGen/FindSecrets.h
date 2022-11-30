#ifndef LLVM_CODEGEN_FINDSECRETS_H
#define LLVM_CODEGEN_FINDSECRETS_H

#include "llvm/ADT/DenseMapInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/CodeGen/Register.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/Casting.h"

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
  bool operator<(const SecretArgument &Other) const {
    return Reg < Other.Reg;
  }
};

template <> struct DenseMapInfo<SecretArgument> {
  static inline SecretArgument getEmptyKey() {
    return SecretArgument(DenseMapInfo<Register>::getEmptyKey());
  }

  static inline SecretArgument getTombstoneKey() {
    return SecretArgument(DenseMapInfo<Register>::getTombstoneKey());
  }

  static unsigned getHashValue(const SecretArgument Key) {
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

  static unsigned getHashValue(const SecretGlobal Key) {
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

  static unsigned getHashValue(const SecretRegisterDef Key) {
    return DenseMapInfo<
        std::pair<Register, const MachineInstr *>>::getHashValue({Key.getReg(),
                                                                  Key.getMI()});
  }
};

// Poor men's Algebraic Data Type
using SecretDef = std::variant<SecretArgument, SecretGlobal, SecretRegisterDef>;

class TrackSecretsAnalysis : public MachineFunctionPass {
public:
  static char ID;
  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;

  using SecretsSet = SmallSet<SecretDef, 8>;
  using SecretsMap = DenseMap<SecretDef, uint64_t>;

  SecretsMap SecretUses;

  TrackSecretsAnalysis();

  void handleUse(MachineInstr &UseInst, MachineOperand *MO, uint64_t SecretMask,
                 SecretsSet &WorkSet, SecretsMap &SecretDefs);
  SecretsSet findSecretSources(MachineFunction &MF);

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<ReachingDefAnalysis>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

private:
  SecretsMap Secrets;
  DenseMap<GlobalVariable *, uint64_t> SecretGlobals;
};

class TrackSecretsPrinter : public MachineFunctionPass {
public:
  static char ID;

  TrackSecretsPrinter();

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TrackSecretsAnalysis>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_FINDSECRETS_H

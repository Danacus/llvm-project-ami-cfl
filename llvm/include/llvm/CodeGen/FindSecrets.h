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

using namespace llvm;

struct SecretDef {
  MachineInstr *MI;
  Register Reg;

  SecretDef(MachineInstr *MI, Register Reg) : MI(MI), Reg(Reg) {}

  bool operator<(const SecretDef &Other) const {
    return std::tie(MI, Reg) < std::tie(Other.MI, Other.Reg);
  }

  bool operator==(const SecretDef &Other) const {
    return std::tie(MI, Reg) == std::tie(Other.MI, Other.Reg);
  }
};

namespace llvm {

template <> struct DenseMapInfo<SecretDef, void> {
  static inline SecretDef getEmptyKey() {
    SecretDef S(nullptr, 0);
    return S;
  }

  static inline SecretDef getTombstoneKey() {
    SecretDef S(nullptr, 0);
    return S;
  }

  static unsigned getHashValue(const SecretDef &Key) {
    return DenseMapInfo<std::pair<uint64_t, uint64_t>>::getHashValue(
        std::pair((uint64_t)Key.MI, Key.Reg));
  }

  static bool isEqual(const SecretDef &LHS, const SecretDef &RHS) {
    return LHS == RHS;
  }
};

class TrackSecretsAnalysis : public MachineFunctionPass {
public:
  static char ID;
  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;

  using SecretsSet = SmallSet<SecretDef, 8>;
  using SecretsMap = DenseMap<SecretDef, uint64_t>;

  SecretsMap SecretUses;

  TrackSecretsAnalysis();

  void handleUse(MachineInstr &UseInst, Register Reg, uint64_t SecretMask,
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

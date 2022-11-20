#ifndef LLVM_CODEGEN_FINDSECRETS_H
#define LLVM_CODEGEN_FINDSECRETS_H

#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/CodeGen/Register.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

using namespace llvm;

struct Secret {
  MachineInstr *MI;
  MCRegister Reg;
  uint64_t SecretMask;
  bool IsDef;

  Secret(MachineInstr *MI, MCRegister Reg, uint64_t SecretMask, bool IsDef = true)
      : MI(MI), Reg(Reg), SecretMask(SecretMask), IsDef(IsDef) {}

  bool operator<(const Secret &Other) const {
    return std::tie(MI, Reg, SecretMask) <
           std::tie(Other.MI, Other.Reg, Other.SecretMask);
  }

  bool operator==(const Secret &Other) const {
    return std::tie(MI, Reg, SecretMask) ==
           std::tie(Other.MI, Other.Reg, Other.SecretMask);
  }
};

namespace llvm {

class TrackSecretsAnalysis : public MachineFunctionPass {
public:
  static char ID;
  SmallSet<Secret, 8> Secrets;

  TrackSecretsAnalysis();

  void handleUse(MachineInstr *UseInst, MCRegister Reg, uint64_t SecretMask,
                 SmallSet<Secret, 8> &SecretDefs);
  SmallSet<Secret, 8> findSecretSources(MachineFunction &MF);

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<ReachingDefAnalysis>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_FINDSECRETS_H

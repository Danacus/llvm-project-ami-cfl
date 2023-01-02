#ifndef LLVM_TARGET_RISCV_AMI_INSERT_PERSISTENT_DEFS
#define LLVM_TARGET_RISCV_AMI_INSERT_PERSISTENT_DEFS

#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/PersistencyAnalysis.h"
#include "llvm/CodeGen/SensitiveRegion.h"

using namespace llvm;

namespace {

class AMiInsertPersistentDefs : public MachineFunctionPass {
  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;

  DenseMap<MachineBasicBlock *, MIBundleBuilder> DefBundles;
  
public:
  static char ID;

  AMiInsertPersistentDefs();
  
  void insertImplicitDef(MachineFunction &MF, MachineRegion &MR, Register Reg);
  
  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // AU.addRequired<MachineRegionInfoPass>();
    // AU.addRequiredTransitive<TrackSecretsAnalysisVirtReg>();
    AU.addRequired<SensitiveRegionAnalysisPass>();
    AU.addPreserved<SensitiveRegionAnalysisPass>();
    AU.addRequired<PersistencyAnalysisPass>();
    AU.addPreserved<PersistencyAnalysisPass>();
    AU.addRequired<LiveVariables>();
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace

#endif // LLVM_TARGET_RISCV_AMI_INSERT_PERSISTENT_DEFS


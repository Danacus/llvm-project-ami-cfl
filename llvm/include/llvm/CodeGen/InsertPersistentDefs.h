#ifndef LLVM_CODEGEN_INSERT_PERSISTENT_DEFS
#define LLVM_CODEGEN_INSERT_PERSISTENT_DEFS

#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/PersistencyAnalysis.h"
#include "llvm/CodeGen/SensitiveRegion.h"

using namespace llvm;

namespace llvm {

class InsertPersistentDefs : public MachineFunctionPass {
  const TargetInstrInfo *TII;
  const TargetRegisterInfo *TRI;

  DenseMap<MachineBasicBlock *, MIBundleBuilder> DefBundles;
  
public:
  static char ID;

  InsertPersistentDefs();
  
  void insertRegionOuts(MachineFunction &MF, MachineRegion &MR);
  void insertPersistentDef(MachineFunction &MF, MachineRegion &MR, Register Reg);
  void insertPersistentDef(MachineInstr *MI);
  
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

} // namespace llvm

#endif // LLVM_CODEGEN_INSERT_PERSISTENT_DEFS


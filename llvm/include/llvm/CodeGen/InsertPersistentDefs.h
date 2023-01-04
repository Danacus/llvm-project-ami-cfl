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

public:
  static char ID;

  InsertPersistentDefs();
  
  void insertGhostLoad(MachineInstr *StoreMI);
  void insertPersistentDefEnd(MachineFunction &MF, MachineRegion &MR, Register Reg);
  void insertPersistentDefEnd(MachineInstr *MI);

  void insertPersistentDefStart(MachineFunction &MF, MachineRegion &MR, Register Reg);
  void insertPersistentDefStart(MachineInstr *MI);

  void insertPersistentDef(MachineInstr *MI);

  void insertPersistentDefs(MachineRegion *MR);

  void updateLiveIntervals(MachineBasicBlock *MBB, MachineInstr &Def, MachineInstr &Extend, Register Reg);
  
  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<SensitiveRegionAnalysisVirtReg>();
    AU.addPreserved<SensitiveRegionAnalysisVirtReg>();
    AU.addRequired<PersistencyAnalysisPass>();
    AU.addPreserved<PersistencyAnalysisPass>();
    AU.addRequired<LiveVariables>();
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // LLVM_CODEGEN_INSERT_PERSISTENT_DEFS


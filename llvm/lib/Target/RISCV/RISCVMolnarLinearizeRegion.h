#ifndef LLVM_TARGET_RISCV_MOLNAR_LINEARIZE_REGION_H
#define LLVM_TARGET_RISCV_MOLNAR_LINEARIZE_REGION_H

#include "RISCVInstrInfo.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineDominanceFrontier.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/CodeGen/SensitiveRegion.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetRegisterInfo.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"

using namespace llvm;

namespace {

class RISCVMolnarLinearizeRegion : public MachineFunctionPass {
public:
  using RegionInstrMap =
      DenseMap<const MachineRegion *, SmallPtrSet<MachineInstr *, 16>>;

private:
  const RISCVInstrInfo *TII;
  const RISCVRegisterInfo *TRI;
  MachineRegionInfo *MRI;
  MachineRegisterInfo *RegInfo;
  SensitiveRegionAnalysis *SRA;
  SmallPtrSet<MachineRegion *, 16> ActivatingRegions;
  SmallVector<SensitiveBranch, 16> ActivatingBranches;
  DenseMap<MachineRegion *, Register> TakenRegMap;
  GlobalVariable *GlobalTaken = nullptr;
  Register GlobalTakenAddrReg;

  RegionInstrMap PersistentStores;
  RegionInstrMap CallInstructions;

public:
  static char ID;

  RISCVMolnarLinearizeRegion();

  void handleRegion(MachineBasicBlock *BranchBlock, MachineRegion *Region, Register TakenReg);
  Register loadTakenReg(MachineFunction &MF);
  void linearizeBranches(MachineFunction &MF);
  void replacePHIInstructions();
  void findStoresAndCalls(MachineRegion *MR);

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequiredTransitive<MachineDominatorTree>();
    AU.addRequired<MachinePostDominatorTree>();
    AU.addRequiredTransitive<MachineDominanceFrontier>();
    AU.addRequired<SensitiveRegionAnalysis>();
    // AU.addRequired<PersistencyAnalysisPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace

#endif // LLVM_TARGET_RISCV_MOLNAR_LINEARIZE_REGION

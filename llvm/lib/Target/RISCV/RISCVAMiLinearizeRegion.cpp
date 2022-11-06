#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "RISCV.h"
#include "RISCVAMiLinearizeRegion.h"
#include "RISCVSubtarget.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "ami-linearize"

char AMiLinearizeRegion::ID = 0;

bool AMiLinearizeRegion::runOnMachineFunction(MachineFunction &MF) {
  errs() << "AMi Linearize Region Pass\n";
  auto &RDA = getAnalysis<ReachingDefAnalysis>();
  
  const auto &ST = MF.getSubtarget();
  const auto *TII = ST.getInstrInfo();
  const auto *TRI = ST.getRegisterInfo();

  
  for (MachineBasicBlock &MB : MF) {
    //errs() << "Entering Machine Block: " << MB.getName() << "\n";

    for (MachineInstr &MI : MB) {
      //errs() << "Entering Machine Instruction: " << MI << "\n";

      if (MI.getOpcode() == RISCV::ADDI) {
        MI.setDesc(TII->get(RISCV::PADDI));
      }

      //errs() << "\n";
    }
  }

  for (MachineBasicBlock &MB : MF) {
    errs() << MB.getName();
    errs() << MB;
  }

  return true;
}

AMiLinearizeRegion::AMiLinearizeRegion() : MachineFunctionPass(ID) {
  initializeAMiLinearizeRegionPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AMiLinearizeRegion, DEBUG_TYPE, "AMi Linearize Region",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(ReachingDefAnalysis)
INITIALIZE_PASS_END(AMiLinearizeRegion, DEBUG_TYPE, "AMi Linearize Region",
                    false, false)

namespace llvm {

FunctionPass *createAMiLinearizeRegionPass() {
  return new AMiLinearizeRegion();
}

} // namespace llvm

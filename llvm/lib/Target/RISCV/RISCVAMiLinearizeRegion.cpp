#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "RISCV.h"
#include "RISCVAMiLinearizeRegion.h"
#include "RISCVInstrInfo.h"
#include "RISCVSubtarget.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegionInfo.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/IR/Argument.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugLoc.h"
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
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>

using namespace llvm;

#define DEBUG_TYPE "ami-linearize-region"

char AMiLinearizeRegion::ID = 0;

template <RISCV::AMi::Qualifier Q>
void AMiLinearizeRegion::setQualifier(MachineInstr *I) {
  if (RISCV::AMi::hasQualifier<Q>(I->getOpcode()))
    return;

  auto PersistentInstr = RISCV::AMi::getQualified<Q>(I->getOpcode());

  if (PersistentInstr != -1) {
    I->setDesc(TII->get(PersistentInstr));
  } else {
    // llvm_unreachable("AMi error: unsupported instruction cannot be
    // qualified!");
    I->dump();
    errs() << "AMi error: unsupported instruction cannot be qualified!\n";
  }
}

void AMiLinearizeRegion::handleRegion(MachineRegion *Region) {
  errs() << "Handling region " << *Region << "\n";
  for (MachineInstr *MI : PA->getPersistentInstructions(Region)) {
    setQualifier<RISCV::AMi::Persistent>(MI);
  }

  for (MachineInstr *I : PA->getPersistentStores(Region)) {
    MachineInstr &GhostLoad = *std::prev(I->getIterator());

    if (GhostLoad.getOpcode() == RISCV::GLW) {
      assert(GhostLoad.getOperand(0).getReg() == I->getOperand(0).getReg() &&
             "AMi error: invalid ghost load");
      continue;
    }

    assert(GhostLoad.getOpcode() == TargetOpcode::GHOST_LOAD &&
           "AMi error: expected GHOST_LOAD pseudo");
    assert(GhostLoad.getOperand(0).getReg() == I->getOperand(0).getReg() &&
           "AMi error: invalid GHOST_LOAD");

    BuildMI(*I->getParent(), GhostLoad.getIterator(), DebugLoc(),
            TII->get(TargetOpcode::COPY),
            GhostLoad.getOperand(0).getReg().asMCReg())
        .add(GhostLoad.getOperand(1));
    GhostLoad.eraseFromParent();

    if (I->getNumOperands() > 2 && I->getOperand(0).isReg()) {
      MachineOperand Op1 = I->getOperand(1);
      Op1.setIsKill(false);
      MachineOperand Op2 = I->getOperand(2);
      BuildMI(*I->getParent(), I->getIterator(), DebugLoc(),
              TII->get(RISCV::GLW), I->getOperand(0).getReg().asMCReg())
          .add(Op1)
          .add(Op2);
    } else {
      llvm_unreachable("AMi error: unable to nullify unwanted side-effects in "
                       "mimicry mode!");
    }
  }
}

bool AMiLinearizeRegion::runOnMachineFunction(MachineFunction &MF) {
  errs() << "AMi Linearize Region Pass\n";

  const auto &ST = MF.getSubtarget<RISCVSubtarget>();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();
  PA = &getAnalysis<PersistencyAnalysisPass>();

  SRA = &getAnalysis<SensitiveRegionAnalysis>();
  ActivatingBranches = SmallVector<SensitiveBranch>(SRA->sensitive_branches());

  std::sort(ActivatingBranches.begin(), ActivatingBranches.end(),
            std::greater<SensitiveBranch>());

  for (auto &Branch : ActivatingBranches) {
    if (Branch.IfRegion) {
      handleRegion(Branch.IfRegion);
    }
    if (Branch.ElseRegion) {
      handleRegion(Branch.ElseRegion);
    }
  }

  SmallPtrSet<MachineInstr *, 8> ToRemove;

  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      if (MI.getOpcode() == TargetOpcode::SECRET)
        ToRemove.insert(&MI);
    }
  }

  for (auto *MI : ToRemove) {
    MI->eraseFromParent();
  }
  MF.dump();

  return true;
}

AMiLinearizeRegion::AMiLinearizeRegion() : MachineFunctionPass(ID) {
  initializeAMiLinearizeRegionPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AMiLinearizeRegion, DEBUG_TYPE, "AMi Linearize Region",
                      false, false)
// INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(SensitiveRegionAnalysis)
INITIALIZE_PASS_DEPENDENCY(PersistencyAnalysisPass)
INITIALIZE_PASS_END(AMiLinearizeRegion, DEBUG_TYPE, "AMi Linearize Region",
                    false, false)

namespace llvm {

FunctionPass *createAMiLinearizeRegionPass() {
  return new AMiLinearizeRegion();
}

} // namespace llvm

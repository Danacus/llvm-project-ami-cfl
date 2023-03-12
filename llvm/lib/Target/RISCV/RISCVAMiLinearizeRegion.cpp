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

#define DEBUG_TYPE "riscv-ami-linearize-region"

char RISCVAMiLinearizeRegion::ID = 0;

template <RISCV::AMi::Qualifier Q>
void RISCVAMiLinearizeRegion::setQualifier(MachineInstr *I) {
  if (RISCV::AMi::hasQualifier<Q>(I->getOpcode()))
    return;

  auto PersistentInstr = RISCV::AMi::getQualified<Q>(I->getOpcode());

  if (PersistentInstr != -1) {
    I->setDesc(TII->get(PersistentInstr));
  } else {
    // llvm_unreachable("AMi error: unsupported instruction cannot be
    // qualified!");
    errs() << "AMi error: unsupported instruction cannot be qualified!\n";
  }
}

bool RISCVAMiLinearizeRegion::setBranchActivating(MachineBasicBlock &MBB, MachineBasicBlock *Target) {
  // If the block has no terminators, it just falls into the block after it.
  MachineBasicBlock::iterator I = MBB.getLastNonDebugInstr();
  if (I == MBB.end() || !TII->isUnpredicatedTerminator(*I))
    return false;

  // Count the number of terminators and find the first unconditional or
  // indirect branch.
  MachineBasicBlock::iterator FirstUncondOrIndirectBr = MBB.end();
  int NumTerminators = 0;
  for (auto J = I.getReverse();
       J != MBB.rend() && TII->isUnpredicatedTerminator(*J); J++) {
    NumTerminators++;
    if (J->getDesc().isUnconditionalBranch() ||
        J->getDesc().isIndirectBranch()) {
      FirstUncondOrIndirectBr = J.getReverse();
    }
  }

  // We can't handle blocks that end in an indirect branch.
  if (I->getDesc().isIndirectBranch())
    return true;

  // We can't handle blocks with more than 2 terminators.
  if (NumTerminators > 2)
    return true;

  // Handle a single unconditional branch.
  if (NumTerminators == 1 && I->getDesc().isUnconditionalBranch()) {
    setBranchInstrActivating(&*I, Target);
    return false;
  }

  // Handle a single conditional branch.
  if (NumTerminators == 1 && I->getDesc().isConditionalBranch()) {
    setBranchInstrActivating(&*I, Target);
    return false;
  }

  // Handle a conditional branch followed by an unconditional branch.
  if (NumTerminators == 2 && std::prev(I)->getDesc().isConditionalBranch() &&
      I->getDesc().isUnconditionalBranch()) {
    setBranchInstrActivating(&*I, Target);
    setBranchInstrActivating(&*std::prev(I), Target);
    return false;
  }

  // Otherwise, we can't handle this.
  return true;
}

void RISCVAMiLinearizeRegion::setBranchInstrActivating(MachineInstr *I, MachineBasicBlock *Target) {
  MachineBasicBlock *Dest = TII->getBranchDestBlock(*I);
  if (!Target || Target == Dest) {
    if (I->getDesc().isConditionalBranch())
      setQualifier<llvm::RISCV::AMi::Activating>(I);

    if (I->getDesc().isUnconditionalBranch()) {
      // HACK: Since a.jal doesn't behave like a call, we need to use a.beq zero, zero
      BuildMI(*I->getParent(), I->getIterator(), DebugLoc(), TII->get(RISCV::ABEQ))
        .addReg(RISCV::X0)
        .addReg(RISCV::X0)
        .addMBB(Dest);
      I->eraseFromParent();
    }
  }
}

void RISCVAMiLinearizeRegion::handleRegion(MachineRegion *Region) {
  LLVM_DEBUG(errs() << "Handling region " << *Region << "\n");
  for (MachineInstr *MI : PA->getPersistentInstructions(Region)) {
    setQualifier<RISCV::AMi::Persistent>(MI);
  }

  LLVM_DEBUG(errs() << "Stores\n");
  for (MachineInstr *I : PA->getPersistentStores(Region)) {
    MachineInstr &GhostLoad = *std::prev(I->getIterator());

    // TODO: fix this to only allow writing to stack in mimicry mode
    // if this instruction originates from calling convention lowering.
    if (GhostLoad.getOpcode() != TargetOpcode::GHOST_LOAD)
      continue;

    assert(GhostLoad.getOpcode() == TargetOpcode::GHOST_LOAD &&
           "AMi error: expected GHOST_LOAD pseudo");
    assert(GhostLoad.getOperand(0).getReg() == I->getOperand(0).getReg() &&
           "AMi error: invalid GHOST_LOAD");

    BuildMI(*I->getParent(), GhostLoad.getIterator(), DebugLoc(),
            TII->get(RISCV::ADDI), GhostLoad.getOperand(0).getReg().asMCReg())
        .add(GhostLoad.getOperand(1))
        .addImm(0);
    GhostLoad.eraseFromParent();

    if (I->getNumOperands() > 2 && I->getOperand(0).isReg()) {
      MachineOperand Op1 = I->getOperand(1);
      Op1.setIsKill(false);
      MachineOperand Op2 = I->getOperand(2);

      auto Opcode = RISCV::AMi::getQualified<RISCV::AMi::Ghost>(
          TII->getMatchingLoad(*I));
      BuildMI(*I->getParent(), I->getIterator(), DebugLoc(), TII->get(Opcode),
              I->getOperand(0).getReg().asMCReg())
          .add(Op1)
          .add(Op2);
    } else {
      llvm_unreachable("AMi error: unable to nullify unwanted side-effects in "
                       "mimicry mode!");
    }
  }
}

bool RISCVAMiLinearizeRegion::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(errs() << "AMi Linearize Region Pass\n");

  const auto &ST = MF.getSubtarget<RISCVSubtarget>();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();
  PA = &getAnalysis<PersistencyAnalysisPass>();

  SRA = &getAnalysis<SensitiveRegionAnalysis>();
  const auto *MRI = SRA->getRegionInfo();
  ActivatingBranches = SmallVector<SensitiveBranch>(SRA->sensitive_branches());

  std::sort(ActivatingBranches.begin(), ActivatingBranches.end(),
            std::greater<SensitiveBranch>());

  handleRegion(MRI->getTopLevelRegion());

  for (auto &Branch : ActivatingBranches) {
    setBranchActivating(*Branch.MBB);
    // if (Branch.FlowBlock)
    //   setBranchActivating(*Branch.FlowBlock);

    if (Branch.IfRegion) {
      SmallVector<MachineBasicBlock *> Exitings;
      Branch.IfRegion->getExitingBlocks(Exitings);
      for (auto *Exiting : Exitings)
        setBranchActivating(*Exiting, Branch.IfRegion->getExit());
      handleRegion(Branch.IfRegion);
    }
    if (Branch.ElseRegion) {
      SmallVector<MachineBasicBlock *> Exitings;
      Branch.ElseRegion->getExitingBlocks(Exitings);
      for (auto *Exiting : Exitings)
        setBranchActivating(*Exiting, Branch.ElseRegion->getExit());
      handleRegion(Branch.ElseRegion);
    }
  }

  return true;
}

RISCVAMiLinearizeRegion::RISCVAMiLinearizeRegion() : MachineFunctionPass(ID) {
  initializeRISCVAMiLinearizeRegionPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(RISCVAMiLinearizeRegion, DEBUG_TYPE,
                      "AMi Linearize Region", false, false)
// INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(SensitiveRegionAnalysis)
INITIALIZE_PASS_DEPENDENCY(PersistencyAnalysisPass)
INITIALIZE_PASS_END(RISCVAMiLinearizeRegion, DEBUG_TYPE, "AMi Linearize Region",
                    false, false)

namespace llvm {

FunctionPass *createRISCVAMiLinearizeRegionPass() {
  return new RISCVAMiLinearizeRegion();
}

} // namespace llvm

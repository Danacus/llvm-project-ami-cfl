#include "MCTargetDesc/RISCVMCTargetDesc.h"
#include "RISCV.h"
#include "RISCVInstrInfo.h"
#include "RISCVMolnarLinearizeRegion.h"
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

#define DEBUG_TYPE "riscv-molnar-linearize-region"

char RISCVMolnarLinearizeRegion::ID = 0;

void RISCVMolnarLinearizeRegion::handleRegion(MachineBasicBlock *BranchBlock,
                                              MachineRegion *Region,
                                              Register TakenReg) {
  errs() << "Handling region " << *Region << "\n";

  // also TODO: make sure taken value is stored to global before function
  // call

  if (BranchBlock) {
    TII->removeBranch(*BranchBlock);
    BranchBlock->removeSuccessor(Region->getExit());

    if (BranchBlock->getSingleSuccessor() != BranchBlock->getFallThrough(true))
      TII->insertUnconditionalBranch(
          *BranchBlock, BranchBlock->getSingleSuccessor(), DebugLoc());
  }

  for (MachineInstr *MI : PA->getPersistentInstructions(Region)) {
    MI->dump();
    // setQualifier<RISCV::Molnar::Persistent>(MI);
  }

  errs() << "Stores\n";
  for (MachineInstr *I : PA->getPersistentStores(Region)) {
    I->dump();
    // TODO: fix this to only allow writing to stack in mimicry mode
    // if this instruction originates from calling convention lowering.
    // if (I->getOperand(1).getReg().asMCReg() == RISCV::X2)
    //   continue;

    // MachineInstr &GhostLoad = *std::prev(I->getIterator());

    // if (GhostLoad.getOpcode() == RISCV::GLW) {
    //   assert(GhostLoad.getOperand(0).getReg() == I->getOperand(0).getReg() &&
    //          "Molnar error: invalid ghost load");
    //   continue;
    // }

    // assert(GhostLoad.getOpcode() == TargetOpcode::GHOST_LOAD &&
    //        "Molnar error: expected GHOST_LOAD pseudo");
    // assert(GhostLoad.getOperand(0).getReg() == I->getOperand(0).getReg() &&
    //        "Molnar error: invalid GHOST_LOAD");

    // BuildMI(*I->getParent(), GhostLoad.getIterator(), DebugLoc(),
    //         TII->get(RISCV::ADDI),
    //         GhostLoad.getOperand(0).getReg().asMCReg())
    //     .add(GhostLoad.getOperand(1))
    //     .addImm(0);
    // GhostLoad.eraseFromParent();

    // if (I->getNumOperands() > 2 && I->getOperand(0).isReg()) {
    //   MachineOperand Op1 = I->getOperand(1);
    //   Op1.setIsKill(false);
    //   MachineOperand Op2 = I->getOperand(2);
    //   BuildMI(*I->getParent(), I->getIterator(), DebugLoc(),
    //           TII->get(RISCV::GLW), I->getOperand(0).getReg().asMCReg())
    //       .add(Op1)
    //       .add(Op2);
    // } else {
    //   llvm_unreachable(
    //       "Molnar error: unable to nullify unwanted side-effects in "
    //       "mimicry mode!");
    // }
  }
}

bool RISCVMolnarLinearizeRegion::runOnMachineFunction(MachineFunction &MF) {
  errs() << "Molnar Linearize Region Pass\n";

  const auto &ST = MF.getSubtarget<RISCVSubtarget>();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();
  PA = &getAnalysis<PersistencyAnalysisPass>();

  SRA = &getAnalysis<SensitiveRegionAnalysis>();
  const auto &MRI = getAnalysis<MachineRegionInfoPass>().getRegionInfo();
  ActivatingBranches = SmallVector<SensitiveBranch>(SRA->sensitive_branches());

  GlobalTaken = MF.getFunction().getParent()->getNamedValue("cfl_taken");
  assert(GlobalTaken && "Expected global taken variable");
  GlobalDummy = MF.getFunction().getParent()->getNamedValue("cfl_dummy");
  assert(GlobalDummy && "Expected global dummy address");

  // std::sort(ActivatingBranches.begin(), ActivatingBranches.end(),
  //           std::greater<SensitiveBranch>());
  std::sort(ActivatingBranches.begin(), ActivatingBranches.end());

  MRI.dump();

  // TODO Load global taken value
  Register TopTakenReg =
      MF.getRegInfo().createVirtualRegister(&RISCV::GPRRegClass);
  // Register TempReg =
  //     MF.getRegInfo().createVirtualRegister(&RISCV::GPRRegClass);
  // BuildMI(*MF.begin(), MF.begin()->getFirstTerminator(), DebugLoc(),
  //         TII->get(RISCV::LUI), TempReg)
  //     .addGlobalAddress(GlobalTaken);
  BuildMI(*MF.begin(), MF.begin()->getFirstTerminator(), DebugLoc(),
          TII->get(RISCV::PseudoLI), TopTakenReg)
      // .addReg(TempReg)
      // .addReg(RISCV::X0)
      .addGlobalAddress(GlobalTaken);
  handleRegion(nullptr, MRI.getTopLevelRegion(), TopTakenReg);

  for (auto &Branch : ActivatingBranches) {
    errs() << "Branch:\n";
    Branch.MBB->dump();
    Branch.IfRegion->dump();
    errs() << Branch.IfRegion->getDepth() << "\n";

    SmallVector<MachineOperand, 8> CondReversed = SmallVector<MachineOperand>(Branch.Cond);
    TII->reverseBranchCondition(CondReversed);
    Register CondReg = TII->materializeBranchCondition(
        Branch.MBB->getFirstTerminator(), CondReversed, MF.getRegInfo());
    Register TakenReg =
        MF.getRegInfo().createVirtualRegister(&RISCV::GPRRegClass);

    auto *ParentRegion = SRA->getSensitiveRegion(Branch.MBB);

    Register IncomingTaken;
    if (ParentRegion) {
      errs() << "here\n";
      ParentRegion->dump();
      assert(TakenRegMap[ParentRegion].isValid() &&
             "No taken reg for parent region");
      IncomingTaken = TakenRegMap[ParentRegion];
    } else {
      IncomingTaken = TopTakenReg;
    }
    BuildMI(*Branch.MBB, Branch.MBB->getFirstTerminator(), DebugLoc(),
            TII->get(RISCV::AND), TakenReg)
        .addReg(CondReg)
        .addReg(IncomingTaken);

    // Harden branch regions: remove branches and make stores conditional
    if (Branch.IfRegion) {
      TakenRegMap.insert({Branch.IfRegion, TakenReg});
      handleRegion(Branch.MBB, Branch.IfRegion, TakenReg);
    }

    if (Branch.ElseRegion) {
      assert(Branch.FlowBlock && "Expected flow block");
      Register InvCondReg =
          MF.getRegInfo().createVirtualRegister(&RISCV::GPRRegClass);
      Register InvTakenReg =
          MF.getRegInfo().createVirtualRegister(&RISCV::GPRRegClass);
      TakenRegMap.insert({Branch.ElseRegion, InvTakenReg});
      // XOR with 1 to invert
      BuildMI(*Branch.FlowBlock, Branch.FlowBlock->getFirstTerminator(),
              DebugLoc(), TII->get(RISCV::XORI), InvCondReg)
          .addReg(TakenReg)
          .addImm(1);
      BuildMI(*Branch.FlowBlock, Branch.FlowBlock->getFirstTerminator(), DebugLoc(),
              TII->get(RISCV::AND), InvTakenReg)
          .addReg(InvCondReg)
          .addReg(IncomingTaken);
      handleRegion(Branch.FlowBlock, Branch.ElseRegion, InvTakenReg);
    }
  }

  for (auto &Branch : ActivatingBranches) {
    // Replace PHI instructions with conditional selection
    MachineBasicBlock *Exit = Branch.IfRegion->getExit();
    if (Branch.ElseRegion)
      Exit = Branch.ElseRegion->getExit();
    SmallPtrSet<MachineInstr *, 8> ToRemove;

    for (MachineBasicBlock::iterator I = Exit->begin();
         I != Exit->getFirstNonPHI(); ++I) {
      SmallVector<uint> OpsToRemove;
      auto SelectMI =
          BuildMI(*Exit, Exit->getFirstNonPHI(), DebugLoc(),
                  TII->get(TargetOpcode::CT_SELECT), I->getOperand(0).getReg());
      uint Counter = 1;
      for (MachineInstr::mop_iterator J = std::next(I->operands_begin());
           J != I->operands_end(); J += 2) {
        Register Reg = J->getReg();
        SelectMI.addReg(Reg);
        MachineBasicBlock *MBB = std::next(J)->getMBB();
        errs() << "here\n";
        MBB->dump();
        auto *ParentRegion = SRA->getSensitiveRegion(MBB);
        // assert(ParentRegion);
        // if (ParentRegion) {
        // Register CondReg = TakenRegMap[ParentRegion];
        Register CondReg;
        if (ParentRegion) {
          ParentRegion->dump();
          CondReg = TakenRegMap[ParentRegion];
        } else {
          errs() << "No parent region\n";
          CondReg = TopTakenReg;
        }
        assert(CondReg.isValid());
        SelectMI.addReg(CondReg);
        OpsToRemove.push_back(Counter++);
        OpsToRemove.push_back(Counter++);
        // }
      }

      for (auto Idx = OpsToRemove.rbegin(); Idx != OpsToRemove.rend(); ++Idx) {
        I->removeOperand(*Idx);
      }
      
      if (I->getNumOperands() == 1)
        ToRemove.insert(&*I);
    }

    for (auto *MI : ToRemove) {
      MI->eraseFromParent();
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

RISCVMolnarLinearizeRegion::RISCVMolnarLinearizeRegion()
    : MachineFunctionPass(ID) {
  initializeRISCVMolnarLinearizeRegionPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(RISCVMolnarLinearizeRegion, DEBUG_TYPE,
                      "Molnar Linearize Region", false, false)
// INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(SensitiveRegionAnalysis)
INITIALIZE_PASS_DEPENDENCY(PersistencyAnalysisPass)
INITIALIZE_PASS_END(RISCVMolnarLinearizeRegion, DEBUG_TYPE,
                    "Molnar Linearize Region", false, false)

namespace llvm {

FunctionPass *createRISCVMolnarLinearizeRegionPass() {
  return new RISCVMolnarLinearizeRegion();
}

} // namespace llvm

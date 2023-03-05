#include "MCTargetDesc/RISCVBaseInfo.h"
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

  for (MachineInstr *I : PA->getPersistentStores(Region)) {
    Register LoadedReg = RegInfo->createVirtualRegister(&RISCV::GPRRegClass);
    Register StoredReg = RegInfo->createVirtualRegister(&RISCV::GPRRegClass);
    // TODO: support LB and LH
    auto NewMI = BuildMI(*I->getParent(), I->getIterator(), DebugLoc(), TII->get(RISCV::LW),
            LoadedReg)
        .add(I->getOperand(1))
        .add(I->getOperand(2));
    NewMI->getOperand(1).setIsKill(false);
    TII->createCTSelect(StoredReg, I->getParent(), I->getIterator(), TakenReg,
                        I->getOperand(0).getReg(), LoadedReg, *RegInfo);
    I->getOperand(0).setReg(StoredReg);
  }

  for (MachineInstr *I : PA->getCallInstrs(Region)) {
    BuildMI(*I->getParent(), I->getIterator(), DebugLoc(), TII->get(RISCV::SW))
        .addReg(TakenReg)
        .addReg(GlobalTakenAddrReg)
        .addGlobalAddress(GlobalTaken, 0, RISCVII::MO_LO);
  }
}

Register RISCVMolnarLinearizeRegion::loadTakenReg(MachineFunction &MF) {
  errs() << "loadTakenReg\n";
  // Setup global variable with external linkage
  Module *Mod = MF.getFunction().getParent();
  Mod->getOrInsertGlobal("cfl_taken", Type::getInt32Ty(Mod->getContext()));
  GlobalTaken = Mod->getNamedGlobal("cfl_taken");
  GlobalTaken->setLinkage(GlobalValue::LinkageTypes::ExternalLinkage);

  // Load global taken value
  GlobalTakenAddrReg = MF.getRegInfo().createVirtualRegister(&RISCV::GPRRegClass);
  Register TopTakenReg =
      MF.getRegInfo().createVirtualRegister(&RISCV::GPRRegClass);

  MachineBasicBlock::iterator InsertPoint = MF.begin()->begin();
  BuildMI(*MF.begin(), InsertPoint, DebugLoc(), TII->get(RISCV::LUI), GlobalTakenAddrReg)
      .addGlobalAddress(GlobalTaken, 0, RISCVII::MO_HI);
  BuildMI(*MF.begin(), InsertPoint, DebugLoc(), TII->get(RISCV::LW),
          TopTakenReg)
      .addReg(GlobalTakenAddrReg)
      .addGlobalAddress(GlobalTaken, 0, RISCVII::MO_LO);
  return TopTakenReg;
}

void RISCVMolnarLinearizeRegion::replacePHIInstructions() {
  for (auto &Branch : ActivatingBranches) {
    // Replace PHI instructions with conditional selection
    MachineBasicBlock *Exit = Branch.IfRegion->getExit();
    if (Branch.ElseRegion)
      Exit = Branch.ElseRegion->getExit();
    SmallPtrSet<MachineInstr *, 8> ToRemove;

    for (MachineBasicBlock::iterator I = Exit->begin();
         I != Exit->getFirstNonPHI(); ++I) {
      SmallVector<uint> OpsToRemove;
      uint Counter = 1;
      Register CondReg;
      uint CurrentDepth = 0;
      Register First;
      Register Second;

      for (MachineInstr::mop_iterator J = std::next(I->operands_begin());
           J != I->operands_end(); J += 2) {
        Register Reg = J->getReg();
        if (Counter == 1) {
          First = Reg;
        } else if (Counter == 3) {
          Second = Reg;
        } else {
          llvm_unreachable("CT_SELECT with more than 2 operands not supported");
        }
        MachineBasicBlock *MBB = std::next(J)->getMBB();
        LLVM_DEBUG(errs() << "here\n");
        MBB->dump();
        auto *ParentRegion = SRA->getSensitiveRegion(MBB);

        if (ParentRegion && ParentRegion->getDepth() > CurrentDepth) {
          // Only replace the condition register if the parent region is deeper
          ParentRegion->dump();
          CondReg = TakenRegMap[ParentRegion];
          CurrentDepth = ParentRegion->getDepth();

          if (Counter > 1) {
            // We are using the condition of the second option,
            // so we need to flip the two options such that the one
            // selected for this condition comes first
            Register Temp = Second;
            Second = First;
            First = Temp;
          }
        } else {
          LLVM_DEBUG(errs() << "No parent region\n");
          // No need to set CondReg, as it is assumed to be set for the next
          // option
        }
        OpsToRemove.push_back(Counter++);
        OpsToRemove.push_back(Counter++);
      }

      MachineBasicBlock::iterator InsertPoint = Exit->getFirstNonPHI();
      if (InsertPoint == Exit->begin())
        ++InsertPoint;
      TII->createCTSelect(I->getOperand(0).getReg(), Exit, InsertPoint, CondReg,
                          First, Second, *RegInfo);

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
}

void RISCVMolnarLinearizeRegion::linearizeBranches(MachineFunction &MF) {
  Register TopTakenReg = loadTakenReg(MF);
  handleRegion(nullptr, MRI->getTopLevelRegion(), TopTakenReg);

  for (auto &Branch : ActivatingBranches) {
    SmallVector<MachineOperand, 8> CondReversed =
        SmallVector<MachineOperand>(Branch.Cond);
    TII->reverseBranchCondition(CondReversed);
    Register CondReg = TII->materializeBranchCondition(
        Branch.MBB->getFirstTerminator(), CondReversed, *RegInfo);
    Register CondMaskReg = RegInfo->createVirtualRegister(&RISCV::GPRRegClass);

    BuildMI(*Branch.MBB, Branch.MBB->getFirstTerminator(), DebugLoc(),
            TII->get(RISCV::SUB), CondMaskReg)
        .addReg(RISCV::X0)
        .addReg(CondReg);
    Register TakenReg = RegInfo->createVirtualRegister(&RISCV::GPRRegClass);

    auto *ParentRegion = SRA->getSensitiveRegion(Branch.MBB);

    Register IncomingTaken;
    if (ParentRegion) {
      assert(TakenRegMap[ParentRegion].isValid() &&
             "No taken reg for parent region");
      IncomingTaken = TakenRegMap[ParentRegion];
    } else {
      IncomingTaken = TopTakenReg;
    }
    BuildMI(*Branch.MBB, Branch.MBB->getFirstTerminator(), DebugLoc(),
            TII->get(RISCV::AND), TakenReg)
        .addReg(CondMaskReg)
        .addReg(IncomingTaken);

    // Harden branch regions: remove branches and make stores conditional
    if (Branch.IfRegion) {
      TakenRegMap.insert({Branch.IfRegion, TakenReg});
      handleRegion(Branch.MBB, Branch.IfRegion, TakenReg);
    }

    if (Branch.ElseRegion) {
      assert(Branch.FlowBlock && "Expected flow block");
      Register InvCondReg = RegInfo->createVirtualRegister(&RISCV::GPRRegClass);
      Register InvTakenReg =
          RegInfo->createVirtualRegister(&RISCV::GPRRegClass);
      TakenRegMap.insert({Branch.ElseRegion, InvTakenReg});
      // XOR with -1 for NOT operation, inverts mask
      BuildMI(*Branch.FlowBlock, Branch.FlowBlock->getFirstTerminator(),
              DebugLoc(), TII->get(RISCV::XORI), InvCondReg)
          .addReg(TakenReg)
          .addImm(-1);
      BuildMI(*Branch.FlowBlock, Branch.FlowBlock->getFirstTerminator(),
              DebugLoc(), TII->get(RISCV::AND), InvTakenReg)
          .addReg(InvCondReg)
          .addReg(IncomingTaken);
      handleRegion(Branch.FlowBlock, Branch.ElseRegion, InvTakenReg);
    }
  }

  MachinePostDominatorTree &MPDT = getAnalysis<MachinePostDominatorTree>();
  MPDT.dump();
  
  for (MachineBasicBlock *RetBlock : MPDT.getBase().roots()) {
    BuildMI(*RetBlock, RetBlock->getFirstTerminator(), DebugLoc(), TII->get(RISCV::SW))
        .addReg(TopTakenReg)
        .addReg(GlobalTakenAddrReg)
        .addGlobalAddress(GlobalTaken, 0, RISCVII::MO_LO);
  }
}

bool RISCVMolnarLinearizeRegion::runOnMachineFunction(MachineFunction &MF) {
  LLVM_DEBUG(errs() << "Molnar Linearize Region Pass\n");

  const auto &ST = MF.getSubtarget<RISCVSubtarget>();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();
  PA = &getAnalysis<PersistencyAnalysisPass>();

  RegInfo = &MF.getRegInfo();

  SRA = &getAnalysis<SensitiveRegionAnalysis>();
  MRI = SRA->getRegionInfo();
  TakenRegMap.clear();
  ActivatingBranches = SmallVector<SensitiveBranch>(SRA->sensitive_branches());

  std::sort(ActivatingBranches.begin(), ActivatingBranches.end());

  linearizeBranches(MF);
  replacePHIInstructions();

  LLVM_DEBUG(MF.dump());

  return true;
}

RISCVMolnarLinearizeRegion::RISCVMolnarLinearizeRegion()
    : MachineFunctionPass(ID) {
  initializeRISCVMolnarLinearizeRegionPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(RISCVMolnarLinearizeRegion, DEBUG_TYPE,
                      "Molnar Linearize Region", false, false)
INITIALIZE_PASS_DEPENDENCY(SensitiveRegionAnalysis)
INITIALIZE_PASS_DEPENDENCY(PersistencyAnalysisPass)
INITIALIZE_PASS_END(RISCVMolnarLinearizeRegion, DEBUG_TYPE,
                    "Molnar Linearize Region", false, false)

namespace llvm {

FunctionPass *createRISCVMolnarLinearizeRegionPass() {
  return new RISCVMolnarLinearizeRegion();
}

} // namespace llvm

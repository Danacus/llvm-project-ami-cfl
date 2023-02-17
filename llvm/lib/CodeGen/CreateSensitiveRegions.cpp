
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/CreateSensitiveRegions.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/IR/PassManager.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

#define DEBUG_TYPE "create-sensitive-regions"

bool CreateSensitiveRegions::runOnMachineFunction(MachineFunction &MF) {
  MRI = &getAnalysisIfAvailable<MachineRegionInfoPass>()->getRegionInfo();
  MDT = getAnalysisIfAvailable<MachineDominatorTree>();
  MPDT = getAnalysisIfAvailable<MachinePostDominatorTree>();
  MDF = getAnalysisIfAvailable<MachineDominanceFrontier>();
  SRA = &getAnalysis<SensitiveRegionAnalysis>();
  auto *LV = getAnalysisIfAvailable<LiveVariables>();
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  bool MadeChange = false;

  MRI->dump();
  
  SmallVector<MachineBasicBlock *> ToUpdate;

  for (auto &Branch : SRA->sensitive_branches()) {
    auto *Exit = Branch.IfRegion->getExit();

    if (!Branch.ElseRegion) {
      errs() << "Creating else region for \n";
      Branch.IfRegion->dump();

      auto *ElseMBB = MF.CreateMachineBasicBlock();
      TII->removeBranch(*Branch.MBB);
      TII->insertBranch(*Branch.MBB, ElseMBB, Branch.IfRegion->getEntry(), Branch.Cond, DebugLoc());
      Branch.MBB->removeSuccessor(Exit);
      Branch.MBB->addSuccessor(ElseMBB);
      TII->insertUnconditionalBranch(*ElseMBB, Branch.IfRegion->getExit(), DebugLoc());
      ElseMBB->addSuccessor(Exit);
      // MF.insert(Exit->getIterator(), ElseMBB);
      MF.insert(MF.end(), ElseMBB);
      MadeChange = true;
      ToUpdate.push_back(Branch.MBB);

      if (MDT)
        MDT->addNewBlock(ElseMBB, Branch.MBB);
      if (MPDT)
        MPDT->getBase().addNewBlock(ElseMBB, Exit);
      if (MDF)
        MDF->addBasicBlock(ElseMBB, { Exit });

      if (MRI) {
        MachineRegion *MR = new MachineRegion(ElseMBB, Exit, MRI, MDT);
        MRI->setRegionFor(ElseMBB, MR);
        MRI->updateStatistics(MR);
        // if (Branch.IfRegion->getParent())
        // Branch.IfRegion->getParent()->addSubRegion(MR);
        MRI->getRegionFor(Branch.MBB)->addSubRegion(MR);
      }

      if (LV)
        LV->addNewBlock(ElseMBB, Branch.MBB, Exit);

      BuildMI(*ElseMBB, ElseMBB->begin(), DebugLoc(), TII->get(TargetOpcode::EXTEND));
    }

    // BuildMI(*Exit, Exit->begin(), DebugLoc(), TII->get(TargetOpcode::BRANCH_TARGET)).addMBB(Branch.MBB);
  }

  for (auto *MBB : ToUpdate) {
    SRA->handleBranch(MBB);
  }

  // for (auto &Branch : SRA->sensitive_branches()) {
  //   BuildMI(*Branch.MBB, Branch.MBB->getFirstTerminator(), DebugLoc(), TII->get(TargetOpcode::SECRET_DEP_BR));
  // }

  MF.dump();
  
  return MadeChange;
}

char CreateSensitiveRegions::ID = 0;
char &llvm::CreateSensitiveRegionsID = CreateSensitiveRegions::ID;

CreateSensitiveRegions::CreateSensitiveRegions()
    : MachineFunctionPass(ID) {
  initializeCreateSensitiveRegionsPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(CreateSensitiveRegions, DEBUG_TYPE,
                      "Create Sensitive Regions", true, false)
// INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(SensitiveRegionAnalysis)
// INITIALIZE_PASS_DEPENDENCY(MachineDominatorTree)
// INITIALIZE_PASS_DEPENDENCY(MachinePostDominatorTree)
// INITIALIZE_PASS_DEPENDENCY(MachineDominanceFrontier)
INITIALIZE_PASS_END(CreateSensitiveRegions, DEBUG_TYPE,
                    "Create Sensitive Regions", true, false)

namespace llvm {

FunctionPass *createCreateSensitiveRegionsPass() {
  return new CreateSensitiveRegions();
}

} // namespace llvm

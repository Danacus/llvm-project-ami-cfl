#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/CodeGen/TargetOpcodes.h"
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

#define DEBUG_TYPE "find-secrets"

char TrackSecretsAnalysis::ID = 0;

char &llvm::TrackSecretsPassID = TrackSecretsAnalysis::ID;

SmallSet<Secret, 8>
TrackSecretsAnalysis::findSecretSources(MachineFunction &MF) {
  auto &RDA = getAnalysis<ReachingDefAnalysis>();
  SmallSet<Secret, 8> SecretDefs;

  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      if (MI.getOpcode() == TargetOpcode::SECRET) {
        uint64_t SecretMask = MI.getOperand(1).getImm();
        MCRegister Reg = MI.getOperand(0).getReg().asMCReg();

        SmallPtrSet<MachineInstr *, 16> Defs;
        RDA.getGlobalReachingDefs(&MI, Reg, Defs);

        // If there are no reaching defs, we assume that the register is an
        // argument and we need to start tracking at the SECRET
        // pseudoinstruction
        if (Defs.empty()) {
          SecretDefs.insert(Secret(&MI, Reg, SecretMask));
        }

        // If there are reaching defs, then we need to make sure to start
        // tracking at the reaching def and not the SECRET pseudoinstruction
        for (MachineInstr *DefMI : Defs) {
          SecretDefs.insert(Secret(DefMI, Reg, SecretMask));
        }
      }
    }
  }

  return SecretDefs;
}

void TrackSecretsAnalysis::handleUse(MachineInstr *UseInst, MCRegister Reg,
                                     uint64_t SecretMask,
                                     SmallSet<Secret, 8> &SecretDefs) {
  //errs() << "handleUse: " << *UseInst << "\n";
  
  if (UseInst->mayLoad() && (SecretMask & (1u << 1))) {
    // TODO: used register(s) are assumed to be dereferenced to get an address to load from
    // Need to use information from TargetInstrInfo
    
    for (MachineOperand &Def : UseInst->defs()) {
      //errs() << "Push: " << *UseInst << "\n";
      SecretDefs.insert(Secret(UseInst, Def.getReg(), SecretMask >> 1));
    }
  }
  
  if (!UseInst->mayLoadOrStore() && (SecretMask & 1u)) {
    // Default behavior for most instructions that don't store or load (register to register)
    for (MachineOperand &Def : UseInst->defs()) {
      //errs() << "Push: " << *UseInst << "\n";
      SecretDefs.insert(Secret(UseInst, Def.getReg(), SecretMask));
    }
  }
}

bool TrackSecretsAnalysis::runOnMachineFunction(MachineFunction &MF) {
  SmallSet<Secret, 8> WorkSet = findSecretSources(MF);

  auto &RDA = getAnalysis<ReachingDefAnalysis>();

  while (!WorkSet.empty()) {
    auto Current = *WorkSet.begin();
    WorkSet.erase(Current);
    
    //errs() << "Pop: " << *Current.MI << "\n";

    SmallPtrSet<MachineInstr *, 8> Uses;
    RDA.getGlobalUses(Current.MI, Current.Reg, Uses);

    for (auto *Use : Uses) {
      if (Current.SecretMask & 1u)
        Secrets.insert(Secret(Use, Current.Reg, Current.SecretMask, false));
      handleUse(Use, Current.Reg, Current.SecretMask, WorkSet);
    }
  }
  
  errs() << "Secret registers: \n";
  
  for (auto S : Secrets) {
    errs() << *S.MI << ": " << S.Reg << "\n";
  }

  return false;
}

TrackSecretsAnalysis::TrackSecretsAnalysis()
    : MachineFunctionPass(ID) {
  initializeMachineCFGPrinterPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(TrackSecretsAnalysis, DEBUG_TYPE, "Find Secrets", false,
                      false)
INITIALIZE_PASS_DEPENDENCY(ReachingDefAnalysis)
INITIALIZE_PASS_END(TrackSecretsAnalysis, DEBUG_TYPE, "Find Secrets", false,
                    false)
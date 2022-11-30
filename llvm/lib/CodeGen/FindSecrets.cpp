#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/FindSecrets.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/ReachingDefAnalysis.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetOpcodes.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
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

#define DEBUG_TYPE "track-secrets"

char TrackSecretsAnalysis::ID = 0;

char &llvm::TrackSecretsPassID = TrackSecretsAnalysis::ID;

TrackSecretsAnalysis::SecretsSet
TrackSecretsAnalysis::findSecretSources(MachineFunction &MF) {
  Function &F = MF.getFunction();
  const Module *M = F.getParent();

  if (GlobalVariable *GA = M->getGlobalVariable("llvm.global.annotations")) {
    for (Value *AOp : GA->operands()) {
      ConstantArray *CA = dyn_cast<ConstantArray>(AOp);
      if (!CA)
        continue;

      for (Value *CAOp : CA->operands()) {
        ConstantStruct *CS = dyn_cast<ConstantStruct>(CAOp);
        if (!CS || CS->getNumOperands() < 2)
          continue;

        GlobalVariable *GV = dyn_cast<GlobalVariable>(CS->getOperand(0));
        if (!GV)
          continue;

        GlobalVariable *GAnn = dyn_cast<GlobalVariable>(CS->getOperand(1));
        if (!GAnn)
          continue;

        ConstantDataArray *A = dyn_cast<ConstantDataArray>(GAnn->getOperand(0));
        if (!A)
          continue;

        StringRef AS = A->getAsString();

        if (!AS.consume_front("secret("))
          continue;
        uint64_t Mask;
        if (AS.consumeInteger(10, Mask))
          continue;
        if (!AS.consume_front(")"))
          continue;

        SecretGlobals[GV] = Mask;
      }
    }
  }

  auto &RDA = getAnalysis<ReachingDefAnalysis>();
  SecretsSet SecretDefs;

  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      if (MI.getOpcode() == TargetOpcode::SECRET) {
        uint64_t SecretMask = MI.getOperand(1).getImm();
        Register Reg = MI.getOperand(0).getReg();

        SmallPtrSet<MachineInstr *, 16> Defs;
        RDA.getGlobalReachingDefs(&MI, Reg, Defs);

        // If there are no reaching defs, we assume that the register is an
        // argument and we need to start tracking at the SECRET
        // pseudoinstruction
        if (Defs.empty()) {
          Secrets[SecretRegisterDef(Reg, &MI)] = SecretMask;
        }

        // If there are reaching defs, then we need to make sure to start
        // tracking at the reaching def and not the SECRET pseudoinstruction
        for (MachineInstr *DefMI : Defs) {
          Secrets[SecretRegisterDef(Reg, DefMI)] = SecretMask;
        }
      }
    }
  }

  return SecretDefs;
}

void TrackSecretsAnalysis::handleUse(MachineInstr &UseInst, MachineOperand *MO,
                                     uint64_t SecretMask, SecretsSet &WorkSet,
                                     SecretsMap &SecretDefs) {
  // errs() << "handleUse: " << UseInst << "\n";
  SmallSet<std::pair<Register, uint64_t>, 8> NewDefs;

  // Transfer the operand
  TII->transferSecret(UseInst, MO, SecretMask, NewDefs);
  
  for (auto Def : NewDefs) {
    SecretDef NewDef = SecretRegisterDef(Def.first, &UseInst);

    uint64_t NewMask = Def.second | SecretDefs[NewDef];

    if (NewMask != SecretDefs[NewDef]) {
      // Add newly defined registers to work set
      WorkSet.insert(NewDef);
       
      // Set or upgrade the mask
      SecretDefs[NewDef] = NewMask;
    }
    
    // errs() << "Add def: " << printReg(Def.first, TRI) << " " << Def.second << "\n";
  }
}

bool TrackSecretsAnalysis::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();
  
  MF.dump();

  SecretsSet WorkSet;
  findSecretSources(MF);
  
  DenseMap<SecretDef, uint64_t> TestMap;  

  auto &RDA = getAnalysis<ReachingDefAnalysis>();
  
  // Step 1: propagate secret arguments and globals to instructions

  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      for (auto Def : Secrets) {
        SmallPtrSet<MachineInstr *, 8> Defs;
        RDA.getGlobalReachingDefs(&MI, std::get<SecretRegisterDef>(Def.first).getReg().asMCReg(), Defs);
        
        if (Defs.empty()) {
          auto *OP = MI.findRegisterUseOperand(std::get<SecretRegisterDef>(Def.first).getReg());
          if (OP)
            handleUse(MI, OP, Def.second, WorkSet, Secrets);
        }
      }
      for (auto P : SecretGlobals) {
        auto *OP =
            std::find_if(MI.operands_begin(), MI.operands_end(),
                      [&P](MachineOperand O) {
                        return O.isGlobal() && O.getGlobal()->getName() == P.first->getName();
                      });
        if (OP != MI.operands_end())
          handleUse(MI, OP, P.second, WorkSet, Secrets);
      }
    }
  }
  
  // Step 2: iteratively propagate secrets from def to uses
  
  while (!WorkSet.empty()) {
    auto Current = *WorkSet.begin();
    WorkSet.erase(Current);
    
    SecretRegisterDef CurrentAsRegDef = std::get<SecretRegisterDef>(Current);

    SmallPtrSet<MachineInstr *, 8> Uses;
    RDA.getGlobalUses(CurrentAsRegDef.getMI(), CurrentAsRegDef.getReg(), Uses);

    for (auto *Use : Uses) {
      if (Secrets[Current] & 1u)
        SecretUses[SecretRegisterDef(CurrentAsRegDef.getReg(), Use)] = Secrets[Current];

      auto *OP = Use->findRegisterUseOperand(CurrentAsRegDef.getReg());
      if (OP)
        handleUse(*Use, OP, Secrets[Current], WorkSet, Secrets);
    }
  }
  
  // Step 3: verify secret types, check for errors
  
  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      DenseMap<MachineOperand *, uint64_t> MaskMap;

      // Handle registers coming from SECRET instruction separately,
      // because they don't count as a reaching def. If there is a closer
      // reaching def, this will be overwritten.
      // Essentially, they are hacked to be a reaching def for each instruction,
      // but at the lowest possible priority ("infinitely far away").
      for (auto Def : Secrets) {
        auto RegDef = std::get<SecretRegisterDef>(Def.first);
        if (RegDef.getMI()->getOpcode() == TargetOpcode::SECRET) {
          auto *OP = MI.findRegisterUseOperand(RegDef.getReg());
          if (OP)
            MaskMap[OP] = Def.second;
        }
      }

      for (auto Def : Secrets) {
        auto RegDef = std::get<SecretRegisterDef>(Def.first);
        SmallPtrSet<MachineInstr *, 8> Defs;
        RDA.getGlobalReachingDefs(&MI, RegDef.getReg().asMCReg(), Defs);

        // Check if the the SecretDef we found is relevant in this context,
        // by making sure the defining instruction is the closest reaching def of
        // UseInst
        if (Defs.contains(RegDef.getMI())) {
          auto *OP = MI.findRegisterUseOperand(RegDef.getReg());
          if (OP)
            MaskMap[OP] = Def.second;
        }
      }

      for (auto P : SecretGlobals) {
        auto *OP =
            std::find_if(MI.operands_begin(), MI.operands_end(),
                      [&P](MachineOperand O) {
                        return O.isGlobal() && O.getGlobal() == P.first;
                      });
        if (OP != MI.operands_end())
          MaskMap[OP] = P.second;
      }

      TII->verifySecretTypes(MI, MaskMap);
    }
  }

  return false;
}

TrackSecretsAnalysis::TrackSecretsAnalysis() : MachineFunctionPass(ID) {
  initializeTrackSecretsAnalysisPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(TrackSecretsAnalysis, DEBUG_TYPE, "Track Secrets", false,
                      false)
INITIALIZE_PASS_DEPENDENCY(ReachingDefAnalysis)
INITIALIZE_PASS_END(TrackSecretsAnalysis, DEBUG_TYPE, "Track Secrets", false,
                    false)

char TrackSecretsPrinter::ID = 0;

char &llvm::TrackSecretsPrinterID = TrackSecretsPrinter::ID;

bool TrackSecretsPrinter::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  const auto *TRI = ST.getRegisterInfo();
  auto SecretUses = getAnalysis<TrackSecretsAnalysis>().SecretUses;

  for (auto S : SecretUses) {
    if (auto *RegDef = std::get_if<SecretRegisterDef>(&S.first)) {
      errs() << "Secret: ";
      errs() << printReg(RegDef->getReg(), TRI) << " with mask ";
      errs() << std::bitset<64>(S.second).to_string() << " in ";
      errs() << *RegDef->getMI();
    }
  }

  return false;
}

TrackSecretsPrinter::TrackSecretsPrinter() : MachineFunctionPass(ID) {
  initializeTrackSecretsPrinterPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(TrackSecretsPrinter, "secrets-printer", "Secrets Printer",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysis)
INITIALIZE_PASS_END(TrackSecretsPrinter, "secrets-printer", "Secrets Printer",
                    false, false)
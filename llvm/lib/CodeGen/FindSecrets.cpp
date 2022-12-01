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
  SecretsSet SecretDefs;
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
        auto Def = SecretDef::global(GV);
        Secrets[Def] = Mask;
        SecretDefs.insert(Def);
      }
    }
  }

  auto &RDA = getAnalysis<ReachingDefAnalysis>();

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
          auto Def = SecretDef::argument(Reg);
          Secrets[Def] = SecretMask;
          SecretDefs.insert(Def);
        }

        // If there are reaching defs, then we need to make sure to start
        // tracking at the reaching def and not the SECRET pseudoinstruction
        for (MachineInstr *DefMI : Defs) {
          auto Def = SecretDef::registerDef(Reg, DefMI);
          Secrets[Def] = SecretMask;
          SecretDefs.insert(Def);
        }
      }
    }
  }

  return SecretDefs;
}

void TrackSecretsAnalysis::getUses(MachineFunction &MF, SecretDef &SD, SmallPtrSet<MachineInstr *, 8> &Uses) const {
  auto &RDA = getAnalysis<ReachingDefAnalysis>();

  switch (SD.getKind()) {
    case SecretDef::SDK_SecretRegisterDef: {
      auto S = SD.get<SecretRegisterDef>();
      RDA.getGlobalUses(S.getMI(), S.getReg().asMCReg(), Uses);
      break;
    }
    case SecretDef::SDK_Argument: {
      auto S = SD.get<SecretArgument>();
      for (MachineBasicBlock &MB : MF) {
        for (MachineInstr &MI : MB) {
          SmallPtrSet<MachineInstr *, 8> Defs;
          RDA.getGlobalReachingDefs(&MI, S.getReg().asMCReg(), Defs);
      
          // If there is no closer reaching def, the argument is the reaching def
          if (Defs.empty()) {
            Uses.insert(&MI);
          }
        }
      }
      break;
    }
    case SecretDef::SDK_Global: {
      auto S = SD.get<SecretGlobal>();
      for (MachineBasicBlock &MB : MF) {
        for (MachineInstr &MI : MB) {
          auto *OP =
              std::find_if(MI.operands_begin(), MI.operands_end(),
                        [&S](MachineOperand O) {
                          return O.isGlobal() && O.getGlobal()->getName() == S.getGlobalVar()->getName();
                        });
          if (OP != MI.operands_end())
            Uses.insert(&MI);
        }
      }
      break;
    }
  }
}

void TrackSecretsAnalysis::getReachingDefs(SecretDef &SD, SmallPtrSet<MachineInstr *, 8> &Defs) const {
  auto &RDA = getAnalysis<ReachingDefAnalysis>();

  switch (SD.getKind()) {
    case SecretDef::SDK_SecretRegisterDef: {
      auto S = SD.get<SecretRegisterDef>();
      RDA.getGlobalReachingDefs(S.getMI(), S.getReg().asMCReg(), Defs);
      break;
    }
    case SecretDef::SDK_Argument:
    case SecretDef::SDK_Global:
      // Arguments and globals don't have a reaching def within this function
      break;
  }
}

void TrackSecretsAnalysis::handleUse(MachineInstr &UseInst, MachineOperand *MO,
                                     uint64_t SecretMask, SecretsSet &WorkSet,
                                     SecretsMap &SecretDefs) {
  SmallSet<std::pair<Register, uint64_t>, 8> NewDefs;

  // Transfer the operand
  TII->transferSecret(UseInst, MO, SecretMask, NewDefs);
  
  for (auto Def : NewDefs) {
    SecretDef NewDef = SecretDef::registerDef(Def.first, &UseInst);

    uint64_t NewMask = Def.second | SecretDefs[NewDef];

    if (NewMask != SecretDefs[NewDef]) {
      // Add newly defined registers to work set
      WorkSet.insert(NewDef);
       
      // Set or upgrade the mask
      SecretDefs[NewDef] = NewMask;
    }
  }
}

bool TrackSecretsAnalysis::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();
  
  MF.dump();

  // Step 1: find secret arguments and globals
  SecretsSet WorkSet = findSecretSources(MF);
  
  // Step 2: iteratively propagate secrets from def to uses
  while (!WorkSet.empty()) {
    auto Current = *WorkSet.begin();
    WorkSet.erase(Current);
    
    SmallPtrSet<MachineInstr *, 8> Uses;
    getUses(MF, Current, Uses);

    for (auto *Use : Uses) {
      auto SU = SecretUse(Current, Use, Secrets[Current]);
      
      if (Secrets[Current] & 1u)
        SecretUses[{ Use, Current }] = SU;

      for (auto *MO : SU.operands()) {
        handleUse(*Use, MO, Secrets[Current], WorkSet, Secrets);
      }
    }
  }
  
  // Step 3: verify secret types, check for errors
  

  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      DenseMap<MachineOperand *, uint64_t> MaskMap;

      for (auto Use : SecretUses) {
        if (Use.second.getUser() == &MI) {
          for (auto *MO : Use.second.operands()) {
            MaskMap[MO] = Use.second.getSecretMask();
          }
        }

        TII->verifySecretTypes(MI, MaskMap);
      }
    }
  }
  
  /*
  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      DenseMap<MachineOperand *, uint64_t> MaskMap;

      // Handle registers coming from SECRET instruction separately,
      // because they don't count as a reaching def. If there is a closer
      // reaching def, this will be overwritten.
      // Essentially, they are hacked to be a reaching def for each instruction,
      // but at the lowest possible priority ("infinitely far away").
      for (auto Def : Secrets) {
        auto RegDef = Def.first.get<SecretRegisterDef>();
        if (RegDef.getMI()->getOpcode() == TargetOpcode::SECRET) {
          auto *OP = MI.findRegisterUseOperand(RegDef.getReg());
          if (OP)
            MaskMap[OP] = Def.second;
        }
      }

      for (auto Def : Secrets) {
        auto RegDef = Def.first.get<SecretRegisterDef>();
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
  */
  for (auto S : SecretUses) {
    for (auto *MO : S.second.operands()) {
      if (MO) {
        errs() << "Secret: ";
        errs() << MO->getTargetIndexName() << " with mask ";
        errs() << std::bitset<64>(S.second.getSecretMask()).to_string() << " in ";
        errs() << *S.second.getUser();
      } else {
        errs() << "nullptr MO \n";
      }
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
    for (auto *MO : S.second.operands()) {
      errs() << "Secret: ";
      errs() << *MO << " with mask ";
      errs() << std::bitset<64>(S.second.getSecretMask()).to_string() << " in ";
      errs() << *S.second.getUser();
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
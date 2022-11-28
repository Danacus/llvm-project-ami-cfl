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

        if (Mask)
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
          Secrets[SecretDef(&MI, Reg)] = SecretMask;
          SecretDefs.insert(SecretDef(&MI, Reg));
        }

        // If there are reaching defs, then we need to make sure to start
        // tracking at the reaching def and not the SECRET pseudoinstruction
        for (MachineInstr *DefMI : Defs) {
          Secrets[SecretDef(DefMI, Reg)] = SecretMask;
          SecretDefs.insert(SecretDef(DefMI, Reg));
        }
      }
    }
  }

  return SecretDefs;
}

void TrackSecretsAnalysis::handleUse(MachineInstr &UseInst, Register Reg,
                                     uint64_t SecretMask, SecretsSet &WorkSet,
                                     SecretsMap &SecretDefs) {
  errs() << "handleUse: " << UseInst << "\n";
  auto &RDA = getAnalysis<ReachingDefAnalysis>();
  // SmallSet<std::pair<Register, uint64_t>, 8> Map;
  DenseMap<Register, uint64_t> MaskMap;
  SmallSet<std::pair<Register, uint64_t>, 8> NewDefs;

  // Handle registers coming from SECRET instruction separately,
  // because they don't count as a reaching def. If there is a closer
  // reaching def, this will be overwritten.
  // Essentially, they are hacked to be a reaching def for each instruction,
  // but at the lowest possible priority ("infinitely far away").
  for (auto Def : SecretDefs) {
    if (Def.getFirst().MI->getOpcode() == TargetOpcode::SECRET) {
      MaskMap[Reg] = Def.second;
    }
  }

  for (auto Def : SecretDefs) {
    SmallPtrSet<MachineInstr *, 8> Defs;
    RDA.getGlobalReachingDefs(&UseInst, Def.first.Reg.asMCReg(), Defs);

    // Check if the the SecretDef we found is relevant in this context,
    // by making sure the defining instruction is the closest reaching def of
    // UseInst
    if (Defs.contains(Def.first.MI)) {
      MaskMap[Reg] = Def.second;
    }
  }

  // Transfer the register
  auto *OP =
      std::find_if(UseInst.operands_begin(), UseInst.operands_end(),
                [&Reg](MachineOperand O) {
                  return O.isReg() && O.getReg() == Reg && O.isUse();
                });
  if (OP != UseInst.operands_end())
    TII->transferSecret(UseInst, OP, SecretMask, MaskMap, NewDefs);
  
  // Tranfer secret globals (if any)
  // TODO Should probably move this out of handleUse, 
  // such that it only runs once for each instruction,
  // since the secrecy mask of a global should be stable
  for (auto P : SecretGlobals) {
    auto *OP =
        std::find_if(UseInst.operands_begin(), UseInst.operands_end(),
                  [&P](MachineOperand O) {
                    return O.isGlobal() && O.getGlobal() == P.first;
                  });
    if (OP != UseInst.operands_end())
      TII->transferSecret(UseInst, OP, P.second, MaskMap, NewDefs);
  }

  for (auto Def : NewDefs) {
    // Add newly defined registers to work set
    WorkSet.insert(SecretDef(&UseInst, Def.first));

    // Set or upgrade the mask
    SecretDefs[SecretDef(&UseInst, Def.first)] =
        Def.second | SecretDefs[SecretDef(&UseInst, Def.first)];
  }
}

bool TrackSecretsAnalysis::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();

  SecretsSet WorkSet = findSecretSources(MF);

  errs() << "Secret globals\n";
  for (auto SG : SecretGlobals) {
    SG.first->dump();
    errs() << SG.second << "\n";
  }

  errs() << "Secret defs\n";
  for (auto S : Secrets) {
    errs() << "Secret: ";
    errs() << printReg(S.first.Reg, TRI) << " with mask ";
    errs() << std::bitset<64>(S.second).to_string() << " in ";
    errs() << *S.first.MI;
  }

  auto &RDA = getAnalysis<ReachingDefAnalysis>();

  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      for (auto Def : Secrets) {
        auto *OP =
            std::find_if(MI.operands_begin(), MI.operands_end(),
                      [&Def](MachineOperand O) {
                        return O.isReg() && O.getReg() == Def.first.Reg && O.isUse();
                      });
        if (OP != MI.operands_end())
          handleUse(MI, Def.first.Reg, Secrets[Def.first], WorkSet, Secrets);
      }
    }
  }

  while (!WorkSet.empty()) {
    auto Current = *WorkSet.begin();
    WorkSet.erase(Current);

    // errs() << "Pop: " << *Current.MI << "\n";

    SmallPtrSet<MachineInstr *, 8> Uses;
    RDA.getGlobalUses(Current.MI, Current.Reg, Uses);

    for (auto *Use : Uses) {
      if (Secrets[Current] & 1u) {
        SecretUses[SecretDef(Use, Current.Reg)] = Secrets[Current];
      }
      handleUse(*Use, Current.Reg, Secrets[Current], WorkSet, Secrets);
    }
  }

  errs() << "Secret uses\n";
  for (auto S : SecretUses) {
    errs() << "Secret: ";
    errs() << printReg(S.first.Reg, TRI) << " with mask ";
    errs() << std::bitset<64>(S.second).to_string() << " in ";
    errs() << *S.first.MI;
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
    errs() << "Secret: ";
    errs() << printReg(S.first.Reg, TRI) << " with mask ";
    errs() << std::bitset<64>(S.second).to_string() << " in ";
    errs() << *S.first.MI;
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
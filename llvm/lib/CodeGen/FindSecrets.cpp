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
  auto &RDA = getAnalysis<ReachingDefAnalysis>();
  // SmallSet<std::pair<Register, uint64_t>, 8> Map;
  DenseMap<Register, uint64_t> MaskMap;
  SmallSet<std::pair<Register, uint64_t>, 8> NewDefs;

  for (auto Def : SecretDefs) {
    SmallPtrSet<MachineInstr *, 8> Defs;
    RDA.getGlobalReachingDefs(&UseInst, Def.first.Reg.asMCReg(), Defs);

    // Check if the the SecretDef we found is relevant in this context,
    // by making sure the defining instruction is the closest reaching def of
    // UseInst
    if (Defs.contains(Def.first.MI))
      MaskMap.insert({Def.first.Reg, Def.second});
  }

  TII->transferSecret(UseInst, Reg, SecretMask, MaskMap, NewDefs);

  for (auto Def : NewDefs) {
    // Add newly defined registers to work set
    WorkSet.insert(SecretDef(&UseInst, Def.first));

    // Set or upgrade the mask
    SecretDefs[SecretDef(&UseInst, Def.first)] =
        Def.second | SecretDefs[SecretDef(&UseInst, Def.first)];
  }
  // errs() << "handleUse: " << *UseInst << "\n";
}

bool TrackSecretsAnalysis::runOnMachineFunction(MachineFunction &MF) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();

  SecretsSet WorkSet = findSecretSources(MF);

  auto &RDA = getAnalysis<ReachingDefAnalysis>();

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
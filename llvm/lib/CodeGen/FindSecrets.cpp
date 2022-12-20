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
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "track-secrets"

void SecretDef::findOperands(MachineInstr *MI,
                             SmallVector<MachineOperand, 8> &Ops,
                             TargetRegisterInfo *TRI) const {
  auto HandleReg = [&](MachineOperand &MO, Register Reg) {
    if (!MO.isReg())
      return;
    Register MOReg = MO.getReg();
    if (!MOReg)
      return;
    if (MOReg == Reg || (TRI && Reg && MOReg && TRI->regsOverlap(MOReg, Reg)))
      Ops.push_back(MO);
  };

  for (auto MO : MI->operands()) {
    switch (this->getKind()) {
    case SDK_Argument: {
      auto S = this->get<SecretArgument>();
      HandleReg(MO, S.getReg());
      break;
    }
    case SDK_Global: {
      auto S = this->get<SecretGlobal>();
      if (!MO.isGlobal())
        break;
      const GlobalValue *GV = MO.getGlobal();
      if (!GV)
        break;
      if (GV->getName() == S.getGlobalVar()->getName())
        Ops.push_back(MO);
      break;
    }
    case SDK_SecretRegisterDef: {
      auto S = this->get<SecretRegisterDef>();
      HandleReg(MO, S.getReg());
      break;
    }
    }
  }
}

template <class GraphDataT, GraphType GT>
void FlowGraph<GraphDataT, GT>::getSources(
    SmallSet<SecretDef, 8> &SecretDefs,
    DenseMap<SecretDef, uint64_t> &Secrets) const {
  Function &F = MF.getFunction();
  const Module *M = F.getParent();

  // Lower global annotations to SecretGlobal
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

        auto Def = SecretDef::global(GV);
        Secrets[Def] = Mask;
        SecretDefs.insert(Def);
      }
    }
  }

  // Lower SECRET pseudo-instructions into SecretArgument and SecretRegisterDef
  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      if (MI.getOpcode() == TargetOpcode::SECRET) {
        uint64_t SecretMask = MI.getOperand(1).getImm();
        Register Reg = MI.getOperand(0).getReg();

        if constexpr (GT == GT_PhysReg) {
          SmallPtrSet<MachineInstr *, 16> Defs;
          Data.RDA.getGlobalReachingDefs(&MI, Reg, Defs);

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
        } else {
          if (MachineOperand *MO = Data.MRI.getOneDef(Reg)) {
            auto Def = SecretDef::registerDef(Reg, MO->getParent());
            Secrets[Def] = SecretMask;
            SecretDefs.insert(Def);
          } else {
            llvm_unreachable("expected single def for vreg");
          }
        }
      }
    }
  }
}

template <class GraphDataT, GraphType GT>
void FlowGraph<GraphDataT, GT>::getUses(
    SecretDef &SD, SmallPtrSet<MachineInstr *, 8> &Uses) const {
  switch (SD.getKind()) {
  case SecretDef::SDK_SecretRegisterDef: {
    auto S = SD.get<SecretRegisterDef>();
    if constexpr (GT == GT_PhysReg) {
      SmallPtrSet<MachineInstr *, 8> GlobalUses;
      Data.RDA.getGlobalUses(S.getMI(), S.getReg().asMCReg(), GlobalUses);
      for (auto *Use : GlobalUses) {
        if (std::find_if(Use->uses().begin(), Use->uses().end(),
                         [&S](MachineOperand &O) {
                           return O.isReg() && O.getReg() == S.getReg();
                         }) != Use->uses().end()) {
          Uses.insert(Use);
        }
      }
    } else {
      for (MachineInstr &MI : Data.MRI.use_instructions(S.getReg())) {
        Uses.insert(&MI);
      }
    }
    break;
  }
  case SecretDef::SDK_Argument: {
    auto S = SD.get<SecretArgument>();
    if constexpr (GT == GT_PhysReg) {
      for (MachineBasicBlock &MB : MF) {
        for (MachineInstr &MI : MB) {
          if (std::find_if(MI.uses().begin(), MI.uses().end(),
                           [&S](MachineOperand &O) {
                             return O.isReg() && O.getReg() == S.getReg();
                           }) != MI.uses().end()) {
            SmallPtrSet<MachineInstr *, 8> Defs;
            Data.RDA.getGlobalReachingDefs(&MI, S.getReg().asMCReg(), Defs);

            // If there is no closer reaching def, the argument is the reaching
            // def
            if (Defs.empty()) {
              Uses.insert(&MI);
            }
          }
        }
      }
    } else {
      llvm_unreachable(
          "SecretArgument defs should not occur before register allocation");
    }
    break;
  }
  case SecretDef::SDK_Global: {
    auto S = SD.get<SecretGlobal>();
    for (MachineBasicBlock &MB : MF) {
      for (MachineInstr &MI : MB) {
        auto *OP = std::find_if(
            MI.operands_begin(), MI.operands_end(), [&S](MachineOperand &O) {
              return O.isGlobal() &&
                     O.getGlobal()->getName() == S.getGlobalVar()->getName();
            });
        if (OP != MI.operands_end()) {
          Uses.insert(&MI);
        }
      }
    }
    break;
  }
  }
}

// NOTE: Why does this function exist?
template <class GraphDataT, GraphType GT>
void FlowGraph<GraphDataT, GT>::getReachingDefs(
    SecretDef &SD, SmallPtrSet<MachineInstr *, 8> &Defs) const {
  switch (SD.getKind()) {
  case SecretDef::SDK_SecretRegisterDef: {
    auto S = SD.get<SecretRegisterDef>();
    if (GT == GT_PhysReg) {
      Data.RDA.getGlobalReachingDefs(S.getMI(), S.getReg().asMCReg(), Defs);
    } else if (GT == GT_VirtReg) {
      if (MachineOperand *MO = Data.MRI.getOneDef(S.getReg())) {
        Defs.insert(MO->getParent());
      } else {
        llvm_unreachable("expected single def for vreg");
      }
    }
    break;
  }
  case SecretDef::SDK_Argument:
  case SecretDef::SDK_Global:
    // Arguments and globals don't have a reaching def within this function
    break;
  }
}

template <class GraphTypeT, GraphType GT>
void TrackSecretsAnalysis<GraphTypeT, GT>::handleUse(MachineInstr &UseInst,
                                                     MachineOperand &MO,
                                                     uint64_t SecretMask,
                                                     SecretsSet &WorkSet,
                                                     SecretsMap &SecretDefs) {
  SmallSet<std::pair<Register, uint64_t>, 8> NewDefs;
  errs() << "handleUse " << MO << " in " << UseInst << "\n";

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

template <class GraphDataT, GraphType GT>
bool TrackSecretsAnalysis<GraphDataT, GT>::run(
    MachineFunction &MF, FlowGraph<GraphDataT, GT> Graph) {
  const auto &ST = MF.getSubtarget();
  TII = ST.getInstrInfo();
  TRI = ST.getRegisterInfo();

  MF.dump();

  // Step 1: find secret arguments and globals
  SecretsSet WorkSet;
  Graph.getSources(WorkSet, Secrets);

  // Step 2: iteratively propagate secrets from def to uses
  while (!WorkSet.empty()) {
    auto Current = *WorkSet.begin();
    WorkSet.erase(Current);

    SmallPtrSet<MachineInstr *, 8> Uses;
    Graph.getUses(Current, Uses);

    for (auto *Use : Uses) {
      auto SU = SecretUse(Current, Use, Secrets[Current]);

      SecretUses[{Use, Current}] = SU;

      for (auto MO : SU.operands()) {
        handleUse(*Use, MO, Secrets[Current], WorkSet, Secrets);
      }
    }
  }

  // Step 3: verify secret types, check for errors
  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      DenseMap<MachineOperand, uint64_t> MaskMap;

      for (auto Use : SecretUses) {
        if (Use.second.getUser() == &MI) {
          for (auto &MO : Use.second.operands()) {
            MaskMap[MO] = Use.second.getSecretMask();
          }
        }
      }

      TII->verifySecretTypes(MI, MaskMap);
    }
  }

  for (auto S : SecretUses) {
    for (auto MO : S.second.operands()) {
      errs() << "Secret: ";
      errs() << MO << " with mask ";
      errs() << std::bitset<4>(S.second.getSecretMask()).to_string() << " in ";
      errs() << *S.second.getUser();
    }
  }

  return false;
}

char TrackSecretsAnalysisVirtReg::ID = 0;
char &llvm::TrackSecretsVirtRegPassID = TrackSecretsAnalysisVirtReg::ID;

char TrackSecretsAnalysisPhysReg::ID = 0;
char &llvm::TrackSecretsPhysRegPassID = TrackSecretsAnalysisPhysReg::ID;

TrackSecretsAnalysisVirtReg::TrackSecretsAnalysisVirtReg()
    : MachineFunctionPass(ID),
      TSA(TrackSecretsAnalysis<GraphDataVirtReg, GT_VirtReg>()) {
  initializeTrackSecretsAnalysisVirtRegPass(*PassRegistry::getPassRegistry());
}
TrackSecretsAnalysisPhysReg::TrackSecretsAnalysisPhysReg()
    : MachineFunctionPass(ID),
      TSA(TrackSecretsAnalysis<GraphDataPhysReg, GT_PhysReg>()) {
  initializeTrackSecretsAnalysisPhysRegPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(TrackSecretsAnalysisPhysReg, "track-secrets-phys-reg",
                      "Track Secrets", true, true)
INITIALIZE_PASS_DEPENDENCY(ReachingDefAnalysis)
INITIALIZE_PASS_END(TrackSecretsAnalysisPhysReg, "track-secrets-phys-reg",
                    "Track Secrets", true, true)

INITIALIZE_PASS_BEGIN(TrackSecretsAnalysisVirtReg, "track-secrets-virt-reg",
                      "Track Secrets", true, true)
INITIALIZE_PASS_END(TrackSecretsAnalysisVirtReg, "track-secrets-virt-reg",
                    "Track Secrets", true, true)

#include "llvm/ADT/SmallBitVector.h"
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
    switch (getKind()) {
    case SDK_Argument: {
      HandleReg(MO, getReg());
      break;
    }
    case SDK_Global: {
      if (!MO.isGlobal())
        break;
      const GlobalValue *GV = MO.getGlobal();
      if (!GV)
        break;
      if (GV->getName() == getGlobalVariable()->getName())
        Ops.push_back(MO);
      break;
    }
    case SDK_SecretRegisterDef: {
      HandleReg(MO, getReg());
      break;
    }
    default:
      break;
    }
  }
}

void FlowGraph::getSources(SmallSet<SecretDef, 8> &SecretDefs,
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

        auto Def = SecretDef::CreateGlobal(GV);
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

        if (RDA) {
          SmallPtrSet<MachineInstr *, 16> Defs;
          RDA->getGlobalReachingDefs(&MI, Reg, Defs);

          // If there are no reaching defs, we assume that the register is an
          // argument and we need to start tracking at the SECRET
          // pseudoinstruction
          if (Defs.empty()) {
            auto Def = SecretDef::CreateArgument(Reg);
            Secrets[Def] = SecretMask;
            SecretDefs.insert(Def);
          }

          // If there are reaching defs, then we need to make sure to start
          // tracking at the reaching def and not the SECRET pseudoinstruction
          for (MachineInstr *DefMI : Defs) {
            auto Def = SecretDef::CreateRegisterDef(Reg, DefMI);
            Secrets[Def] = SecretMask;
            SecretDefs.insert(Def);
          }
        } else {
          if (MachineOperand *MO = MRI.getOneDef(Reg)) {
            auto Def = SecretDef::CreateRegisterDef(Reg, MO->getParent());
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

void FlowGraph::getUses(SecretDef &SD,
                        SmallPtrSet<MachineInstr *, 8> &Uses) {
  switch (SD.getKind()) {
  case SecretDef::SDK_SecretRegisterDef: {
    if (RDA) {
      SmallPtrSet<MachineInstr *, 8> GlobalUses;
      RDA->getGlobalUses(SD.getMI(), SD.getReg().asMCReg(), GlobalUses);
      for (auto *Use : GlobalUses) {
        if (std::find_if(Use->uses().begin(), Use->uses().end(),
                         [&SD](MachineOperand &O) {
                           return O.isReg() && O.getReg() == SD.getReg();
                         }) != Use->uses().end()) {
          Uses.insert(Use);
        }
      }
    } else {
      for (MachineInstr &MI : MRI.use_instructions(SD.getReg())) {
        Uses.insert(&MI);
      }
    }

    for (auto *MBB : ControlDeps[SD.getMI()]) {
      for (MachineInstr &MI : *MBB) {
        Uses.insert(&MI);
        LLVM_DEBUG(errs() << "Handle control dependency from ");
        LLVM_DEBUG(errs() << SD.getMI());
        LLVM_DEBUG(errs() << "to ");
        LLVM_DEBUG(errs() << MI);
      }
    }

    break;
  }
  case SecretDef::SDK_Argument: {
    if (RDA) {
      for (MachineBasicBlock &MB : MF) {
        for (MachineInstr &MI : MB) {
          if (std::find_if(MI.uses().begin(), MI.uses().end(),
                           [&SD](MachineOperand &O) {
                             return O.isReg() && O.getReg() == SD.getReg();
                           }) != MI.uses().end()) {
            SmallPtrSet<MachineInstr *, 8> Defs;
            // RDA->getGlobalReachingDefs(&MI, SD.getReg().asMCReg(), Defs);
            auto Def = RDA->getReachingDef(&MI, SD.getReg().asMCReg());

            // If there is no closer reaching def, the argument is the reaching
            // def
            // if (Defs.empty()) {
            if (Def < 0) {
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
    for (MachineBasicBlock &MB : MF) {
      for (MachineInstr &MI : MB) {
        auto *OP = std::find_if(
            MI.operands_begin(), MI.operands_end(), [&SD](MachineOperand &O) {
              return O.isGlobal() &&
                     O.getGlobal()->getName() == SD.getGlobalVariable()->getName();
            });
        if (OP != MI.operands_end()) {
          Uses.insert(&MI);
        }
      }
    }
    break;
  }
  default:
    break;
  }
}

FlowGraph::FlowGraph(MachineFunction &MF, ReachingDefAnalysis *RDA, MachineDominatorTree *MDT, MachinePostDominatorTree *MPDT)
      : MF(MF), MDT(MDT), MPDT(MPDT), RDA(RDA), MRI(MF.getRegInfo()) {
  const auto &ST = MF.getSubtarget();
  const auto *TII = ST.getInstrInfo();

  for (auto &MBB : MF) {
    if (MBB.succ_size() > 1) {
      for (auto &MI : MBB) {
        if (MI.isConditionalBranch()) {
          SmallVector<MachineInstr *> DefMIs;
          for (auto &MO : MI.operands()) {
            if (!MO.isReg())
              continue;
            if (RDA) {
              SmallPtrSet<MachineInstr *, 8> Defs;
              RDA->getGlobalReachingDefs(&MI, MO.getReg(), Defs);
              for (auto *D : Defs) {
                DefMIs.push_back(D);
              }
            } else {
              auto *D = MRI.getVRegDef(MO.getReg());
              if (D)
                DefMIs.push_back(D);
            }
          }
          
          // auto *Target = TII->getBranchDestBlock(MI);
          auto *Target = MPDT->getBase().getNode(&MBB)->getIDom()->getBlock();
          SmallVector<MachineBasicBlock *, 4> WorkSet;
          for (auto *Succ : MBB.successors())
            WorkSet.push_back(Succ);
          while (!WorkSet.empty()) {
            auto *Current = WorkSet.pop_back_val();
            if (Current == Target)
              continue;

            for (auto *D : DefMIs) {
              ControlDeps[D].insert(Current);
            
              LLVM_DEBUG(errs() << "Control dependency from ");
              LLVM_DEBUG(errs() << *D);
              LLVM_DEBUG(errs() << " to ");
              LLVM_DEBUG(Current->printAsOperand(errs()));
              LLVM_DEBUG(errs() << "\n");
            }

            for (auto &Succ : Current->successors())
              WorkSet.push_back(Succ);
          }
        }
      }
    }
  }
}

void TrackSecretsAnalysis::handleUse(MachineInstr &UseInst, MachineOperand &MO,
                                     uint64_t SecretMask, SecretsSet &WorkSet,
                                     SecretsMap &SecretDefs) {
  SmallSet<std::pair<Register, uint64_t>, 8> NewDefs;
  LLVM_DEBUG(errs() << "handleUse " << MO << " in " << UseInst << "\n");

  // Transfer the operand
  TII->transferSecret(UseInst, MO, SecretMask, NewDefs);

  for (auto Def : NewDefs) {
    SecretDef NewDef = SecretDef::CreateRegisterDef(Def.first, &UseInst);

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

  if (Graph) {
    delete Graph;
    Graph = nullptr;
  }

  Secrets.clear();
  SecretUses.clear();

  if (IsSSA) {
    Graph = new FlowGraph(MF, nullptr, nullptr, &getAnalysis<MachinePostDominatorTree>());
  } else {
    Graph = new FlowGraph(MF, &getAnalysis<ReachingDefAnalysis>(), &getAnalysis<MachineDominatorTree>(), &getAnalysis<MachinePostDominatorTree>());
  }

  LLVM_DEBUG(MF.dump());

  // Step 1: find secret arguments and globals
  SecretsSet WorkSet;
  Graph->getSources(WorkSet, Secrets);

  // Step 2: iteratively propagate secrets from def to uses
  while (!WorkSet.empty()) {
    auto Current = *WorkSet.begin();
    WorkSet.erase(Current);

    SmallPtrSet<MachineInstr *, 8> Uses;
    Graph->getUses(Current, Uses);

    for (auto *Use : Uses) {
      auto SU = SecretUse(Current, Use, Secrets[Current]);

      SecretUses[{Use, Current}] = SU;

      for (auto MO : SU.operands()) {
        handleUse(*Use, MO, Secrets[Current], WorkSet, Secrets);
      }

      if (SU.operands().empty()) {
        // Implicit dependency
        MachineOperand Temp = MachineOperand::CreateImm(0);
        handleUse(*Use, Temp, Secrets[Current], WorkSet, Secrets);
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
      LLVM_DEBUG(errs() << *S.second.getUser());
      LLVM_DEBUG(errs() << "Secret: ");
      LLVM_DEBUG(errs() << MO << " with mask ");
      LLVM_DEBUG(errs() << std::bitset<4>(S.second.getSecretMask()).to_string() << " in ");
      LLVM_DEBUG(errs() << *S.second.getUser());
    }
  }

  return false;
}

char TrackSecretsAnalysis::ID = 0;
char &llvm::TrackSecretsAnalysisID = TrackSecretsAnalysis::ID;

TrackSecretsAnalysis::TrackSecretsAnalysis(bool IsSSA)
    : MachineFunctionPass(ID), IsSSA(IsSSA) {
  initializeTrackSecretsAnalysisPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(TrackSecretsAnalysis, "track-secrets",
                      "Track Secrets", true, true)
INITIALIZE_PASS_END(TrackSecretsAnalysis, "track-secrets",
                    "Track Secrets", true, true)

namespace llvm {

FunctionPass *createTrackSecretsAnalysisPass(bool IsSSA) {
  return new TrackSecretsAnalysis(IsSSA);
}

} // namespace llvm

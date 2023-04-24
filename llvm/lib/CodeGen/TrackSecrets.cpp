
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TrackSecrets.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"

#define DEBUG_TYPE "track-secrets"

void getSources(MachineFunction &MF, ReachingDefAnalysis *RDA,
                SmallSet<FlowGraphNode, 8> &SecretDefs,
                DenseMap<FlowGraphNode, uint64_t> &SecretMasks) {
  Function &F = MF.getFunction();
  const Module *M = F.getParent();
  auto &MRI = MF.getRegInfo();

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

        auto Def = FlowGraphNode::CreateGlobal(GV);
        SecretMasks[Def] = Mask;
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
            auto Def = FlowGraphNode::CreateArgument(Reg);
            SecretMasks[Def] = SecretMask;
            SecretDefs.insert(Def);
          }

          // If there are reaching defs, then we need to make sure to start
          // tracking at the reaching def and not the SECRET pseudoinstruction
          for (MachineInstr *DefMI : Defs) {
            auto Def = FlowGraphNode::CreateRegisterDef(Reg, DefMI);
            SecretMasks[Def] = SecretMask;
            SecretDefs.insert(Def);
          }
        } else {
          if (MachineOperand *MO = MRI.getOneDef(Reg)) {
            auto Def = FlowGraphNode::CreateRegisterDef(Reg, MO->getParent());
            SecretMasks[Def] = SecretMask;
            SecretDefs.insert(Def);
          } else {
            llvm_unreachable("expected single def for vreg");
          }
        }
      }
    }
  }
}

FlowGraph::FlowGraph(MachineFunction &MF, ReachingDefAnalysis *RDA,
                     MachineDominatorTree *MDT,
                     MachinePostDominatorTree *MPDT) {
  auto &MRI = MF.getRegInfo();
  SmallSet<FlowGraphNode, 8> TmpNodes;
  SmallSet<FlowGraphNode, 8> WorkSet;
  getSources(MF, RDA, WorkSet, SecretMasks);
  while (!WorkSet.empty()) {
    auto Current = *WorkSet.begin();
    WorkSet.erase(Current);

    if (!Nodes.count(Current)) {
      switch (Current.getKind()) {
      case FlowGraphNode::FGNK_SecretGlobalUse:
      case FlowGraphNode::FGNK_SecretRegisterUse: {
        for (auto &Def : Current.getMI()->defs()) {
          auto Node =
              FlowGraphNode::CreateRegisterDef(Def.getReg(), Current.getMI());
          createEdge(Current, Node);
          WorkSet.insert(Node);
        }
        break;
      }
      case FlowGraphNode::FGNK_ControlDep: {    
        for (auto &Def : Current.getMI()->defs()) {
          auto DNode = FlowGraphNode::CreateRegisterDef(Def.getReg(), Current.getMI());
          createEdge(Current, DNode);
          WorkSet.insert(DNode);
        }
        break;
      }
      case FlowGraphNode::FGNK_SecretRegisterDef: {
        TmpNodes.clear();
          
        if (RDA) {
          SmallPtrSet<MachineInstr *, 8> GlobalUses;
          RDA->getGlobalUses(Current.getMI(), Current.getReg().asMCReg(),
                             GlobalUses);
          for (auto *Use : GlobalUses) {
            if (std::find_if(Use->uses().begin(), Use->uses().end(),
                             [&Current](MachineOperand &O) {
                               return O.isReg() &&
                                      O.getReg() == Current.getReg();
                             }) != Use->uses().end()) {
              auto Node =
                  FlowGraphNode::CreateRegisterUse(Current.getReg(), Use);
              TmpNodes.insert(Node);
            }
          }
        } else {
          for (MachineInstr &MI : MRI.use_instructions(Current.getReg())) {
            auto Node = FlowGraphNode::CreateRegisterUse(Current.getReg(), &MI);
            TmpNodes.insert(Node);
          }
        }
        for (auto &Node : TmpNodes) {
          createEdge(Current, Node);
          WorkSet.insert(Node);

          if (Node.getMI()->isConditionalBranch()) {
            auto *MBB = Node.getMI()->getParent();
            auto *Target = MPDT->getBase().getNode(MBB)->getIDom()->getBlock();
            SmallVector<MachineBasicBlock *, 4> WorkSet2;
            for (auto *Succ : MBB->successors())
              WorkSet2.push_back(Succ);
            while (!WorkSet2.empty()) {
              auto *CurrentBlock = WorkSet2.pop_back_val();
              if (CurrentBlock == Target)
                continue;

              for (auto &MI : *CurrentBlock) {
                auto NewNode = FlowGraphNode::CreateControlDep(Current.getReg(), &MI);
                createEdge(Current, NewNode);
                WorkSet.insert(NewNode);
              }

              for (auto &Succ : CurrentBlock->successors())
                WorkSet2.push_back(Succ);
            }
          }
        }
        break;
      }
      case FlowGraphNode::FGNK_Argument: {
        if (RDA) {
          TmpNodes.clear();

          for (MachineBasicBlock &MB : MF) {
            for (MachineInstr &MI : MB) {
              if (std::find_if(MI.uses().begin(), MI.uses().end(),
                               [&Current](MachineOperand &O) {
                                 return O.isReg() &&
                                        O.getReg() == Current.getReg();
                               }) != MI.uses().end()) {
                SmallPtrSet<MachineInstr *, 8> Defs;
                // RDA->getGlobalReachingDefs(&MI, SD.getReg().asMCReg(), Defs);
                auto Def = RDA->getReachingDef(&MI, Current.getReg().asMCReg());

                // If there is no closer reaching def, the argument is the
                // reaching def if (Defs.empty()) {
                if (Def < 0) {
                  auto Node =
                      FlowGraphNode::CreateRegisterUse(Current.getReg(), &MI);
                  TmpNodes.insert(Node);
                }
              }
            }
          }
          for (auto &Node : TmpNodes) {
            createEdge(Current, Node);
            WorkSet.insert(Node);

            if (Node.getMI()->isConditionalBranch()) {
              auto *MBB = Node.getMI()->getParent();
              auto *Target = MPDT->getBase().getNode(MBB)->getIDom()->getBlock();
              SmallVector<MachineBasicBlock *, 4> WorkSet2;
              for (auto *Succ : MBB->successors())
                WorkSet2.push_back(Succ);
              while (!WorkSet2.empty()) {
                auto *CurrentBlock = WorkSet2.pop_back_val();
                if (CurrentBlock == Target)
                  continue;

                for (auto &MI : *CurrentBlock) {
                  auto NewNode = FlowGraphNode::CreateControlDep(Current.getReg(), &MI);
                  createEdge(Current, NewNode);
                  WorkSet.insert(NewNode);
                }

                for (auto &Succ : CurrentBlock->successors())
                  WorkSet2.push_back(Succ);
              }
            }
          }
        } else {
          llvm_unreachable("SecretArgument defs should not occur before "
                           "register allocation");
        }
        break;
      }
      case FlowGraphNode::FGNK_Global: {
        for (MachineBasicBlock &MB : MF) {
          for (MachineInstr &MI : MB) {
            auto *OP = std::find_if(
                MI.operands_begin(), MI.operands_end(),
                [&Current](MachineOperand &O) {
                  return O.isGlobal() &&
                         O.getGlobal()->getName() ==
                             Current.getGlobalVariable()->getName();
                });
            if (OP != MI.operands_end()) {
              auto Node = FlowGraphNode::CreateGlobalUse(
                  Current.getGlobalVariable(), &MI);
              createEdge(Current, Node);
              WorkSet.insert(Node);
            }
          }
        }
        break;
      }
      default:
        break;
      }
    }
  }
}

void findOperands(FlowGraphNode &Node, SmallVectorImpl<MachineOperand> &Ops,
                  const TargetRegisterInfo *TRI) {
  auto HandleReg = [&](MachineOperand &MO, Register Reg) {
    if (!MO.isReg())
      return;
    Register MOReg = MO.getReg();
    if (!MOReg)
      return;
    if (MOReg == Reg || (TRI && Reg && MOReg && TRI->regsOverlap(MOReg, Reg)))
      Ops.push_back(MO);
  };

  for (auto MO : Node.getMI()->operands()) {
    switch (Node.getKind()) {
    case FlowGraphNode::FGNK_SecretRegisterUse:
    case FlowGraphNode::FGNK_SecretRegisterDef:
    case FlowGraphNode::FGNK_Argument: {
      HandleReg(MO, Node.getReg());
      break;
    }
    case FlowGraphNode::FGNK_Global:
    case FlowGraphNode::FGNK_SecretGlobalUse: {
      if (!MO.isGlobal())
        break;
      const GlobalValue *GV = MO.getGlobal();
      if (!GV)
        break;
      if (GV->getName() == Node.getGlobalVariable()->getName())
        Ops.push_back(MO);
      break;
    }
    default:
      llvm_unreachable("cannot be reached if getMI is valid");
      break;
    }
  }
}

DenseMap<FlowGraphNode, uint64_t> &
FlowGraph::compute(const TargetInstrInfo *TII, const TargetRegisterInfo *TRI) {
  SmallSet<FlowGraphNode, 8> WorkSet;
  SmallVector<MachineOperand, 8> Ops;
  SmallSet<std::pair<Register, uint64_t>, 8> NewDefs;

  for (auto &Pair : SecretMasks) {
    if (Pair.getSecond())
      WorkSet.insert(Pair.getFirst());
  }

  while (!WorkSet.empty()) {
    auto Current = *WorkSet.begin();
    WorkSet.erase(Current);

    Ops.clear();
    if (Current.getKind() == FlowGraphNode::FGNK_SecretRegisterUse ||
        Current.getKind() == FlowGraphNode::FGNK_SecretGlobalUse) {
      findOperands(Current, Ops, TRI);
    }

    for (auto &Use : Nodes[Current]) {
      auto Mask = SecretMasks[Current];
      uint64_t NewMask = 0;

      switch (Current.getKind()) {
      case FlowGraphNode::FGNK_Argument:
      case FlowGraphNode::FGNK_SecretRegisterDef:
      case FlowGraphNode::FGNK_Global:
        assert(Use.isGlobalUse() || Use.isRegisterUse() ||
               Use.isControlDep() && "Invalid Flow Graph");
        NewMask = Mask;
        break;
      case FlowGraphNode::FGNK_SecretRegisterUse:
      case FlowGraphNode::FGNK_SecretGlobalUse:
        assert(Use.isRegisterDef() && "Invalid Flow Graph");
        assert(Use.getMI() == Current.getMI() && "Invalid Use-Def edge");
        // Transfer using TTI->transferSecret
        for (auto &MO : Ops) {
          NewDefs.clear();
          TII->transferSecret(*Current.getMI(), MO, Mask, NewDefs);
          for (auto &D : NewDefs) {
            if (D.first == Use.getReg()) {
              NewMask = D.second;
            }
          }
        }
        break;
      case FlowGraphNode::FGNK_ControlDep:
        assert(Use.isRegisterDef() && "Invalid Flow Graph");
        NewMask = Mask;
        break;
      default:
        break;
      }

      if (NewMask != SecretMasks[Use]) {
        SecretMasks[Use] = NewMask;
        WorkSet.insert(Use);
      }
    }
  }

  return SecretMasks;
}

void FlowGraph::getSecretUses(SmallPtrSetImpl<MachineInstr *> &Uses) {
  for (auto &Pair : SecretMasks) {
    auto &Node = Pair.getFirst();
    if (Node.isRegisterUse() || Node.isGlobalUse()) {
      if (Pair.getSecond())
        Uses.insert(Node.getMI());
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

  if (IsSSA) {
    Graph = new FlowGraph(MF, nullptr, nullptr,
                          &getAnalysis<MachinePostDominatorTree>());
  } else {
    Graph = new FlowGraph(MF, &getAnalysis<ReachingDefAnalysis>(),
                          &getAnalysis<MachineDominatorTree>(),
                          &getAnalysis<MachinePostDominatorTree>());
  }

  LLVM_DEBUG(Graph->dump());
  Graph->compute(TII, TRI);
  LLVM_DEBUG(Graph->dump());
  Graph->getSecretUses(SecretUses);

  LLVM_DEBUG(errs() << "Secret uses\n");
  for (auto *MI : SecretUses) {
    LLVM_DEBUG(MI->dump());
  }

  return false;
}

char TrackSecretsAnalysis::ID = 0;
char &llvm::TrackSecretsAnalysisID = TrackSecretsAnalysis::ID;

TrackSecretsAnalysis::TrackSecretsAnalysis(bool IsSSA)
    : MachineFunctionPass(ID), IsSSA(IsSSA) {
  initializeTrackSecretsAnalysisPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(TrackSecretsAnalysis, "track-secrets", "Track Secrets",
                      true, true)
INITIALIZE_PASS_END(TrackSecretsAnalysis, "track-secrets", "Track Secrets",
                    true, true)

namespace llvm {

FunctionPass *createTrackSecretsAnalysisPass(bool IsSSA) {
  return new TrackSecretsAnalysis(IsSSA);
}

} // namespace llvm

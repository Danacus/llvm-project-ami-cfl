
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/ControlDependenceGraph.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TrackSecrets.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/GraphWriter.h"

#define DEBUG_TYPE "track-secrets"

static void writeFlowGraphToDotFile(MachineFunction &MF, FlowGraph *FG) {
  std::string Filename = (".fg." + MF.getName() + ".dot").str();
  errs() << "Writing '" << Filename << "'...";

  std::error_code EC;
  raw_fd_ostream File(Filename, EC, sys::fs::OF_Text);

  if (!EC)
    WriteGraph(File, FG, false);
  else
    errs() << "  error opening file for writing!";
  errs() << '\n';
}

FlowGraph::~FlowGraph() {
  for (auto &Pair : Nodes) {
    delete Pair.getSecond();
  }
}

void FlowGraph::getSources(MachineFunction &MF, ReachingDefAnalysis *RDA,
                           SmallSet<FlowGraphNode *, 8> &SecretDefs,
                           DenseMap<FlowGraph::Key, uint64_t> &SecretMasks) {
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

        auto *Def = getOrInsert(FlowGraphNode::CreateGlobal(GV));
        SecretMasks[Def->inner()] = Mask;
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
            auto *Def = getOrInsert(FlowGraphNode::CreateArgument(Reg));
            SecretMasks[Def->inner()] = SecretMask;
            SecretDefs.insert(Def);
          }

          // If there are reaching defs, then we need to make sure to start
          // tracking at the reaching def and not the SECRET pseudoinstruction
          for (MachineInstr *DefMI : Defs) {
            auto *Def =
                getOrInsert(FlowGraphNode::CreateRegisterDef(Reg, DefMI));
            SecretMasks[Def->inner()] = SecretMask;
            SecretDefs.insert(Def);
          }
        } else {
          if (MachineOperand *MO = MRI.getOneDef(Reg)) {
            auto *Def = getOrInsert(
                FlowGraphNode::CreateRegisterDef(Reg, MO->getParent()));
            SecretMasks[Def->inner()] = SecretMask;
            SecretDefs.insert(Def);
          } else {
            llvm_unreachable("expected single def for vreg");
          }
        }
      }
    }
  }
}

void FlowGraph::handleControlDep(MachineInstr &BranchMI,
                                 ControlDependenceGraph *CDG,
                                 MachinePostDominatorTree *MPDT,
                                 LiveVariables *LV, const TargetInstrInfo *TII,
                                 Register DepReg,
                                 SmallSet<FlowGraphNode *, 8> &Nodes) {
  auto *CurrentMBB = BranchMI.getParent();
  auto *MF = CurrentMBB->getParent();
  // MachineBasicBlock *TBB;
  // MachineBasicBlock *FBB;
  // SmallVector<MachineOperand> Cond;
  // TII->analyzeBranch(*CurrentMBB, TBB, FBB, Cond);
  MachineBasicBlock *PostDom = MPDT->getNode(CurrentMBB)->getIDom()->getBlock();

  for (auto &MI : *PostDom) {
    if (!MI.isPHI())
      break;

    auto *Node = getOrInsert(FlowGraphNode::CreateControlDep(DepReg, &MI));
    Nodes.insert(Node);
  }

  for (auto &MBB : *MF) {
    if (CDG->influences(CurrentMBB, &MBB)) {
      for (auto &MI : MBB) {
        if (std::find_if(
                MI.defs().begin(), MI.defs().end(), [&](MachineOperand &MO) {
                  return MO.isReg() && isLiveAt(MO.getReg(), PostDom, LV);
                }) != MI.defs().end()) {
          LLVM_DEBUG(errs() << "New control dep\n");
          LLVM_DEBUG(MI.dump());
          LLVM_DEBUG(PostDom->dump());
          auto *Node =
              getOrInsert(FlowGraphNode::CreateControlDep(DepReg, &MI));
          Nodes.insert(Node);
          LLVM_DEBUG(Node->dump());
        }
      }
    }
  }
}

FlowGraph::FlowGraph(MachineFunction &MF, ReachingDefAnalysis *RDA,
                     MachineDominatorTree *MDT, MachinePostDominatorTree *MPDT,
                     ControlDependenceGraph *CDG, LiveVariables *LV) {
  const auto &ST = MF.getSubtarget();
  const auto *TII = ST.getInstrInfo();
  auto &MRI = MF.getRegInfo();
  SmallSet<FlowGraphNode *, 8> TmpNodes;
  SmallSet<FlowGraphNode *, 8> WorkSet;
  getSources(MF, RDA, WorkSet, SecretMasks);

  auto *Root = getOrInsert(FlowGraphNode::CreateRoot());
  setRoot(Root);
  for (auto *Node : WorkSet) {
    createEdge(Root, Node);
  }

  while (!WorkSet.empty()) {
    auto *CurrentNode = *WorkSet.begin();
    WorkSet.erase(CurrentNode);
    auto &Current = CurrentNode->inner();

    LLVM_DEBUG(CurrentNode->dump());
    LLVM_DEBUG(errs() << "\n");

    if (!CurrentNode->isVisited()) {
      CurrentNode->setVisited();
      LLVM_DEBUG(CurrentNode->dump());
      LLVM_DEBUG(errs() << "\n");
      switch (Current.getKind()) {
      case FlowGraphNodeInner::FGNK_SecretGlobalUse:
      case FlowGraphNodeInner::FGNK_SecretRegisterUse: {
        for (auto &Def : Current.getMI()->defs()) {
          auto *Node = getOrInsert(
              FlowGraphNode::CreateRegisterDef(Def.getReg(), Current.getMI()));
          createEdge(CurrentNode, Node);
          WorkSet.insert(Node);
        }
        break;
      }
      case FlowGraphNodeInner::FGNK_ControlDep: {
        for (auto &Def : Current.getMI()->defs()) {
          auto *DNode = getOrInsert(
              FlowGraphNode::CreateRegisterDef(Def.getReg(), Current.getMI()));
          LLVM_DEBUG(errs() << "\naaaaaa\n");
          LLVM_DEBUG(CurrentNode->dump());
          LLVM_DEBUG(DNode->dump());
          createEdge(CurrentNode, DNode);
          WorkSet.insert(DNode);
        }
        break;
      }
      case FlowGraphNodeInner::FGNK_SecretRegisterDef: {
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
              auto *Node = getOrInsert(
                  FlowGraphNode::CreateRegisterUse(Current.getReg(), Use));
              TmpNodes.insert(Node);
              if (Use->isBranch()) {
                handleControlDep(*Use, CDG, MPDT, LV, TII, Current.getReg(),
                                 TmpNodes);
              }
            }
          }
        } else {
          for (MachineInstr &MI : MRI.use_instructions(Current.getReg())) {
            auto *Node = getOrInsert(
                FlowGraphNode::CreateRegisterUse(Current.getReg(), &MI));
            TmpNodes.insert(Node);

            if (MI.isBranch()) {
              handleControlDep(MI, CDG, MPDT, LV, TII, Current.getReg(),
                               TmpNodes);
            }
          }
        }

        LLVM_DEBUG(errs() << "\nhere\n");

        for (auto *Node : TmpNodes) {
          LLVM_DEBUG(Node->dump());
          createEdge(CurrentNode, Node);
          WorkSet.insert(Node);
        }
        break;
      }
      case FlowGraphNodeInner::FGNK_Argument: {
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
                  auto *Node = getOrInsert(
                      FlowGraphNode::CreateRegisterUse(Current.getReg(), &MI));
                  TmpNodes.insert(Node);

                  if (MI.isBranch()) {
                    handleControlDep(MI, CDG, MPDT, LV, TII, Current.getReg(),
                                     TmpNodes);
                  }
                }
              }
            }
          }

          for (auto *Node : TmpNodes) {
            createEdge(CurrentNode, Node);
            WorkSet.insert(Node);
          }
        } else {
          llvm_unreachable("SecretArgument defs should not occur before "
                           "register allocation");
        }
        break;
      }
      case FlowGraphNodeInner::FGNK_Global: {
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
              auto *Node = getOrInsert(FlowGraphNode::CreateGlobalUse(
                  Current.getGlobalVariable(), &MI));
              createEdge(CurrentNode, Node);
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

  LLVM_DEBUG(errs() << "done\n");
}

void findOperands(FlowGraphNodeInner &Node,
                  SmallVectorImpl<MachineOperand> &Ops,
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
    case FlowGraphNodeInner::FGNK_SecretRegisterUse:
    case FlowGraphNodeInner::FGNK_SecretRegisterDef:
    case FlowGraphNodeInner::FGNK_Argument: {
      HandleReg(MO, Node.getReg());
      break;
    }
    case FlowGraphNodeInner::FGNK_Global:
    case FlowGraphNodeInner::FGNK_SecretGlobalUse: {
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

DenseMap<FlowGraphNodeInner, uint64_t> &
FlowGraph::compute(const TargetInstrInfo *TII, const TargetRegisterInfo *TRI) {
  SmallSet<FlowGraphNode *, 8> WorkSet;
  SmallVector<MachineOperand, 8> Ops;
  SmallSet<std::pair<Register, uint64_t>, 8> NewDefs;

  for (auto &Pair : SecretMasks) {
    if (Pair.getSecond())
      WorkSet.insert(Nodes[Pair.getFirst()]);
  }

  while (!WorkSet.empty()) {
    auto *CurrentNode = *WorkSet.begin();
    WorkSet.erase(CurrentNode);
    auto &Current = CurrentNode->inner();

    Ops.clear();
    if (Current.getKind() == FlowGraphNodeInner::FGNK_SecretRegisterUse ||
        Current.getKind() == FlowGraphNodeInner::FGNK_SecretGlobalUse) {
      findOperands(Current, Ops, TRI);
    }

    for (auto *UseNode : Nodes[Current]->successors()) {
      auto &Use = UseNode->inner();
      auto Mask = SecretMasks[Current];
      uint64_t NewMask = 0;

      switch (Current.getKind()) {
      case FlowGraphNodeInner::FGNK_Argument:
      case FlowGraphNodeInner::FGNK_SecretRegisterDef:
      case FlowGraphNodeInner::FGNK_Global:
        assert(Use.isGlobalUse() || Use.isRegisterUse() ||
               Use.isControlDep() && "Invalid Flow Graph");
        NewMask = Mask;
        break;
      case FlowGraphNodeInner::FGNK_SecretRegisterUse:
      case FlowGraphNodeInner::FGNK_SecretGlobalUse:
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
      case FlowGraphNodeInner::FGNK_ControlDep:
        assert(Use.isRegisterDef() && "Invalid Flow Graph");
        NewMask = Mask;
        break;
      default:
        break;
      }

      if (NewMask != SecretMasks[Use]) {
        SecretMasks[Use] = NewMask;
        WorkSet.insert(UseNode);
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

  SecretUses.clear();

  if (Graph) {
    delete Graph;
    Graph = nullptr;
  }

  if (IsSSA) {
    Graph = new FlowGraph(
        MF, nullptr, nullptr, &getAnalysis<MachinePostDominatorTree>(),
        &getAnalysis<ControlDependenceGraph>(), &getAnalysis<LiveVariables>());
  } else {
    Graph = new FlowGraph(MF, &getAnalysis<ReachingDefAnalysis>(),
                          &getAnalysis<MachineDominatorTree>(),
                          &getAnalysis<MachinePostDominatorTree>(),
                          &getAnalysis<ControlDependenceGraph>(), nullptr);
  }

  LLVM_DEBUG(Graph->dump());
  Graph->compute(TII, TRI);
  LLVM_DEBUG(Graph->dump());
  Graph->getSecretUses(SecretUses);

  LLVM_DEBUG(errs() << "Secret uses\n");
  for (auto *MI : SecretUses) {
    LLVM_DEBUG(MI->dump());
  }
  LLVM_DEBUG(writeFlowGraphToDotFile(MF, getGraph()));

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

char FlowGraphPrinter::ID = 0;

char &llvm::FlowGraphPrinterID = FlowGraphPrinter::ID;

INITIALIZE_PASS(FlowGraphPrinter, "machine-flowgraph-printer",
                "Machine FlowGraph Printer Pass", false, true)

/// Default construct and initialize the pass.
FlowGraphPrinter::FlowGraphPrinter() : MachineFunctionPass(ID) {
  initializeMachineCDGPrinterPass(*PassRegistry::getPassRegistry());
}

bool FlowGraphPrinter::runOnMachineFunction(MachineFunction &MF) {
  errs() << "Writing Machine CDG for function ";
  errs().write_escaped(MF.getName()) << '\n';

  writeFlowGraphToDotFile(MF, getAnalysis<TrackSecretsAnalysis>().getGraph());
  return false;
}

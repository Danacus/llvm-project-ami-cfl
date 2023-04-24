
#include "llvm/CodeGen/AMiLinearizationAnalysis.h"
#include "llvm/CodeGen/AMiLinearizationAnalysisPCFL.h"
#include "llvm/CodeGen/AMiLinearizationAnalysisSESE.h"

#define DEBUG_TYPE "ami-linearization-analysis"

using namespace llvm;

bool LinearizationAnalysisBase::run() {
  LLVM_DEBUG(MF->dump());

  Result.clear();
  Blocks.clear();
  BlockIndex.clear();

  findSecretDependentBranches();

  linearize();

  LLVM_DEBUG(dump());

  createActivatingRegions();
  updateEdgeMaps();

  LLVM_DEBUG(MF->dump());
  LLVM_DEBUG(MDT->dump());

  if (AnalysisOnly) {
    undoCFGChanges();
    return false;
  }

  return true;
}

void LinearizationAnalysisBase::undoCFGChanges() {
  for (auto &Edge : Result.GhostEdges) {
    Edge.first->removeSuccessor(Edge.second);
  }

  for (auto &Edge : Result.ActivatingEdges) {
    Edge.first->addSuccessor(Edge.second);
  }

  MDT->calculate(*MF);
  MPDT->getBase().recalculate(*MF);
}

void LinearizationAnalysisBase::findSecretDependentBranches() {
  // Mark blocks with secret dependent branches
  for (auto *User : TSA->SecretUses) {
    // We still need those registers
    // TODO: Does this code belong here? Can is be removed?
    for (auto &MO : User->uses()) {
      if (MO.isReg())
        MO.setIsKill(false);
    }

    LLVM_DEBUG(User->dump());

    if (User->isConditionalBranch() || User->isIndirectBranch()) {
      Result.SensitiveBranchBlocks.set(User->getParent()->getNumber());
    }
  }
}

void LinearizationAnalysisBase::createActivatingRegions() {
  for (auto &Edge : Result.GhostEdges) {
    if (!Edge.first->isSuccessor(Edge.second))
      Edge.first->addSuccessor(Edge.second);
  }

  for (auto &Edge : Result.ActivatingEdges) {
    if (Edge.first->isSuccessor(Edge.second))
      Edge.first->removeSuccessor(Edge.second);
  }

  MDT->calculate(*MF);
  MPDT->getBase().recalculate(*MF);

  LLVM_DEBUG(MDT->dump());
  LLVM_DEBUG(MPDT->dump());

  SmallPtrSet<MachineBasicBlock *, 8> Blocks;
  for (auto &MBB : *MF)
    Blocks.insert(&MBB);
  LinearizationResult::Edge Edge = {MDT->getRoot(), nullptr};
  Result.ActivatingRegions.insert(
      {Edge, ActivatingRegion(nullptr, MDT->getRoot(), nullptr, Blocks)});

  for (auto &Edge : Result.ActivatingEdges) {
    auto *Branch = Edge.first;
    MachineBasicBlock *Entry = nullptr;
    for (auto *Succ : Branch->successors()) {
      if (!Result.ActivatingEdges.contains({Edge.first, Succ})) {
        Entry = Succ;
      }
    }
    assert(Entry && "Expected an entry");
    auto *Exit = Edge.second;

    Blocks.clear();
    for (auto *Node : regionDomTreeIterator(Entry, Exit)) {
      Blocks.insert(Node->getBlock());
    }

    Result.ActivatingRegions.insert(
        {Edge, ActivatingRegion(Branch, Entry, Exit, Blocks)});
    ActivatingRegion *AR = &Result.ActivatingRegions.find(Edge)->getSecond();
    for (auto *MBB : Blocks) {
      Result.RegionMap[MBB].insert(AR);
    }

    // Check that activating region is SESE
    // - Exit should post-dominate entry
    // - Every cycle containing Entry contains Exit: assuming there is no
    // return
    //   within the region
    for (auto *Exiting : Exit->predecessors()) {
      if (MDT->dominates(Entry, Exiting))
        assert(MPDT->dominates(Exit, Entry) && "Activating region not SESE");
    }
  }
}

void LinearizationAnalysisBase::updateEdgeMaps() {
  for (auto &Edge : Result.ActivatingEdges) {
    Result.OutgoingActivatingEdges[Edge.first].insert(Edge.second);
  }
  for (auto &Edge : Result.GhostEdges) {
    Result.OutgoingGhostEdges[Edge.first].insert(Edge.second);
  }
}

bool AMiLinearizationAnalysis::runOnMachineFunction(MachineFunction &MF) {
  if (Analysis)
    delete Analysis;
  switch (Method) {
  case ALM_SESE:
    Analysis = new LinearizationAnalysisSESE(
        &getAnalysis<SensitiveRegionAnalysis>(),
        &getAnalysis<TrackSecretsAnalysis>(),
        &getAnalysis<MachineDominatorTree>(),
        &getAnalysis<MachinePostDominatorTree>(),
        &getAnalysis<MachineDominanceFrontier>(), &MF, AnalysisOnly);
    break;
  case ALM_PCFL:
    Analysis = new LinearizationAnalysisPCFL(
        &getAnalysis<CompactOrder>(), &getAnalysis<TrackSecretsAnalysis>(),
        &getAnalysis<MachineDominatorTree>(),
        &getAnalysis<MachinePostDominatorTree>(),
        &getAnalysis<MachineDominanceFrontier>(), &MF, AnalysisOnly);
    break;
  }
  bool Result = Analysis->run();
  LLVM_DEBUG(dump());
  return Result;
}

void AMiLinearizationAnalysis::print(raw_ostream &OS, const Module *) const {
  OS << "AMi Linearization Analysis\n";
  OS << "Method: ";
  switch (Method) {
  case ALM_PCFL:
    OS << "PCFL";
    break;
  case ALM_SESE:
    OS << "SESE";
    break;
  }
  OS << "\n";
  OS << "--------------------------\n";

  Analysis->print(OS);
}

char AMiLinearizationAnalysis::ID = 0;
char &llvm::AMiLinearizationAnalysisID = AMiLinearizationAnalysis::ID;

AMiLinearizationAnalysis::AMiLinearizationAnalysis(bool AnalysisOnly,
                                                   LinearizationMethod Method)
    : MachineFunctionPass(ID), AnalysisOnly(AnalysisOnly), Method(Method) {
  initializeAMiLinearizationAnalysisPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(AMiLinearizationAnalysis, "ami-linearization-analysis",
                      "AMi Linearization Analysis", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(TrackSecretsAnalysis)
INITIALIZE_PASS_END(AMiLinearizationAnalysis, "ami-linearization-analysis",
                    "AMi Linearization Analysis", false, false)

namespace llvm {

FunctionPass *createAMiLinearizationAnalysisPass(bool AnalysisOnly,
                                                 int Method) {
  return new AMiLinearizationAnalysis(AnalysisOnly, (LinearizationMethod)Method);
}

} // namespace llvm

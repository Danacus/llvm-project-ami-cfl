//===- IntraProc/ControlDependenceGraph.cpp ---------------------*- C++ -*-===//
//
//                      Static Program Analysis for LLVM
//
// This file is based on a file distributed under a Modified BSD License (see
// below).
//
// Copyright (c) 2013 President and Fellows of Harvard College
// All rights reserved.
//
// Developed by:
//
//     Scott Moore
//     Harvard School of Engineering and Applied Science
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
//
//     Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in
//     the documentation and/or other materials provided with the
//     distribution.
//
//     Neither the name of the Harvard University nor the names of the
//     developers may be used to endorse or promote products derived
//     from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//===----------------------------------------------------------------------===//
//
// This file defines the ControlDependenceGraph class, which allows fast and
// efficient control dependence queries. It is based on Ferrante et al's "The
// Program Dependence Graph and Its Use in Optimization."
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/ControlDependenceGraph.h"

#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Analysis/DOTGraphTraitsPass.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/Support/GenericDomTree.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/InitializePasses.h"

#include <deque>
#include <set>

#define DEBUG_TYPE "machine-cdg"

using namespace llvm;

namespace llvm {

void ControlDependenceNode::addTrue(ControlDependenceNode *Child) {
  TrueChildren.insert(Child);
}

void ControlDependenceNode::addFalse(ControlDependenceNode *Child) {
  FalseChildren.insert(Child);
}

void ControlDependenceNode::addOther(ControlDependenceNode *Child) {
  OtherChildren.insert(Child);
}

void ControlDependenceNode::addParent(ControlDependenceNode *Parent) {
  assert(std::find(Parent->begin(), Parent->end(), this) != Parent->end() &&
         "Must be a child before adding the parent!");
  Parents.insert(Parent);
}

void ControlDependenceNode::removeTrue(ControlDependenceNode *Child) {
  node_iterator CN = TrueChildren.find(Child);
  if (CN != TrueChildren.end())
    TrueChildren.erase(CN);
}

void ControlDependenceNode::removeFalse(ControlDependenceNode *Child) {
  node_iterator CN = FalseChildren.find(Child);
  if (CN != FalseChildren.end())
    FalseChildren.erase(CN);
}

void ControlDependenceNode::removeOther(ControlDependenceNode *Child) {
  node_iterator CN = OtherChildren.find(Child);
  if (CN != OtherChildren.end())
    OtherChildren.erase(CN);
}

void ControlDependenceNode::removeParent(ControlDependenceNode *Parent) {
  node_iterator PN = Parents.find(Parent);
  if (PN != Parents.end())
    Parents.erase(PN);
}

const ControlDependenceNode *ControlDependenceNode::enclosingRegion() const {
  if (this->isRegion()) {
    return this;
  } else {
    assert(this->Parents.size() == 1);
    const ControlDependenceNode *region = *this->Parents.begin();
    assert(region->isRegion());
    return region;
  }
}

ControlDependenceNode::EdgeType
ControlDependenceGraphBase::getEdgeType(MachineBasicBlock *A,
                                        MachineBasicBlock *B) {
  auto *MF = A->getParent();
  auto &ST = MF->getSubtarget();
  const auto *TII = ST.getInstrInfo();

  MachineBasicBlock *TBB;
  MachineBasicBlock *FBB;
  SmallVector<MachineOperand, 4> Cond;
  TII->analyzeBranch(*A, TBB, FBB, Cond);

  if (Cond.size() > 0) {
    if (TBB == B) {
      return ControlDependenceNode::TRUE;
    } 
    if (FBB == B || (A->canFallThrough() && A->getFallThrough() == B)) {
      return ControlDependenceNode::FALSE;
    }        
    llvm_unreachable("Asking for edge type between unconnected basic blocks!");
  }
  return ControlDependenceNode::OTHER;
}

void ControlDependenceGraphBase::computeDependencies(
    MachineFunction &F, MachinePostDominatorTree &pdt) {
  root = new ControlDependenceNode();
  nodes.insert(root);

  for (MachineFunction::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
    ControlDependenceNode *bn = new ControlDependenceNode(&*BB);
    nodes.insert(bn);
    bbMap[&*BB] = bn;
  }

  for (MachineFunction::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
    MachineBasicBlock *A = &*BB;
    ControlDependenceNode *AN = bbMap[A];

    for (auto *B : A->successors()) {
      assert(A && B);
      if (A == B || !pdt.dominates(B, A)) {
        MachineBasicBlock *L = pdt.findNearestCommonDominator(A, B);
        ControlDependenceNode::EdgeType type =
            ControlDependenceGraphBase::getEdgeType(A, B);
        if (A == L) {
          switch (type) {
          case ControlDependenceNode::TRUE:
            AN->addTrue(AN);
            break;
          case ControlDependenceNode::FALSE:
            AN->addFalse(AN);
            break;
          case ControlDependenceNode::OTHER:
            AN->addOther(AN);
            break;
          }
          AN->addParent(AN);
        }
        for (MachineDomTreeNode *cur = pdt[B]; cur && cur != pdt[L];
             cur = cur->getIDom()) {
          ControlDependenceNode *CN = bbMap[cur->getBlock()];
          switch (type) {
          case ControlDependenceNode::TRUE:
            AN->addTrue(CN);
            break;
          case ControlDependenceNode::FALSE:
            AN->addFalse(CN);
            break;
          case ControlDependenceNode::OTHER:
            AN->addOther(CN);
            break;
          }
          assert(CN);
          CN->addParent(AN);
        }
      }
    }
  }

  // ENTRY -> START
  for (MachineDomTreeNode *cur = pdt[&*F.begin()]; cur; cur = cur->getIDom()) {
    if (cur->getBlock()) {
      ControlDependenceNode *CN = bbMap[cur->getBlock()];
      assert(CN);
      root->addOther(CN);
      CN->addParent(root);
    }
  }
}

void ControlDependenceGraphBase::insertRegions(MachinePostDominatorTree &pdt) {
  typedef po_iterator<MachinePostDominatorTree *> po_pdt_iterator;
  typedef std::pair<ControlDependenceNode::EdgeType, ControlDependenceNode *>
      cd_type;
  typedef std::set<cd_type> cd_set_type;
  typedef std::map<cd_set_type, ControlDependenceNode *> cd_map_type;

  cd_map_type cdMap;
  cd_set_type initCDs;
  initCDs.insert(std::make_pair(ControlDependenceNode::OTHER, root));
  cdMap.insert(std::make_pair(initCDs, root));

  for (po_pdt_iterator DTN = po_pdt_iterator::begin(&pdt),
                       END = po_pdt_iterator::end(&pdt);
       DTN != END; ++DTN) {
    if (!DTN->getBlock())
      continue;

    ControlDependenceNode *node = bbMap[DTN->getBlock()];
    assert(node);

    cd_set_type cds;
    for (ControlDependenceNode::node_iterator P = node->Parents.begin(),
                                              E = node->Parents.end();
         P != E; ++P) {
      ControlDependenceNode *parent = *P;
      if (parent->TrueChildren.find(node) != parent->TrueChildren.end())
        cds.insert(std::make_pair(ControlDependenceNode::TRUE, parent));
      if (parent->FalseChildren.find(node) != parent->FalseChildren.end())
        cds.insert(std::make_pair(ControlDependenceNode::FALSE, parent));
      if (parent->OtherChildren.find(node) != parent->OtherChildren.end())
        cds.insert(std::make_pair(ControlDependenceNode::OTHER, parent));
    }

    cd_map_type::iterator CDEntry = cdMap.find(cds);
    ControlDependenceNode *region;
    if (CDEntry == cdMap.end()) {
      region = new ControlDependenceNode();
      nodes.insert(region);
      cdMap.insert(std::make_pair(cds, region));
      for (cd_set_type::iterator CD = cds.begin(), CDEnd = cds.end();
           CD != CDEnd; ++CD) {
        switch (CD->first) {
        case ControlDependenceNode::TRUE:
          CD->second->addTrue(region);
          break;
        case ControlDependenceNode::FALSE:
          CD->second->addFalse(region);
          break;
        case ControlDependenceNode::OTHER:
          CD->second->addOther(region);
          break;
        }
        region->addParent(CD->second);
      }
    } else {
      region = CDEntry->second;
    }
    for (cd_set_type::iterator CD = cds.begin(), CDEnd = cds.end(); CD != CDEnd;
         ++CD) {
      switch (CD->first) {
      case ControlDependenceNode::TRUE:
        CD->second->removeTrue(node);
        break;
      case ControlDependenceNode::FALSE:
        CD->second->removeFalse(node);
        break;
      case ControlDependenceNode::OTHER:
        CD->second->removeOther(node);
        break;
      }
      region->addOther(node);
      node->addParent(region);
      node->removeParent(CD->second);
    }
  }

  SmallSet<ControlDependenceNode *, 4> ToRemove;

  // Make sure that each node has at most one true or false edge
  for (std::set<ControlDependenceNode *>::iterator N = nodes.begin(),
                                                   E = nodes.end();
       N != E; ++N) {
    ControlDependenceNode *node = *N;
    assert(node);
    if (node->isRegion())
      continue;

    // Fix too many true nodes
    if (node->TrueChildren.size() > 1) {
      ControlDependenceNode *region = new ControlDependenceNode();
      nodes.insert(region);
      ToRemove.clear();
      for (ControlDependenceNode::node_iterator C = node->true_begin(),
                                                CE = node->true_end();
           C != CE; ++C) {
        ControlDependenceNode *child = *C;
        assert(node);
        assert(child);
        assert(region);
        region->addOther(child);
        child->addParent(region);
        child->removeParent(node);
        // node->removeTrue(child);
        ToRemove.insert(child);
      }
      for (auto *C : ToRemove)
        node->removeTrue(C);
      node->addTrue(region);
      region->addParent(node);
    }

    // Fix too many false nodes
    if (node->FalseChildren.size() > 1) {
      ControlDependenceNode *region = new ControlDependenceNode();
      nodes.insert(region);
      ToRemove.clear();
      for (ControlDependenceNode::node_iterator C = node->false_begin(),
                                                CE = node->false_end();
           C != CE; ++C) {
        ControlDependenceNode *child = *C;
        region->addOther(child);
        child->addParent(region);
        child->removeParent(node);
        // node->removeFalse(child);
        ToRemove.insert(child);
      }
      for (auto *C : ToRemove)
        node->removeTrue(C);
      node->addFalse(region);
      region->addParent(node);
    }
  }
}

void ControlDependenceGraphBase::graphForFunction(
    MachineFunction &F, MachinePostDominatorTree &pdt) {
  computeDependencies(F, pdt);
  insertRegions(pdt);
}

bool ControlDependenceGraphBase::controls(MachineBasicBlock *A,
                                          MachineBasicBlock *B) const {
  const ControlDependenceNode *n = getNode(B);
  assert(n && "Basic block not in control dependence graph!");
  while (n->getNumParents() == 1) {
    n = *n->parent_begin();
    if (n->getBlock() == A)
      return true;
  }
  return false;
}

bool ControlDependenceGraphBase::influences(MachineBasicBlock *A,
                                            MachineBasicBlock *B) const {
  const ControlDependenceNode *n = getNode(B);
  assert(n && "Basic block not in control dependence graph!");

  std::deque<ControlDependenceNode *> worklist;
  worklist.insert(worklist.end(), n->parent_begin(), n->parent_end());

  SmallPtrSet<ControlDependenceNode *, 8> Visited;
  while (!worklist.empty()) {
    auto *n = worklist.front();
    worklist.pop_front();
    if (Visited.contains(n))
      continue;
    Visited.insert(n);
    if (n->getBlock() == A)
      return true;
    worklist.insert(worklist.end(), n->parent_begin(), n->parent_end());
  }

  return false;
}

const ControlDependenceNode *
ControlDependenceGraphBase::enclosingRegion(MachineBasicBlock *BB) const {
  if (const ControlDependenceNode *node = this->getNode(BB)) {
    return node->enclosingRegion();
  } else {
    return NULL;
  }
}

} // namespace llvm

char ControlDependenceGraph::ID = 0;

char &llvm::ControlDependenceGraphID = MachineCDGPrinter::ID;

INITIALIZE_PASS(ControlDependenceGraph, DEBUG_TYPE, "Control Dependence Graph",
                false, true)

/// Default construct and initialize the pass.
ControlDependenceGraph::ControlDependenceGraph()
    : MachineFunctionPass(ID), ControlDependenceGraphBase() {
  initializeControlDependenceGraphPass(*PassRegistry::getPassRegistry());
}

static void writeMCDGToDotFile(MachineFunction &MF,
                               ControlDependenceGraph &MCDG) {
  std::string Filename = (".cdg." + MF.getName() + ".dot").str();
  errs() << "Writing '" << Filename << "'...";

  std::error_code EC;
  raw_fd_ostream File(Filename, EC, sys::fs::OF_Text);

  if (!EC)
    WriteGraph(File, &MCDG, false);
  else
    errs() << "  error opening file for writing!";
  errs() << '\n';
}

char MachineCDGPrinter::ID = 0;

char &llvm::MachineCDGPrinterID = MachineCDGPrinter::ID;

INITIALIZE_PASS(MachineCDGPrinter, "machine-cdg-printer", "Machine CDG Printer Pass",
                false, true)

/// Default construct and initialize the pass.
MachineCDGPrinter::MachineCDGPrinter() : MachineFunctionPass(ID) {
  initializeMachineCDGPrinterPass(*PassRegistry::getPassRegistry());
}

bool MachineCDGPrinter::runOnMachineFunction(MachineFunction &MF) {
  errs() << "Writing Machine CDG for function ";
  errs().write_escaped(MF.getName()) << '\n';

  writeMCDGToDotFile(MF, getAnalysis<ControlDependenceGraph>());
  return false;
}


//===- IntraProc/ControlDependenceGraph.h -----------------------*- C++ -*-===//
//
//                      Static Program Analysis for LLVM
//
// This file is based on a file distributed under a Modified BSD License (see below).
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

#ifndef CODEGEN_CONTROLDEPENDENCEGRAPH_H
#define CODEGEN_CONTROLDEPENDENCEGRAPH_H

#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/DOTGraphTraits.h"

#include <iterator>
#include <map>
#include <set>

namespace llvm {

class MachineBasicBlock;
class ControlDependenceGraphBase;

class ControlDependenceNode {
public:
  enum EdgeType { TRUE, FALSE, OTHER };
  typedef std::set<ControlDependenceNode *>::iterator node_iterator;
  typedef std::set<ControlDependenceNode *>::const_iterator const_node_iterator;

  struct edge_iterator {
    typedef node_iterator::value_type value_type;
    typedef node_iterator::difference_type difference_type;
    typedef node_iterator::reference reference;
    typedef node_iterator::pointer pointer;
    typedef std::input_iterator_tag iterator_category;

    edge_iterator(ControlDependenceNode *n)
        : node(n), stage(TRUE), it(n->TrueChildren.begin()),
          end(n->TrueChildren.end()) {
      while ((stage != OTHER) && (it == end))
        this->operator++();
    }
    edge_iterator(ControlDependenceNode *n, EdgeType t, node_iterator i,
                  node_iterator e)
        : node(n), stage(t), it(i), end(e) {
      while ((stage != OTHER) && (it == end))
        this->operator++();
    }
    EdgeType type() const { return stage; }
    bool operator==(edge_iterator const &other) const {
      return (this->stage == other.stage) && (this->it == other.it);
    }
    bool operator!=(edge_iterator const &other) const {
      return !(*this == other);
    }
    reference operator*() { return *this->it; }
    pointer operator->() { return &*this->it; }
    edge_iterator &operator++() {
      if (it != end)
        ++it;
      while ((stage != OTHER) && (it == end)) {
        if (stage == TRUE) {
          it = node->FalseChildren.begin();
          end = node->FalseChildren.end();
          stage = FALSE;
        } else {
          it = node->OtherChildren.begin();
          end = node->OtherChildren.end();
          stage = OTHER;
        }
      }
      return *this;
    }
    edge_iterator operator++(int) {
      edge_iterator ret(*this);
      assert(ret.stage == OTHER || ret.it != ret.end);
      this->operator++();
      return ret;
    }

  private:
    ControlDependenceNode *node;
    EdgeType stage;
    node_iterator it, end;
  };

  edge_iterator begin() { return edge_iterator(this); }
  edge_iterator end() {
    return edge_iterator(this, OTHER, OtherChildren.end(), OtherChildren.end());
  }

  node_iterator true_begin() { return TrueChildren.begin(); }
  node_iterator true_end() { return TrueChildren.end(); }

  node_iterator false_begin() { return FalseChildren.begin(); }
  node_iterator false_end() { return FalseChildren.end(); }

  node_iterator other_begin() { return OtherChildren.begin(); }
  node_iterator other_end() { return OtherChildren.end(); }

  node_iterator parent_begin() { return Parents.begin(); }
  node_iterator parent_end() { return Parents.end(); }
  const_node_iterator parent_begin() const { return Parents.begin(); }
  const_node_iterator parent_end() const { return Parents.end(); }

  MachineBasicBlock *getBlock() const { return TheBB; }
  size_t getNumParents() const { return Parents.size(); }
  size_t getNumChildren() const {
    return TrueChildren.size() + FalseChildren.size() + OtherChildren.size();
  }
  bool isRegion() const { return TheBB == NULL; }
  const ControlDependenceNode *enclosingRegion() const;

private:
  MachineBasicBlock *TheBB;
  std::set<ControlDependenceNode *> Parents;
  std::set<ControlDependenceNode *> TrueChildren;
  std::set<ControlDependenceNode *> FalseChildren;
  std::set<ControlDependenceNode *> OtherChildren;

  friend class ControlDependenceGraphBase;

  void clearAllChildren() {
    TrueChildren.clear();
    FalseChildren.clear();
    OtherChildren.clear();
  }
  void clearAllParents() { Parents.clear(); }

  void addTrue(ControlDependenceNode *Child);
  void addFalse(ControlDependenceNode *Child);
  void addOther(ControlDependenceNode *Child);
  void addParent(ControlDependenceNode *Parent);
  void removeTrue(ControlDependenceNode *Child);
  void removeFalse(ControlDependenceNode *Child);
  void removeOther(ControlDependenceNode *Child);
  void removeParent(ControlDependenceNode *Child);

  ControlDependenceNode() : TheBB(NULL) {}
  ControlDependenceNode(MachineBasicBlock *bb) : TheBB(bb) {}
};

template <> struct GraphTraits<ControlDependenceNode *> {
  // typedef ControlDependenceNode * NodeRef;
  using NodeRef = ControlDependenceNode *;
  typedef ControlDependenceNode::edge_iterator ChildIteratorType;

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static inline ChildIteratorType child_begin(NodeRef N) {
    return N->begin();
  }
  static inline ChildIteratorType child_end(NodeRef N) { return N->end(); }

  typedef df_iterator<ControlDependenceNode *> nodes_iterator;

  static nodes_iterator nodes_begin(NodeRef N) {
    return df_begin(getEntryNode(N));
  }
  static nodes_iterator nodes_end(NodeRef N) {
    return df_end(getEntryNode(N));
  }
};

class ControlDependenceGraphBase {
public:
  ControlDependenceGraphBase() : root(NULL) {}
  virtual ~ControlDependenceGraphBase() { releaseMemory(); }
  virtual void releaseMemory() {
    for (ControlDependenceNode::node_iterator n = nodes.begin(),
                                              e = nodes.end();
         n != e; ++n)
      delete *n;
    nodes.clear();
    bbMap.clear();
    root = NULL;
  }

  void graphForFunction(MachineFunction &F, MachinePostDominatorTree &pdt);

  ControlDependenceNode *getRoot() { return root; }
  const ControlDependenceNode *getRoot() const { return root; }
  ControlDependenceNode *operator[](const MachineBasicBlock *BB) {
    return getNode(BB);
  }
  const ControlDependenceNode *operator[](const MachineBasicBlock *BB) const {
    return getNode(BB);
  }
  ControlDependenceNode *getNode(const MachineBasicBlock *BB) {
    return bbMap[BB];
  }
  const ControlDependenceNode *getNode(const MachineBasicBlock *BB) const {
    return (bbMap.find(BB) != bbMap.end()) ? bbMap.find(BB)->second : NULL;
  }
  bool controls(MachineBasicBlock *A, MachineBasicBlock *B) const;
  bool influences(MachineBasicBlock *A, MachineBasicBlock *B) const;
  const ControlDependenceNode *enclosingRegion(MachineBasicBlock *BB) const;

private:
  ControlDependenceNode *root;
  std::set<ControlDependenceNode *> nodes;
  std::map<const MachineBasicBlock *, ControlDependenceNode *> bbMap;
  static ControlDependenceNode::EdgeType getEdgeType(const MachineBasicBlock *,
                                                     const MachineBasicBlock *);
  void computeDependencies(MachineFunction &F, MachinePostDominatorTree &pdt);
  void insertRegions(MachinePostDominatorTree &pdt);
};

class ControlDependenceGraph : public MachineFunctionPass,
                               public ControlDependenceGraphBase {
public:
  static char ID;

  ControlDependenceGraph();
  virtual ~ControlDependenceGraph() {}
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachinePostDominatorTree>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
  bool runOnMachineFunction(MachineFunction &F) override {
    MachinePostDominatorTree &pdt = getAnalysis<MachinePostDominatorTree>();
    graphForFunction(F, pdt);
    return false;
  }
};

template <>
struct GraphTraits<ControlDependenceGraph *>
    : public GraphTraits<ControlDependenceNode *> {
  static NodeRef getEntryNode(ControlDependenceGraph *CD) {
    return CD->getRoot();
  }

  static nodes_iterator nodes_begin(ControlDependenceGraph *CD) {
    if (getEntryNode(CD))
      return df_begin(getEntryNode(CD));
    else
      return df_end(getEntryNode(CD));
  }

  static nodes_iterator nodes_end(ControlDependenceGraph *CD) {
    return df_end(getEntryNode(CD));
  }
};

template <>
struct DOTGraphTraits<ControlDependenceGraph *> : public DefaultDOTGraphTraits {
  DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}

  static std::string getGraphName(ControlDependenceGraph *Graph) {
    return "Control dependence graph";
  }

  std::string getNodeLabel(ControlDependenceNode *Node,
                           ControlDependenceGraph *Graph) {
    if (Node->isRegion()) {
      return "REGION";
    } else {
      return Node->getBlock()->getFullName();
    }
  }

  static std::string
  getEdgeSourceLabel(ControlDependenceNode *Node,
                     ControlDependenceNode::edge_iterator I) {
    switch (I.type()) {
    case ControlDependenceNode::TRUE:
      return "T";
    case ControlDependenceNode::FALSE:
      return "F";
    case ControlDependenceNode::OTHER:
      return "";
    }
  }
};

class MachineCDGPrinter : public MachineFunctionPass {
public:
  static char ID;

  MachineCDGPrinter();

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<ControlDependenceGraph>();
    AU.setPreservesAll();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // namespace llvm

#endif // CODEGEN_CONTROLDEPENDENCEGRAPH_H
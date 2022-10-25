#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/TrackSecrets.h"
#include "llvm/CodeGen/MachineFunction.h"
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
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"


using namespace llvm;

#define DEBUG_TYPE "track-secrets"

char TrackSecretsAnalysis::ID = 0;

/// Propagates given input secrets through the given basic block and stores them
bool TrackSecretsAnalysis::transfer(MachineBasicBlock &BB, SmallVector<MachineOperand>) {
  auto Out = BBOuts.lookup(&BB);
  auto Changed = false;
  
  // For each instruction ...
  
  // Return true of Out was changed
  return Changed;
}

/// Computes secret inputs for the given basic block by joining secret outputs of predecessors
SmallVector<MachineOperand> TrackSecretsAnalysis::join(MachineBasicBlock &BB) {
  auto In = SmallVector<MachineOperand>();
  
  // For each basic block in pred iterator ...
  
  return In;
}

bool TrackSecretsAnalysis::runOnMachineFunction(MachineFunction &MF) {
  auto FS = getAnalysis<FindSecretsAnalysis>().Secrets;
  
  // TODO: track flow of secrets
  
  // Initialize BBOuts: create vector for each basic block
  
  // Initialize entry block: transfer with secret arguments of MF
  
  // While BBOuts is changing:
    // For each basic block: join and transfer
  
  // TODO: later, use work list, more efficient
  
  return false;
}

TrackSecretsAnalysis::TrackSecretsAnalysis() : MachineFunctionPass(ID), Secrets() {
  initializeMachineCFGPrinterPass(*PassRegistry::getPassRegistry());
}

INITIALIZE_PASS_BEGIN(TrackSecretsAnalysis, DEBUG_TYPE,
    "Track secrets", false, true)
INITIALIZE_PASS_DEPENDENCY(FindSecretsAnalysis)
INITIALIZE_PASS_END(TrackSecretsAnalysis, DEBUG_TYPE,
    "Track secrets", false, true)

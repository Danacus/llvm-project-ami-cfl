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

bool TrackSecretsAnalysis::runOnMachineFunction(MachineFunction &MF) {
  auto FS = getAnalysis<FindSecretsAnalysis>().Secrets;
  
  // TODO: track flow of secrets
  
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

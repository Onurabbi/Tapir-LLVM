//===- LoopSpawning.cpp - Spawn loop iterations efficiently ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Modify Tapir loops to spawn their iterations efficiently.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Tapir/LoopSpawning.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/OptimizationDiagnosticInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/IndVarSimplify.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/LoopDeletion.h"
#include "llvm/Transforms/Tapir.h"
#include "llvm/Transforms/Tapir/Outline.h"
#include "llvm/Transforms/Tapir/PTXABI.h"
#include "llvm/Transforms/Tapir/TapirUtils.h"
#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/TapirUtils.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include <utility>

using std::make_pair;

using namespace llvm;

#define DEBUG_TYPE LS_NAME

STATISTIC(LoopsAnalyzed, "Number of Tapir loops analyzed");

static cl::opt<TapirTargetType> ClTapirTarget(
    "ls-tapir-target", cl::desc("Target runtime for Tapir"),
    cl::init(TapirTargetType::Cilk),
    cl::values(clEnumValN(TapirTargetType::None,
                          "none", "None"),
               clEnumValN(TapirTargetType::Serial,
                          "serial", "Serial code"),
               clEnumValN(TapirTargetType::Cilk,
                          "cilk", "Cilk Plus"),
               clEnumValN(TapirTargetType::OpenMP,
                          "openmp", "OpenMP"),
               clEnumValN(TapirTargetType::Qthreads,
                          "qthreads", "Qthreads"),
               clEnumValN(TapirTargetType::PTX,
                          "ptx", "PTX")));

namespace {

static void emitMissedWarning(Function *F, Loop *L,
                              const LoopSpawningHints &LH,
                              OptimizationRemarkEmitter *ORE) {
  switch (LH.getStrategy()) {
  case LoopSpawningHints::ST_DAC:
    ORE->emit(DiagnosticInfoOptimizationFailure(
                  DEBUG_TYPE, "FailedRequestedSpawning",
                  L->getStartLoc(), L->getHeader())
              << "Tapir loop not transformed: "
              << "failed to use divide-and-conquer loop spawning");
    break;
  case LoopSpawningHints::ST_GPU:
    ORE->emit(DiagnosticInfoOptimizationFailure(
                  DEBUG_TYPE, "FailedRequestedSpawning",
                  L->getStartLoc(), L->getHeader())
              << "Tapir loop not transformed: "
              << "failed to use GPU loop spawning");
    break;
  case LoopSpawningHints::ST_SEQ:
    ORE->emit(DiagnosticInfoOptimizationFailure(
                  DEBUG_TYPE, "SpawningDisabled",
                  L->getStartLoc(), L->getHeader())
              << "Tapir loop not transformed: "
              << "loop-spawning transformation disabled");
    break;
  case LoopSpawningHints::ST_END:
    ORE->emit(DiagnosticInfoOptimizationFailure(
                  DEBUG_TYPE, "FailedRequestedSpawning",
                  L->getStartLoc(), L->getHeader())
              << "Tapir loop not transformed: "
              << "unknown loop-spawning strategy");
    break;
  }
}

struct LoopSpawningImpl {
  LoopSpawningImpl(Function &F,
                   LoopInfo &LI,
                   ScalarEvolution &SE,
                   DominatorTree &DT,
                   AssumptionCache &AC,
                   OptimizationRemarkEmitter &ORE,
                   TapirTarget* tapirTarget)
      : F(F), LI(LI), SE(SE), DT(DT), AC(AC), ORE(ORE), tapirTarget(tapirTarget) {}

  bool run();

private:
  void addTapirLoop(Loop *L, SmallVectorImpl<Loop *> &V);
  bool processLoop(Loop *L);

  Function &F;
  LoopInfo &LI;
  ScalarEvolution &SE;
  DominatorTree &DT;
  AssumptionCache &AC;
  OptimizationRemarkEmitter &ORE;

  TapirTarget* tapirTarget;
};
} // end anonymous namespace

/// Canonicalize the induction variables in the loop.  Return the canonical
/// induction variable created or inserted by the scalar evolution expander.
PHINode* LoopOutline::canonicalizeIVs(Type *Ty) {
  Loop *L = OrigLoop;

  BasicBlock* Header = L->getHeader();
  Module* M = Header->getParent()->getParent();
  const DataLayout &DL = M->getDataLayout();

  SCEVExpander Exp(SE, DL, "ls");

  PHINode *CanonicalIV = Exp.getOrInsertCanonicalInductionVariable(L, Ty);
  DEBUG(dbgs() << "LS Canonical induction variable " << *CanonicalIV << "\n");

  SmallVector<WeakTrackingVH, 16> DeadInsts;
  Exp.replaceCongruentIVs(L, DT, DeadInsts);
  for (WeakTrackingVH V : DeadInsts) {
    DEBUG(dbgs() << "LS erasing dead inst " << *V << "\n");
    Instruction *I = cast<Instruction>(V);
    I->eraseFromParent();
  }

  return CanonicalIV;
}

// IVs is output
bool LoopOutline::removeNonCanonicalIVs(BasicBlock* Header, BasicBlock* Preheader, PHINode* CanonicalIV, SmallVector<PHINode*, 8> &IVs, SCEVExpander &Exp) {
  // Remove all IV's other than CanonicalIV.
  // First, check that we can do this.
  bool CanRemoveIVs = true;
  for (BasicBlock::iterator II = Header->begin(); isa<PHINode>(II); ++II) {
    PHINode *PN = cast<PHINode>(II);
    if (CanonicalIV == PN) continue;
    const SCEV *S = SE.getSCEV(PN);
    if (SE.getCouldNotCompute() == S) {
      ORE.emit(OptimizationRemarkAnalysis(LS_NAME, "NoSCEV", PN)
               << "could not compute scalar evolution of "
               << ore::NV("PHINode", PN));
      CanRemoveIVs = false;
    }
  }

  if (!CanRemoveIVs) {
    DEBUG(dbgs() << "Could not compute scalar evolutions for all IV's.\n");
    return false;
  }

  {
    SmallVector<PHINode*, 8> IVsToRemove;
    for (BasicBlock::iterator II = Header->begin(); isa<PHINode>(II); ++II) {
      PHINode *PN = cast<PHINode>(II);
      if (PN == CanonicalIV) continue;
      const SCEV *S = SE.getSCEV(PN);
      DEBUG(dbgs() << "Removing the IV " << *PN << " (" << *S << ")\n");
      ORE.emit(OptimizationRemarkAnalysis(LS_NAME, "RemoveIV", PN)
               << "removing the IV "
               << ore::NV("PHINode", PN));
      Value *NewIV = Exp.expandCodeFor(S, S->getType(), CanonicalIV);
      PN->replaceAllUsesWith(NewIV);
      IVsToRemove.push_back(PN);
    }
    for (PHINode *PN : IVsToRemove)
      PN->eraseFromParent();
  }

  // All remaining IV's should be canonical.  Collect them.
  //
  // TODO?: We can probably adapt this loop->DAC process such that we
  // don't require all IV's to be canonical.
  bool AllCanonical = true;
  for (BasicBlock::iterator II = Header->begin(); isa<PHINode>(II); ++II) {
    PHINode *PN = cast<PHINode>(II);
    DEBUG({
        const SCEVAddRecExpr *PNSCEV =
          dyn_cast<const SCEVAddRecExpr>(SE.getSCEV(PN));
        assert(PNSCEV && "PHINode did not have corresponding SCEVAddRecExpr");
        assert(PNSCEV->getStart()->isZero() &&
               "PHINode SCEV does not start at 0");
        dbgs() << "LS step recurrence for SCEV " << *PNSCEV << " is "
               << *(PNSCEV->getStepRecurrence(SE)) << "\n";
        assert(PNSCEV->getStepRecurrence(SE)->isOne() &&
               "PHINode SCEV step is not 1");
      });
    if (ConstantInt *C =
        dyn_cast<ConstantInt>(PN->getIncomingValueForBlock(Preheader))) {
      if (C->isZero()) {
        DEBUG({
            if (PN != CanonicalIV) {
              const SCEVAddRecExpr *PNSCEV =
                dyn_cast<const SCEVAddRecExpr>(SE.getSCEV(PN));
              dbgs() << "Saving the canonical IV " << *PN << " (" << *PNSCEV << ")\n";
            }
          });
        if (PN != CanonicalIV)
          ORE.emit(OptimizationRemarkAnalysis(LS_NAME, "SaveIV", PN)
                   << "saving the canonical the IV "
                   << ore::NV("PHINode", PN));
        IVs.push_back(PN);
      }
    } else {
      AllCanonical = false;
      DEBUG(dbgs() << "Remaining non-canonical PHI Node found: " << *PN <<
            "\n");
      // emitAnalysis(LoopSpawningReport(PN)
      //              << "Found a remaining non-canonical IV.\n");
      ORE.emit(OptimizationRemarkAnalysis(DEBUG_TYPE, "NonCanonicalIV", PN)
               << "found a remaining noncanonical IV");
    }
  }
  if (!AllCanonical)
    return false;   
}

// TODO
/*
bool LoopOutline::setIVStartingValues(Value* newStart, Value* NewCanonicalIV, BasicBlock* NewPreheader) {
    if (auto startInst = dyn_cast<Instruction>(NewPreheader)) {
        assert(DT->dominates(startInst, NewPreheader->getTerminator()));
    }

    {
      int NewPreheaderIdx = NewCanonicalIV->getBasicBlockIndex(NewPreheader);
      assert(isa<Constant>(NewCanonicalIV->getIncomingValue(NewPreheaderIdx)) &&
             "Cloned canonical IV does not inherit a constant value from cloned preheader.");
      NewCanonicalIV->setIncomingValue(NewPreheaderIdx, newStart);
    }

    // Rewrite other cloned IV's to start at their value at the start
    // iteration.
    const SCEV *StartIterSCEV = SE.getSCEV(newStart);
    DEBUG(dbgs() << "StartIterSCEV: " << *StartIterSCEV << "\n");
    for (PHINode *IV : IVs) {
      if (CanonicalIV == IV) continue;

      // Get the value of the IV at the start iteration.
      DEBUG(dbgs() << "IV " << *IV);
      const SCEV *IVSCEV = SE.getSCEV(IV);
      DEBUG(dbgs() << " (SCEV " << *IVSCEV << ")");
      const SCEVAddRecExpr *IVSCEVAddRec = cast<const SCEVAddRecExpr>(IVSCEV);
      const SCEV *IVAtIter = IVSCEVAddRec->evaluateAtIteration(StartIterSCEV, SE);
      DEBUG(dbgs() << " expands at iter " << *StartIterSCEV <<
            " to " << *IVAtIter << "\n");

      // NOTE: Expanded code should not refer to other IV's.
      Value *IVStart = Exp.expandCodeFor(IVAtIter, IVAtIter->getType(),
                                         NewPreheader->getTerminator());

      // Set the value that the cloned IV inherits from the cloned preheader.
      PHINode *NewIV = cast<PHINode>(VMap[IV]);
      int NewPreheaderIdx = NewIV->getBasicBlockIndex(NewPreheader);
      assert(isa<Constant>(NewIV->getIncomingValue(NewPreheaderIdx)) &&
             "Cloned IV does not inherit a constant value from cloned preheader.");
      NewIV->setIncomingValue(NewPreheaderIdx, IVStart);
    }
}
*/

/// \brief Replace the latch of the loop to check that IV is always less than or
/// equal to the limit.
///
/// This method assumes that the loop has a single loop latch.
Value* LoopOutline::canonicalizeLoopLatch(PHINode *IV, Value *Limit) {
  Loop *L = OrigLoop;

  Value *NewCondition;
  BasicBlock *Header = L->getHeader();
  BasicBlock *Latch = L->getLoopLatch();
  assert(Latch && "No single loop latch found for loop.");

  IRBuilder<> Builder(&*Latch->getFirstInsertionPt());

  // This process assumes that IV's increment is in Latch.

  // Create comparison between IV and Limit at top of Latch.
  NewCondition = Builder.CreateICmpULT(IV, Limit);

  // Replace the conditional branch at the end of Latch.
  BranchInst *LatchBr = dyn_cast_or_null<BranchInst>(Latch->getTerminator());
  assert(LatchBr && LatchBr->isConditional() &&
         "Latch does not terminate with a conditional branch.");
  Builder.SetInsertPoint(Latch->getTerminator());
  Builder.CreateCondBr(NewCondition, Header, ExitBlock);

  // Erase the old conditional branch.
  Value *OldCond = LatchBr->getCondition();
  LatchBr->eraseFromParent();
  if (!OldCond->hasNUsesOrMore(1))
    if (Instruction *OldCondInst = dyn_cast<Instruction>(OldCond))
      OldCondInst->eraseFromParent();

  return NewCondition;
}

/// Unlink the specified loop, and update analysis accordingly.  The heavy
/// lifting of deleting the loop is carried out by a run of LoopDeletion after
/// this pass.
void LoopOutline::unlinkLoop() {
  Loop *L = OrigLoop;

  // Get components of the old loop.
  BasicBlock *Preheader = L->getLoopPreheader();
  assert(Preheader && "Loop does not have a unique preheader.");
  BasicBlock *Latch = L->getLoopLatch();

  // Invalidate the analysis of the old loop.
  SE.forgetLoop(L);

  // Redirect the preheader to branch directly to loop exit.
  assert(1 == Preheader->getTerminator()->getNumSuccessors() &&
         "Preheader does not have a unique successor.");
  Preheader->getTerminator()->replaceUsesOfWith(L->getHeader(),
                                                ExitBlock);

  // Rewrite phis in the exit block to get their inputs from
  // the preheader instead of the exiting block.
  BasicBlock::iterator BI = ExitBlock->begin();
  while (PHINode *P = dyn_cast<PHINode>(BI)) {
    int j = P->getBasicBlockIndex(Latch);
    assert(j >= 0 && "Can't find exiting block in exit block's phi node!");
    P->setIncomingBlock(j, Preheader);
    P->removeIncomingValue(Latch);
    ++BI;
  }

  // Rewrite phis in the header block to not receive an input from
  // the preheader.
  BI = L->getHeader()->begin();
  while (PHINode *P = dyn_cast<PHINode>(BI)) {
    P->removeIncomingValue(Preheader);
    ++BI;
  }
}

/// This routine recursively examines all descendants of the specified loop and
/// adds all Tapir loops in that tree to the vector.  This routine performs a
/// pre-order traversal of the tree of loops and pushes each Tapir loop found
/// onto the end of the vector.
void LoopSpawningImpl::addTapirLoop(Loop *L, SmallVectorImpl<Loop *> &V) {
  if (isCanonicalTapirLoop(L)) {
    V.push_back(L);
    return;
  }

  LoopSpawningHints Hints(L);

  DEBUG(dbgs() << "LS: Loop hints:"
               << " strategy = " << Hints.printStrategy(Hints.getStrategy())
               << " grainsize = " << Hints.getGrainsize()
               << "\n");

  using namespace ore;

  if (LoopSpawningHints::ST_SEQ != Hints.getStrategy()) {
    DEBUG(dbgs() << "LS: Marked loop is not a valid Tapir loop.\n"
          << "\tLoop hints:"
          << " strategy = " << Hints.printStrategy(Hints.getStrategy())
          << "\n");
    ORE.emit(OptimizationRemarkMissed(LS_NAME, "NotTapir",
                                      L->getStartLoc(), L->getHeader())
             << "marked loop is not a valid Tapir loop");
  }

  for (Loop *InnerL : *L)
    addTapirLoop(InnerL, V);
}

#ifndef NDEBUG
/// \return string containing a file name and a line # for the given loop.
static std::string getDebugLocString(const Loop *L) {
  std::string Result;
  if (L) {
    raw_string_ostream OS(Result);
    if (const DebugLoc LoopDbgLoc = L->getStartLoc())
      LoopDbgLoc.print(OS);
    else
      // Just print the module name.
      OS << L->getHeader()->getParent()->getParent()->getModuleIdentifier();
    OS.flush();
  }
  return Result;
}
#endif

bool LoopSpawningImpl::run() {
  // Build up a worklist of inner-loops to vectorize. This is necessary as
  // the act of vectorizing or partially unrolling a loop creates new loops
  // and can invalidate iterators across the loops.
  SmallVector<Loop *, 8> Worklist;

  // Examine all top-level loops in this function, and call addTapirLoop to push
  // those loops onto the work list.
  for (Loop *L : LI)
    addTapirLoop(L, Worklist);

  LoopsAnalyzed += Worklist.size();

  // Now walk the identified inner loops.
  bool Changed = false;
  while (!Worklist.empty())
    // Process the work list of loops backwards.  For each tree of loops in this
    // function, addTapirLoop pushed those loops onto the work list according to
    // a pre-order tree traversal.  Therefore, processing the work list
    // backwards leads us to process innermost loops first.
    Changed |= processLoop(Worklist.pop_back_val());

  // Process each loop nest in the function.
  return Changed;
}


// Top-level routine to process a given loop.
bool LoopSpawningImpl::processLoop(Loop *L) {
#ifndef NDEBUG
  const std::string DebugLocStr = getDebugLocString(L);
#endif /* NDEBUG */

  // Function containing loop
  Function *F = L->getHeader()->getParent();

  DEBUG(dbgs() << "\nLS: Checking a Tapir loop in \""
               << L->getHeader()->getParent()->getName() << "\" from "
        << DebugLocStr << ": " << *L << "\n");

  LoopSpawningHints Hints(L);

  DEBUG(dbgs() << "LS: Loop hints:"
               << " strategy = " << Hints.printStrategy(Hints.getStrategy())
               << " grainsize = " << Hints.getGrainsize()
               << "\n");

  using namespace ore;

  // Get the loop preheader.  LoopSimplify should guarantee that the loop
  // preheader is not terminated by a sync.
  BasicBlock *Preheader = L->getLoopPreheader();
  if (!Preheader) {
    DEBUG(dbgs() << "LS: Loop lacks a preheader.\n");
    ORE.emit(OptimizationRemarkMissed(LS_NAME, "NoPreheader",
                                      L->getStartLoc(), L->getHeader())
             << "loop lacks a preheader");
    emitMissedWarning(F, L, Hints, &ORE);
    return false;
  } else if (!isa<BranchInst>(Preheader->getTerminator())) {
    DEBUG(dbgs() << "LS: Loop preheader is not terminated by a branch.\n");
    ORE.emit(OptimizationRemarkMissed(LS_NAME, "ComplexPreheader",
                                      L->getStartLoc(), L->getHeader())
             << "loop preheader not terminated by a branch");
    emitMissedWarning(F, L, Hints, &ORE);
    return false;
  }

  switch(Hints.getStrategy()) {
  case LoopSpawningHints::ST_SEQ:
    DEBUG(dbgs() << "LS: Hints dictate sequential spawning.\n");
    break;
  default:
    return tapirTarget->processLoop(Hints, LI, SE, DT, AC, ORE);
  case LoopSpawningHints::ST_END:
    dbgs() << "LS: Hints specify unknown spawning strategy.\n";
    break;
  }
  return false;
}

PreservedAnalyses LoopSpawningPass::run(Function &F,
                                        FunctionAnalysisManager &AM) {
  // Determine if function detaches.
  bool DetachingFunction = false;
  for (BasicBlock &BB : F)
    if (isa<DetachInst>(BB.getTerminator()))
      DetachingFunction = true;

  if (!DetachingFunction)
    return PreservedAnalyses::all();

  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &AC = AM.getResult<AssumptionAnalysis>(F);
  auto &ORE =
    AM.getResult<OptimizationRemarkEmitterAnalysis>(F);

  bool Changed = LoopSpawningImpl(F, LI, SE, DT, AC, ORE, tapirTarget).run();

  AM.invalidate<ScalarEvolutionAnalysis>(F);

  if (Changed)
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}

namespace {
struct LoopSpawning : public FunctionPass {
  /// Pass identification, replacement for typeid
  static char ID;
  TapirTarget* tapirTarget;
  explicit LoopSpawning(TapirTarget* tapirTarget = nullptr)
      : FunctionPass(ID), tapirTarget(tapirTarget) {
    if (!this->tapirTarget)
      this->tapirTarget = getTapirTargetFromType(ClTapirTarget);

    assert(this->tapirTarget);
    initializeLoopSpawningPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override {
    if (skipFunction(F))
      return false;

    bool DetachingFunction = false;
    for (BasicBlock &BB : F)
      if (isa<DetachInst>(BB.getTerminator()))
        DetachingFunction = true;

    if (!DetachingFunction)
      return false;

    auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    auto &SE = getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    auto &DT = getAnalysis<DominatorTreeWrapperPass>().getDomTree();
    auto &AC = getAnalysis<AssumptionCacheTracker>().getAssumptionCache(F);
    auto &ORE =
      getAnalysis<OptimizationRemarkEmitterWrapperPass>().getORE();
    // OptimizationRemarkEmitter ORE(F);

    return LoopSpawningImpl(F, LI, SE, DT, AC, ORE, tapirTarget).run();
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AssumptionCacheTracker>();
    AU.addRequiredID(LoopSimplifyID);
    AU.addRequiredID(LCSSAID);
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<TargetTransformInfoWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<OptimizationRemarkEmitterWrapperPass>();
  }
};
}

char LoopSpawning::ID = 0;
static const char ls_name[] = "Loop Spawning";
INITIALIZE_PASS_BEGIN(LoopSpawning, LS_NAME, ls_name, false, false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_DEPENDENCY(LCSSAWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(OptimizationRemarkEmitterWrapperPass)
INITIALIZE_PASS_END(LoopSpawning, LS_NAME, ls_name, false, false)

namespace llvm {
Pass *createLoopSpawningPass(TapirTarget* target) {
  return new LoopSpawning(target);
}
}

//===- LoopVectorize.cpp - A Loop Vectorizer ------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This is the LLVM loop vectorizer. This pass modifies 'vectorizable' loops
// and generates target-independent LLVM-IR. Legalization of the IR is done
// in the codegen. However, the vectorizes uses (will use) the codegen
// interfaces to generate IR that is likely to result in an optimal binary.
//
// The loop vectorizer combines consecutive loop iteration into a single
// 'wide' iteration. After this transformation the index is incremented
// by the SIMD vector width, and not by one.
//
// This pass has three parts:
// 1. The main loop pass that drives the different parts.
// 2. LoopVectorizationLegality - A unit that checks for the legality
//    of the vectorization.
// 3. InnerLoopVectorizer - A unit that performs the actual
//    widening of instructions.
// 4. LoopVectorizationCostModel - A unit that checks for the profitability
//    of vectorization. It decides on the optimal vector width, which
//    can be one, if vectorization is not profitable.
//
//===----------------------------------------------------------------------===//
//
// The reduction-variable vectorization is based on the paper:
//  D. Nuzman and R. Henderson. Multi-platform Auto-vectorization.
//
// Variable uniformity checks are inspired by:
// Karrenberg, R. and Hack, S. Whole Function Vectorization.
//
// Other ideas/concepts are from:
//  A. Zaks and D. Nuzman. Autovectorization in GCC-two years later.
//
//  S. Maleki, Y. Gao, M. Garzaran, T. Wong and D. Padua.  An Evaluation of
//  Vectorizing Compilers.
//
//===----------------------------------------------------------------------===//

#define LV_NAME "loop-vectorize"
#define DEBUG_TYPE LV_NAME

#include "llvm/Transforms/Vectorize.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include <algorithm>
#include <map>

using namespace llvm;

static cl::opt<unsigned>
VectorizationFactor("force-vector-width", cl::init(0), cl::Hidden,
                    cl::desc("Sets the SIMD width. Zero is autoselect."));

static cl::opt<unsigned>
VectorizationUnroll("force-vector-unroll", cl::init(0), cl::Hidden,
                    cl::desc("Sets the vectorization unroll count. "
                             "Zero is autoselect."));

static cl::opt<bool>
EnableIfConversion("enable-if-conversion", cl::init(true), cl::Hidden,
                   cl::desc("Enable if-conversion during vectorization."));

/// We don't vectorize loops with a known constant trip count below this number.
static const unsigned TinyTripCountVectorThreshold = 16;

/// We don't unroll loops with a known constant trip count below this number.
static const unsigned TinyTripCountUnrollThreshold = 128;

/// We don't unroll loops that are larget than this threshold.
static const unsigned MaxLoopSizeThreshold = 32;

/// When performing a runtime memory check, do not check more than this
/// number of pointers. Notice that the check is quadratic!
static const unsigned RuntimeMemoryCheckThreshold = 4;

namespace {

// Forward declarations.
class LoopVectorizationLegality;
class LoopVectorizationCostModel;

/// InnerLoopVectorizer vectorizes loops which contain only one basic
/// block to a specified vectorization factor (VF).
/// This class performs the widening of scalars into vectors, or multiple
/// scalars. This class also implements the following features:
/// * It inserts an epilogue loop for handling loops that don't have iteration
///   counts that are known to be a multiple of the vectorization factor.
/// * It handles the code generation for reduction variables.
/// * Scalarization (implementation using scalars) of un-vectorizable
///   instructions.
/// InnerLoopVectorizer does not perform any vectorization-legality
/// checks, and relies on the caller to check for the different legality
/// aspects. The InnerLoopVectorizer relies on the
/// LoopVectorizationLegality class to provide information about the induction
/// and reduction variables that were found to a given vectorization factor.
class InnerLoopVectorizer {
public:
  InnerLoopVectorizer(Loop *OrigLoop, ScalarEvolution *SE, LoopInfo *LI,
                      DominatorTree *DT, DataLayout *DL, unsigned VecWidth,
                      unsigned UnrollFactor)
      : OrigLoop(OrigLoop), SE(SE), LI(LI), DT(DT), DL(DL), VF(VecWidth),
        UF(UnrollFactor), Builder(SE->getContext()), Induction(0),
        OldInduction(0), WidenMap(UnrollFactor) {}

  // Perform the actual loop widening (vectorization).
  void vectorize(LoopVectorizationLegality *Legal) {
    // Create a new empty loop. Unlink the old loop and connect the new one.
    createEmptyLoop(Legal);
    // Widen each instruction in the old loop to a new one in the new loop.
    // Use the Legality module to find the induction and reduction variables.
    vectorizeLoop(Legal);
    // Register the new loop and update the analysis passes.
    updateAnalysis();
  }

private:
  /// A small list of PHINodes.
  typedef SmallVector<PHINode*, 4> PhiVector;
  /// When we unroll loops we have multiple vector values for each scalar.
  /// This data structure holds the unrolled and vectorized values that
  /// originated from one scalar instruction.
  typedef SmallVector<Value*, 2> VectorParts;

  /// Add code that checks at runtime if the accessed arrays overlap.
  /// Returns the comparator value or NULL if no check is needed.
  Value *addRuntimeCheck(LoopVectorizationLegality *Legal,
                         Instruction *Loc);
  /// Create an empty loop, based on the loop ranges of the old loop.
  void createEmptyLoop(LoopVectorizationLegality *Legal);
  /// Copy and widen the instructions from the old loop.
  void vectorizeLoop(LoopVectorizationLegality *Legal);

  /// A helper function that computes the predicate of the block BB, assuming
  /// that the header block of the loop is set to True. It returns the *entry*
  /// mask for the block BB.
  VectorParts createBlockInMask(BasicBlock *BB);
  /// A helper function that computes the predicate of the edge between SRC
  /// and DST.
  VectorParts createEdgeMask(BasicBlock *Src, BasicBlock *Dst);

  /// A helper function to vectorize a single BB within the innermost loop.
  void vectorizeBlockInLoop(LoopVectorizationLegality *Legal, BasicBlock *BB,
                            PhiVector *PV);

  /// Insert the new loop to the loop hierarchy and pass manager
  /// and update the analysis passes.
  void updateAnalysis();

  /// This instruction is un-vectorizable. Implement it as a sequence
  /// of scalars.
  void scalarizeInstruction(Instruction *Instr);

  /// Create a broadcast instruction. This method generates a broadcast
  /// instruction (shuffle) for loop invariant values and for the induction
  /// value. If this is the induction variable then we extend it to N, N+1, ...
  /// this is needed because each iteration in the loop corresponds to a SIMD
  /// element.
  Value *getBroadcastInstrs(Value *V);

  /// This function adds 0, 1, 2 ... to each vector element, starting at zero.
  /// If Negate is set then negative numbers are added e.g. (0, -1, -2, ...).
  /// The sequence starts at StartIndex.
  Value *getConsecutiveVector(Value* Val, unsigned StartIdx, bool Negate);

  /// When we go over instructions in the basic block we rely on previous
  /// values within the current basic block or on loop invariant values.
  /// When we widen (vectorize) values we place them in the map. If the values
  /// are not within the map, they have to be loop invariant, so we simply
  /// broadcast them into a vector.
  VectorParts &getVectorValue(Value *V);

  /// Generate a shuffle sequence that will reverse the vector Vec.
  Value *reverseVector(Value *Vec);

  /// This is a helper class that holds the vectorizer state. It maps scalar
  /// instructions to vector instructions. When the code is 'unrolled' then
  /// then a single scalar value is mapped to multiple vector parts. The parts
  /// are stored in the VectorPart type.
  struct ValueMap {
    /// C'tor.  UnrollFactor controls the number of vectors ('parts') that
    /// are mapped.
    ValueMap(unsigned UnrollFactor) : UF(UnrollFactor) {}

    /// \return True if 'Key' is saved in the Value Map.
    bool has(Value *Key) { return MapStoreage.count(Key); }

    /// Initializes a new entry in the map. Sets all of the vector parts to the
    /// save value in 'Val'.
    /// \return A reference to a vector with splat values.
    VectorParts &splat(Value *Key, Value *Val) {
      MapStoreage[Key].clear();
      MapStoreage[Key].append(UF, Val);
      return MapStoreage[Key];
    }

    ///\return A reference to the value that is stored at 'Key'.
    VectorParts &get(Value *Key) {
      if (!has(Key))
        MapStoreage[Key].resize(UF);
      return MapStoreage[Key];
    }

    /// The unroll factor. Each entry in the map stores this number of vector
    /// elements.
    unsigned UF;

    /// Map storage. We use std::map and not DenseMap because insertions to a
    /// dense map invalidates its iterators.
    std::map<Value*, VectorParts> MapStoreage;
  };

  /// The original loop.
  Loop *OrigLoop;
  /// Scev analysis to use.
  ScalarEvolution *SE;
  /// Loop Info.
  LoopInfo *LI;
  /// Dominator Tree.
  DominatorTree *DT;
  /// Data Layout.
  DataLayout *DL;
  /// The vectorization SIMD factor to use. Each vector will have this many
  /// vector elements.
  unsigned VF;
  /// The vectorization unroll factor to use. Each scalar is vectorized to this
  /// many different vector instructions.
  unsigned UF;

  /// The builder that we use
  IRBuilder<> Builder;

  // --- Vectorization state ---

  /// The vector-loop preheader.
  BasicBlock *LoopVectorPreHeader;
  /// The scalar-loop preheader.
  BasicBlock *LoopScalarPreHeader;
  /// Middle Block between the vector and the scalar.
  BasicBlock *LoopMiddleBlock;
  ///The ExitBlock of the scalar loop.
  BasicBlock *LoopExitBlock;
  ///The vector loop body.
  BasicBlock *LoopVectorBody;
  ///The scalar loop body.
  BasicBlock *LoopScalarBody;
  ///The first bypass block.
  BasicBlock *LoopBypassBlock;

  /// The new Induction variable which was added to the new block.
  PHINode *Induction;
  /// The induction variable of the old basic block.
  PHINode *OldInduction;
  /// Maps scalars to widened vectors.
  ValueMap WidenMap;
};

/// LoopVectorizationLegality checks if it is legal to vectorize a loop, and
/// to what vectorization factor.
/// This class does not look at the profitability of vectorization, only the
/// legality. This class has two main kinds of checks:
/// * Memory checks - The code in canVectorizeMemory checks if vectorization
///   will change the order of memory accesses in a way that will change the
///   correctness of the program.
/// * Scalars checks - The code in canVectorizeInstrs and canVectorizeMemory
/// checks for a number of different conditions, such as the availability of a
/// single induction variable, that all types are supported and vectorize-able,
/// etc. This code reflects the capabilities of InnerLoopVectorizer.
/// This class is also used by InnerLoopVectorizer for identifying
/// induction variable and the different reduction variables.
class LoopVectorizationLegality {
public:
  LoopVectorizationLegality(Loop *L, ScalarEvolution *SE, DataLayout *DL,
                            DominatorTree *DT)
      : TheLoop(L), SE(SE), DL(DL), DT(DT), Induction(0) {}

  /// This enum represents the kinds of reductions that we support.
  enum ReductionKind {
    RK_NoReduction, ///< Not a reduction.
    RK_IntegerAdd,  ///< Sum of integers.
    RK_IntegerMult, ///< Product of integers.
    RK_IntegerOr,   ///< Bitwise or logical OR of numbers.
    RK_IntegerAnd,  ///< Bitwise or logical AND of numbers.
    RK_IntegerXor,  ///< Bitwise or logical XOR of numbers.
    RK_FloatAdd,    ///< Sum of floats.
    RK_FloatMult    ///< Product of floats.
  };

  /// This enum represents the kinds of inductions that we support.
  enum InductionKind {
    IK_NoInduction,         ///< Not an induction variable.
    IK_IntInduction,        ///< Integer induction variable. Step = 1.
    IK_ReverseIntInduction, ///< Reverse int induction variable. Step = -1.
    IK_PtrInduction         ///< Pointer induction variable. Step = sizeof(elem).
  };

  /// This POD struct holds information about reduction variables.
  struct ReductionDescriptor {
    ReductionDescriptor() : StartValue(0), LoopExitInstr(0),
      Kind(RK_NoReduction) {}

    ReductionDescriptor(Value *Start, Instruction *Exit, ReductionKind K)
        : StartValue(Start), LoopExitInstr(Exit), Kind(K) {}

    // The starting value of the reduction.
    // It does not have to be zero!
    Value *StartValue;
    // The instruction who's value is used outside the loop.
    Instruction *LoopExitInstr;
    // The kind of the reduction.
    ReductionKind Kind;
  };

  // This POD struct holds information about the memory runtime legality
  // check that a group of pointers do not overlap.
  struct RuntimePointerCheck {
    RuntimePointerCheck() : Need(false) {}

    /// Reset the state of the pointer runtime information.
    void reset() {
      Need = false;
      Pointers.clear();
      Starts.clear();
      Ends.clear();
    }

    /// Insert a pointer and calculate the start and end SCEVs.
    void insert(ScalarEvolution *SE, Loop *Lp, Value *Ptr);

    /// This flag indicates if we need to add the runtime check.
    bool Need;
    /// Holds the pointers that we need to check.
    SmallVector<Value*, 2> Pointers;
    /// Holds the pointer value at the beginning of the loop.
    SmallVector<const SCEV*, 2> Starts;
    /// Holds the pointer value at the end of the loop.
    SmallVector<const SCEV*, 2> Ends;
  };

  /// A POD for saving information about induction variables.
  struct InductionInfo {
    InductionInfo(Value *Start, InductionKind K) : StartValue(Start), IK(K) {}
    InductionInfo() : StartValue(0), IK(IK_NoInduction) {}
    /// Start value.
    Value *StartValue;
    /// Induction kind.
    InductionKind IK;
  };

  /// ReductionList contains the reduction descriptors for all
  /// of the reductions that were found in the loop.
  typedef DenseMap<PHINode*, ReductionDescriptor> ReductionList;

  /// InductionList saves induction variables and maps them to the
  /// induction descriptor.
  typedef MapVector<PHINode*, InductionInfo> InductionList;

  /// Returns true if it is legal to vectorize this loop.
  /// This does not mean that it is profitable to vectorize this
  /// loop, only that it is legal to do so.
  bool canVectorize();

  /// Returns the Induction variable.
  PHINode *getInduction() { return Induction; }

  /// Returns the reduction variables found in the loop.
  ReductionList *getReductionVars() { return &Reductions; }

  /// Returns the induction variables found in the loop.
  InductionList *getInductionVars() { return &Inductions; }

  /// Returns True if V is an induction variable in this loop.
  bool isInductionVariable(const Value *V);

  /// Return true if the block BB needs to be predicated in order for the loop
  /// to be vectorized.
  bool blockNeedsPredication(BasicBlock *BB);

  /// Check if this  pointer is consecutive when vectorizing. This happens
  /// when the last index of the GEP is the induction variable, or that the
  /// pointer itself is an induction variable.
  /// This check allows us to vectorize A[idx] into a wide load/store.
  /// Returns:
  /// 0 - Stride is unknown or non consecutive.
  /// 1 - Address is consecutive.
  /// -1 - Address is consecutive, and decreasing.
  int isConsecutivePtr(Value *Ptr);

  /// Returns true if the value V is uniform within the loop.
  bool isUniform(Value *V);

  /// Returns true if this instruction will remain scalar after vectorization.
  bool isUniformAfterVectorization(Instruction* I) { return Uniforms.count(I); }

  /// Returns the information that we collected about runtime memory check.
  RuntimePointerCheck *getRuntimePointerCheck() { return &PtrRtCheck; }
private:
  /// Check if a single basic block loop is vectorizable.
  /// At this point we know that this is a loop with a constant trip count
  /// and we only need to check individual instructions.
  bool canVectorizeInstrs();

  /// When we vectorize loops we may change the order in which
  /// we read and write from memory. This method checks if it is
  /// legal to vectorize the code, considering only memory constrains.
  /// Returns true if the loop is vectorizable
  bool canVectorizeMemory();

  /// Return true if we can vectorize this loop using the IF-conversion
  /// transformation.
  bool canVectorizeWithIfConvert();

  /// Collect the variables that need to stay uniform after vectorization.
  void collectLoopUniforms();

  /// Return true if all of the instructions in the block can be speculatively
  /// executed.
  bool blockCanBePredicated(BasicBlock *BB);

  /// Returns True, if 'Phi' is the kind of reduction variable for type
  /// 'Kind'. If this is a reduction variable, it adds it to ReductionList.
  bool AddReductionVar(PHINode *Phi, ReductionKind Kind);
  /// Returns true if the instruction I can be a reduction variable of type
  /// 'Kind'.
  bool isReductionInstr(Instruction *I, ReductionKind Kind);
  /// Returns the induction kind of Phi. This function may return NoInduction
  /// if the PHI is not an induction variable.
  InductionKind isInductionVariable(PHINode *Phi);
  /// Return true if can compute the address bounds of Ptr within the loop.
  bool hasComputableBounds(Value *Ptr);

  /// The loop that we evaluate.
  Loop *TheLoop;
  /// Scev analysis.
  ScalarEvolution *SE;
  /// DataLayout analysis.
  DataLayout *DL;
  // Dominators.
  DominatorTree *DT;

  //  ---  vectorization state --- //

  /// Holds the integer induction variable. This is the counter of the
  /// loop.
  PHINode *Induction;
  /// Holds the reduction variables.
  ReductionList Reductions;
  /// Holds all of the induction variables that we found in the loop.
  /// Notice that inductions don't need to start at zero and that induction
  /// variables can be pointers.
  InductionList Inductions;

  /// Allowed outside users. This holds the reduction
  /// vars which can be accessed from outside the loop.
  SmallPtrSet<Value*, 4> AllowedExit;
  /// This set holds the variables which are known to be uniform after
  /// vectorization.
  SmallPtrSet<Instruction*, 4> Uniforms;
  /// We need to check that all of the pointers in this list are disjoint
  /// at runtime.
  RuntimePointerCheck PtrRtCheck;
};

/// LoopVectorizationCostModel - estimates the expected speedups due to
/// vectorization.
/// In many cases vectorization is not profitable. This can happen because of
/// a number of reasons. In this class we mainly attempt to predict the
/// expected speedup/slowdowns due to the supported instruction set. We use the
/// TargetTransformInfo to query the different backends for the cost of
/// different operations.
class LoopVectorizationCostModel {
public:
  LoopVectorizationCostModel(Loop *L, ScalarEvolution *SE, LoopInfo *LI,
                             LoopVectorizationLegality *Legal,
                             const TargetTransformInfo &TTI)
      : TheLoop(L), SE(SE), LI(LI), Legal(Legal), TTI(TTI) {}

  /// \return The most profitable vectorization factor.
  /// This method checks every power of two up to VF. If UserVF is not ZERO
  /// then this vectorization factor will be selected if vectorization is
  /// possible.
  unsigned selectVectorizationFactor(bool OptForSize, unsigned UserVF);

  /// \returns The size (in bits) of the widest type in the code that
  /// needs to be vectorized. We ignore values that remain scalar such as
  /// 64 bit loop indices.
  unsigned getWidestType();

  /// \return The most profitable unroll factor.
  /// If UserUF is non-zero then this method finds the best unroll-factor
  /// based on register pressure and other parameters.
  unsigned selectUnrollFactor(bool OptForSize, unsigned UserUF);

  /// \brief A struct that represents some properties of the register usage
  /// of a loop.
  struct RegisterUsage {
    /// Holds the number of loop invariant values that are used in the loop.
    unsigned LoopInvariantRegs;
    /// Holds the maximum number of concurrent live intervals in the loop.
    unsigned MaxLocalUsers;
    /// Holds the number of instructions in the loop.
    unsigned NumInstructions;
  };

  /// \return  information about the register usage of the loop.
  RegisterUsage calculateRegisterUsage();

private:
  /// Returns the expected execution cost. The unit of the cost does
  /// not matter because we use the 'cost' units to compare different
  /// vector widths. The cost that is returned is *not* normalized by
  /// the factor width.
  unsigned expectedCost(unsigned VF);

  /// Returns the execution time cost of an instruction for a given vector
  /// width. Vector width of one means scalar.
  unsigned getInstructionCost(Instruction *I, unsigned VF);

  /// A helper function for converting Scalar types to vector types.
  /// If the incoming type is void, we return void. If the VF is 1, we return
  /// the scalar type.
  static Type* ToVectorTy(Type *Scalar, unsigned VF);

  /// The loop that we evaluate.
  Loop *TheLoop;
  /// Scev analysis.
  ScalarEvolution *SE;
  /// Loop Info analysis.
  LoopInfo *LI;
  /// Vectorization legality.
  LoopVectorizationLegality *Legal;
  /// Vector target information.
  const TargetTransformInfo &TTI;
};

/// The LoopVectorize Pass.
struct LoopVectorize : public LoopPass {
  /// Pass identification, replacement for typeid
  static char ID;

  explicit LoopVectorize() : LoopPass(ID) {
    initializeLoopVectorizePass(*PassRegistry::getPassRegistry());
  }

  ScalarEvolution *SE;
  DataLayout *DL;
  LoopInfo *LI;
  TargetTransformInfo *TTI;
  DominatorTree *DT;

  virtual bool runOnLoop(Loop *L, LPPassManager &LPM) {
    // We only vectorize innermost loops.
    if (!L->empty())
      return false;

    SE = &getAnalysis<ScalarEvolution>();
    DL = getAnalysisIfAvailable<DataLayout>();
    LI = &getAnalysis<LoopInfo>();
    TTI = &getAnalysis<TargetTransformInfo>();
    DT = &getAnalysis<DominatorTree>();

    DEBUG(dbgs() << "LV: Checking a loop in \"" <<
          L->getHeader()->getParent()->getName() << "\"\n");

    // Check if it is legal to vectorize the loop.
    LoopVectorizationLegality LVL(L, SE, DL, DT);
    if (!LVL.canVectorize()) {
      DEBUG(dbgs() << "LV: Not vectorizing.\n");
      return false;
    }

    // Use the cost model.
    LoopVectorizationCostModel CM(L, SE, LI, &LVL, *TTI);

    // Check the function attribues to find out if this function should be
    // optimized for size.
    Function *F = L->getHeader()->getParent();
    Attribute::AttrKind SzAttr = Attribute::OptimizeForSize;
    Attribute::AttrKind FlAttr = Attribute::NoImplicitFloat;
    unsigned FnIndex = AttributeSet::FunctionIndex;
    bool OptForSize = F->getAttributes().hasAttribute(FnIndex, SzAttr);
    bool NoFloat = F->getAttributes().hasAttribute(FnIndex, FlAttr);

    if (NoFloat) {
      DEBUG(dbgs() << "LV: Can't vectorize when the NoImplicitFloat"
            "attribute is used.\n");
      return false;
    }

    unsigned VF = CM.selectVectorizationFactor(OptForSize, VectorizationFactor);
    unsigned UF = CM.selectUnrollFactor(OptForSize, VectorizationUnroll);

    if (VF == 1) {
      DEBUG(dbgs() << "LV: Vectorization is possible but not beneficial.\n");
      return false;
    }

    DEBUG(dbgs() << "LV: Found a vectorizable loop ("<< VF << ") in "<<
          F->getParent()->getModuleIdentifier()<<"\n");
    DEBUG(dbgs() << "LV: Unroll Factor is " << UF << "\n");

    // If we decided that it is *legal* to vectorizer the loop then do it.
    InnerLoopVectorizer LB(L, SE, LI, DT, DL, VF, UF);
    LB.vectorize(&LVL);

    DEBUG(verifyFunction(*L->getHeader()->getParent()));
    return true;
  }

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    LoopPass::getAnalysisUsage(AU);
    AU.addRequiredID(LoopSimplifyID);
    AU.addRequiredID(LCSSAID);
    AU.addRequired<DominatorTree>();
    AU.addRequired<LoopInfo>();
    AU.addRequired<ScalarEvolution>();
    AU.addRequired<TargetTransformInfo>();
    AU.addPreserved<LoopInfo>();
    AU.addPreserved<DominatorTree>();
  }

};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Implementation of LoopVectorizationLegality, InnerLoopVectorizer and
// LoopVectorizationCostModel.
//===----------------------------------------------------------------------===//

void
LoopVectorizationLegality::RuntimePointerCheck::insert(ScalarEvolution *SE,
                                                       Loop *Lp, Value *Ptr) {
  const SCEV *Sc = SE->getSCEV(Ptr);
  const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(Sc);
  assert(AR && "Invalid addrec expression");
  const SCEV *Ex = SE->getExitCount(Lp, Lp->getLoopLatch());
  const SCEV *ScEnd = AR->evaluateAtIteration(Ex, *SE);
  Pointers.push_back(Ptr);
  Starts.push_back(AR->getStart());
  Ends.push_back(ScEnd);
}

Value *InnerLoopVectorizer::getBroadcastInstrs(Value *V) {
  // Save the current insertion location.
  Instruction *Loc = Builder.GetInsertPoint();

  // We need to place the broadcast of invariant variables outside the loop.
  Instruction *Instr = dyn_cast<Instruction>(V);
  bool NewInstr = (Instr && Instr->getParent() == LoopVectorBody);
  bool Invariant = OrigLoop->isLoopInvariant(V) && !NewInstr;

  // Place the code for broadcasting invariant variables in the new preheader.
  if (Invariant)
    Builder.SetInsertPoint(LoopVectorPreHeader->getTerminator());

  // Broadcast the scalar into all locations in the vector.
  Value *Shuf = Builder.CreateVectorSplat(VF, V, "broadcast");

  // Restore the builder insertion point.
  if (Invariant)
    Builder.SetInsertPoint(Loc);

  return Shuf;
}

Value *InnerLoopVectorizer::getConsecutiveVector(Value* Val, unsigned StartIdx,
                                                 bool Negate) {
  assert(Val->getType()->isVectorTy() && "Must be a vector");
  assert(Val->getType()->getScalarType()->isIntegerTy() &&
         "Elem must be an integer");
  // Create the types.
  Type *ITy = Val->getType()->getScalarType();
  VectorType *Ty = cast<VectorType>(Val->getType());
  int VLen = Ty->getNumElements();
  SmallVector<Constant*, 8> Indices;

  // Create a vector of consecutive numbers from zero to VF.
  for (int i = 0; i < VLen; ++i) {
    int Idx = Negate ? (-i): i;
    Indices.push_back(ConstantInt::get(ITy, StartIdx + Idx));
  }

  // Add the consecutive indices to the vector value.
  Constant *Cv = ConstantVector::get(Indices);
  assert(Cv->getType() == Val->getType() && "Invalid consecutive vec");
  return Builder.CreateAdd(Val, Cv, "induction");
}

int LoopVectorizationLegality::isConsecutivePtr(Value *Ptr) {
  assert(Ptr->getType()->isPointerTy() && "Unexpected non ptr");

  // If this value is a pointer induction variable we know it is consecutive.
  PHINode *Phi = dyn_cast_or_null<PHINode>(Ptr);
  if (Phi && Inductions.count(Phi)) {
    InductionInfo II = Inductions[Phi];
    if (IK_PtrInduction == II.IK)
      return 1;
  }

  GetElementPtrInst *Gep = dyn_cast_or_null<GetElementPtrInst>(Ptr);
  if (!Gep)
    return 0;

  unsigned NumOperands = Gep->getNumOperands();
  Value *LastIndex = Gep->getOperand(NumOperands - 1);

  // Check that all of the gep indices are uniform except for the last.
  for (unsigned i = 0; i < NumOperands - 1; ++i)
    if (!SE->isLoopInvariant(SE->getSCEV(Gep->getOperand(i)), TheLoop))
      return 0;

  // We can emit wide load/stores only if the last index is the induction
  // variable.
  const SCEV *Last = SE->getSCEV(LastIndex);
  if (const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(Last)) {
    const SCEV *Step = AR->getStepRecurrence(*SE);

    // The memory is consecutive because the last index is consecutive
    // and all other indices are loop invariant.
    if (Step->isOne())
      return 1;
    if (Step->isAllOnesValue())
      return -1;
  }

  return 0;
}

bool LoopVectorizationLegality::isUniform(Value *V) {
  return (SE->isLoopInvariant(SE->getSCEV(V), TheLoop));
}

InnerLoopVectorizer::VectorParts&
InnerLoopVectorizer::getVectorValue(Value *V) {
  assert(V != Induction && "The new induction variable should not be used.");
  assert(!V->getType()->isVectorTy() && "Can't widen a vector");

  // If we have this scalar in the map, return it.
  if (WidenMap.has(V))
    return WidenMap.get(V);

  // If this scalar is unknown, assume that it is a constant or that it is
  // loop invariant. Broadcast V and save the value for future uses.
  Value *B = getBroadcastInstrs(V);
  WidenMap.splat(V, B);
  return WidenMap.get(V);
}

Value *InnerLoopVectorizer::reverseVector(Value *Vec) {
  assert(Vec->getType()->isVectorTy() && "Invalid type");
  SmallVector<Constant*, 8> ShuffleMask;
  for (unsigned i = 0; i < VF; ++i)
    ShuffleMask.push_back(Builder.getInt32(VF - i - 1));

  return Builder.CreateShuffleVector(Vec, UndefValue::get(Vec->getType()),
                                     ConstantVector::get(ShuffleMask),
                                     "reverse");
}

void InnerLoopVectorizer::scalarizeInstruction(Instruction *Instr) {
  assert(!Instr->getType()->isAggregateType() && "Can't handle vectors");
  // Holds vector parameters or scalars, in case of uniform vals.
  SmallVector<VectorParts, 4> Params;

  // Find all of the vectorized parameters.
  for (unsigned op = 0, e = Instr->getNumOperands(); op != e; ++op) {
    Value *SrcOp = Instr->getOperand(op);

    // If we are accessing the old induction variable, use the new one.
    if (SrcOp == OldInduction) {
      Params.push_back(getVectorValue(SrcOp));
      continue;
    }

    // Try using previously calculated values.
    Instruction *SrcInst = dyn_cast<Instruction>(SrcOp);

    // If the src is an instruction that appeared earlier in the basic block
    // then it should already be vectorized.
    if (SrcInst && OrigLoop->contains(SrcInst)) {
      assert(WidenMap.has(SrcInst) && "Source operand is unavailable");
      // The parameter is a vector value from earlier.
      Params.push_back(WidenMap.get(SrcInst));
    } else {
      // The parameter is a scalar from outside the loop. Maybe even a constant.
      VectorParts Scalars;
      Scalars.append(UF, SrcOp);
      Params.push_back(Scalars);
    }
  }

  assert(Params.size() == Instr->getNumOperands() &&
         "Invalid number of operands");

  // Does this instruction return a value ?
  bool IsVoidRetTy = Instr->getType()->isVoidTy();

  Value *UndefVec = IsVoidRetTy ? 0 :
    UndefValue::get(VectorType::get(Instr->getType(), VF));
  // Create a new entry in the WidenMap and initialize it to Undef or Null.
  VectorParts &VecResults = WidenMap.splat(Instr, UndefVec);

  // For each scalar that we create:
  for (unsigned Width = 0; Width < VF; ++Width) {
    // For each vector unroll 'part':
    for (unsigned Part = 0; Part < UF; ++Part) {
      Instruction *Cloned = Instr->clone();
      if (!IsVoidRetTy)
        Cloned->setName(Instr->getName() + ".cloned");
      // Replace the operands of the cloned instrucions with extracted scalars.
      for (unsigned op = 0, e = Instr->getNumOperands(); op != e; ++op) {
        Value *Op = Params[op][Part];
        // Param is a vector. Need to extract the right lane.
        if (Op->getType()->isVectorTy())
          Op = Builder.CreateExtractElement(Op, Builder.getInt32(Width));
        Cloned->setOperand(op, Op);
      }

      // Place the cloned scalar in the new loop.
      Builder.Insert(Cloned);

      // If the original scalar returns a value we need to place it in a vector
      // so that future users will be able to use it.
      if (!IsVoidRetTy)
        VecResults[Part] = Builder.CreateInsertElement(VecResults[Part], Cloned,
                                                       Builder.getInt32(Width));
    }
  }
}

Value*
InnerLoopVectorizer::addRuntimeCheck(LoopVectorizationLegality *Legal,
                                     Instruction *Loc) {
  LoopVectorizationLegality::RuntimePointerCheck *PtrRtCheck =
  Legal->getRuntimePointerCheck();

  if (!PtrRtCheck->Need)
    return NULL;

  Value *MemoryRuntimeCheck = 0;
  unsigned NumPointers = PtrRtCheck->Pointers.size();
  SmallVector<Value* , 2> Starts;
  SmallVector<Value* , 2> Ends;

  SCEVExpander Exp(*SE, "induction");

  // Use this type for pointer arithmetic.
  Type* PtrArithTy = Type::getInt8PtrTy(Loc->getContext(), 0);

  for (unsigned i = 0; i < NumPointers; ++i) {
    Value *Ptr = PtrRtCheck->Pointers[i];
    const SCEV *Sc = SE->getSCEV(Ptr);

    if (SE->isLoopInvariant(Sc, OrigLoop)) {
      DEBUG(dbgs() << "LV: Adding RT check for a loop invariant ptr:" <<
            *Ptr <<"\n");
      Starts.push_back(Ptr);
      Ends.push_back(Ptr);
    } else {
      DEBUG(dbgs() << "LV: Adding RT check for range:" << *Ptr <<"\n");

      Value *Start = Exp.expandCodeFor(PtrRtCheck->Starts[i], PtrArithTy, Loc);
      Value *End = Exp.expandCodeFor(PtrRtCheck->Ends[i], PtrArithTy, Loc);
      Starts.push_back(Start);
      Ends.push_back(End);
    }
  }

  for (unsigned i = 0; i < NumPointers; ++i) {
    for (unsigned j = i+1; j < NumPointers; ++j) {
      Instruction::CastOps Op = Instruction::BitCast;
      Value *Start0 = CastInst::Create(Op, Starts[i], PtrArithTy, "bc", Loc);
      Value *Start1 = CastInst::Create(Op, Starts[j], PtrArithTy, "bc", Loc);
      Value *End0 =   CastInst::Create(Op, Ends[i],   PtrArithTy, "bc", Loc);
      Value *End1 =   CastInst::Create(Op, Ends[j],   PtrArithTy, "bc", Loc);

      Value *Cmp0 = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_ULE,
                                    Start0, End1, "bound0", Loc);
      Value *Cmp1 = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_ULE,
                                    Start1, End0, "bound1", Loc);
      Value *IsConflict = BinaryOperator::Create(Instruction::And, Cmp0, Cmp1,
                                                 "found.conflict", Loc);
      if (MemoryRuntimeCheck)
        MemoryRuntimeCheck = BinaryOperator::Create(Instruction::Or,
                                                    MemoryRuntimeCheck,
                                                    IsConflict,
                                                    "conflict.rdx", Loc);
      else
        MemoryRuntimeCheck = IsConflict;

    }
  }

  return MemoryRuntimeCheck;
}

void
InnerLoopVectorizer::createEmptyLoop(LoopVectorizationLegality *Legal) {
  /*
   In this function we generate a new loop. The new loop will contain
   the vectorized instructions while the old loop will continue to run the
   scalar remainder.

       [ ] <-- vector loop bypass.
     /  |
    /   v
   |   [ ]     <-- vector pre header.
   |    |
   |    v
   |   [  ] \
   |   [  ]_|   <-- vector loop.
   |    |
    \   v
      >[ ]   <--- middle-block.
     /  |
    /   v
   |   [ ]     <--- new preheader.
   |    |
   |    v
   |   [ ] \
   |   [ ]_|   <-- old scalar loop to handle remainder.
    \   |
     \  v
      >[ ]     <-- exit block.
   ...
   */

  BasicBlock *OldBasicBlock = OrigLoop->getHeader();
  BasicBlock *BypassBlock = OrigLoop->getLoopPreheader();
  BasicBlock *ExitBlock = OrigLoop->getExitBlock();
  assert(ExitBlock && "Must have an exit block");

  // Some loops have a single integer induction variable, while other loops
  // don't. One example is c++ iterators that often have multiple pointer
  // induction variables. In the code below we also support a case where we
  // don't have a single induction variable.
  OldInduction = Legal->getInduction();
  Type *IdxTy = OldInduction ? OldInduction->getType() :
  DL->getIntPtrType(SE->getContext());

  // Find the loop boundaries.
  const SCEV *ExitCount = SE->getExitCount(OrigLoop, OrigLoop->getLoopLatch());
  assert(ExitCount != SE->getCouldNotCompute() && "Invalid loop count");

  // Get the total trip count from the count by adding 1.
  ExitCount = SE->getAddExpr(ExitCount,
                             SE->getConstant(ExitCount->getType(), 1));

  // Expand the trip count and place the new instructions in the preheader.
  // Notice that the pre-header does not change, only the loop body.
  SCEVExpander Exp(*SE, "induction");

  // Count holds the overall loop count (N).
  Value *Count = Exp.expandCodeFor(ExitCount, ExitCount->getType(),
                                   BypassBlock->getTerminator());

  // The loop index does not have to start at Zero. Find the original start
  // value from the induction PHI node. If we don't have an induction variable
  // then we know that it starts at zero.
  Value *StartIdx = OldInduction ?
  OldInduction->getIncomingValueForBlock(BypassBlock):
  ConstantInt::get(IdxTy, 0);

  assert(BypassBlock && "Invalid loop structure");

  // Generate the code that checks in runtime if arrays overlap.
  Value *MemoryRuntimeCheck = addRuntimeCheck(Legal,
                                              BypassBlock->getTerminator());

  // Split the single block loop into the two loop structure described above.
  BasicBlock *VectorPH =
  BypassBlock->splitBasicBlock(BypassBlock->getTerminator(), "vector.ph");
  BasicBlock *VecBody =
  VectorPH->splitBasicBlock(VectorPH->getTerminator(), "vector.body");
  BasicBlock *MiddleBlock =
  VecBody->splitBasicBlock(VecBody->getTerminator(), "middle.block");
  BasicBlock *ScalarPH =
  MiddleBlock->splitBasicBlock(MiddleBlock->getTerminator(), "scalar.ph");

  // This is the location in which we add all of the logic for bypassing
  // the new vector loop.
  Instruction *Loc = BypassBlock->getTerminator();

  // Use this IR builder to create the loop instructions (Phi, Br, Cmp)
  // inside the loop.
  Builder.SetInsertPoint(VecBody->getFirstInsertionPt());

  // Generate the induction variable.
  Induction = Builder.CreatePHI(IdxTy, 2, "index");
  // The loop step is equal to the vectorization factor (num of SIMD elements)
  // times the unroll factor (num of SIMD instructions).
  Constant *Step = ConstantInt::get(IdxTy, VF * UF);

  // We may need to extend the index in case there is a type mismatch.
  // We know that the count starts at zero and does not overflow.
  unsigned IdxTyBW = IdxTy->getScalarSizeInBits();
  if (Count->getType() != IdxTy) {
    // The exit count can be of pointer type. Convert it to the correct
    // integer type.
    if (ExitCount->getType()->isPointerTy())
      Count = CastInst::CreatePointerCast(Count, IdxTy, "ptrcnt.to.int", Loc);
    else if (IdxTyBW < Count->getType()->getScalarSizeInBits())
      Count = CastInst::CreateTruncOrBitCast(Count, IdxTy, "tr.cnt", Loc);
    else
      Count = CastInst::CreateZExtOrBitCast(Count, IdxTy, "zext.cnt", Loc);
  }

  // Add the start index to the loop count to get the new end index.
  Value *IdxEnd = BinaryOperator::CreateAdd(Count, StartIdx, "end.idx", Loc);

  // Now we need to generate the expression for N - (N % VF), which is
  // the part that the vectorized body will execute.
  Value *R = BinaryOperator::CreateURem(Count, Step, "n.mod.vf", Loc);
  Value *CountRoundDown = BinaryOperator::CreateSub(Count, R, "n.vec", Loc);
  Value *IdxEndRoundDown = BinaryOperator::CreateAdd(CountRoundDown, StartIdx,
                                                     "end.idx.rnd.down", Loc);

  // Now, compare the new count to zero. If it is zero skip the vector loop and
  // jump to the scalar loop.
  Value *Cmp = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_EQ,
                               IdxEndRoundDown,
                               StartIdx,
                               "cmp.zero", Loc);

  // If we are using memory runtime checks, include them in.
  if (MemoryRuntimeCheck)
    Cmp = BinaryOperator::Create(Instruction::Or, Cmp, MemoryRuntimeCheck,
                                 "CntOrMem", Loc);

  BranchInst::Create(MiddleBlock, VectorPH, Cmp, Loc);
  // Remove the old terminator.
  Loc->eraseFromParent();

  // We are going to resume the execution of the scalar loop.
  // Go over all of the induction variables that we found and fix the
  // PHIs that are left in the scalar version of the loop.
  // The starting values of PHI nodes depend on the counter of the last
  // iteration in the vectorized loop.
  // If we come from a bypass edge then we need to start from the original
  // start value.

  // This variable saves the new starting index for the scalar loop.
  PHINode *ResumeIndex = 0;
  LoopVectorizationLegality::InductionList::iterator I, E;
  LoopVectorizationLegality::InductionList *List = Legal->getInductionVars();
  for (I = List->begin(), E = List->end(); I != E; ++I) {
    PHINode *OrigPhi = I->first;
    LoopVectorizationLegality::InductionInfo II = I->second;
    PHINode *ResumeVal = PHINode::Create(OrigPhi->getType(), 2, "resume.val",
                                         MiddleBlock->getTerminator());
    Value *EndValue = 0;
    switch (II.IK) {
    case LoopVectorizationLegality::IK_NoInduction:
      llvm_unreachable("Unknown induction");
    case LoopVectorizationLegality::IK_IntInduction: {
      // Handle the integer induction counter:
      assert(OrigPhi->getType()->isIntegerTy() && "Invalid type");
      assert(OrigPhi == OldInduction && "Unknown integer PHI");
      // We know what the end value is.
      EndValue = IdxEndRoundDown;
      // We also know which PHI node holds it.
      ResumeIndex = ResumeVal;
      break;
    }
    case LoopVectorizationLegality::IK_ReverseIntInduction: {
      // Convert the CountRoundDown variable to the PHI size.
      unsigned CRDSize = CountRoundDown->getType()->getScalarSizeInBits();
      unsigned IISize = II.StartValue->getType()->getScalarSizeInBits();
      Value *CRD = CountRoundDown;
      if (CRDSize > IISize)
        CRD = CastInst::Create(Instruction::Trunc, CountRoundDown,
                               II.StartValue->getType(),
                               "tr.crd", BypassBlock->getTerminator());
      else if (CRDSize < IISize)
        CRD = CastInst::Create(Instruction::SExt, CountRoundDown,
                               II.StartValue->getType(),
                               "sext.crd", BypassBlock->getTerminator());
      // Handle reverse integer induction counter:
      EndValue = BinaryOperator::CreateSub(II.StartValue, CRD, "rev.ind.end",
                                           BypassBlock->getTerminator());
      break;
    }
    case LoopVectorizationLegality::IK_PtrInduction: {
      // For pointer induction variables, calculate the offset using
      // the end index.
      EndValue = GetElementPtrInst::Create(II.StartValue, CountRoundDown,
                                           "ptr.ind.end",
                                           BypassBlock->getTerminator());
      break;
    }
    }// end of case

    // The new PHI merges the original incoming value, in case of a bypass,
    // or the value at the end of the vectorized loop.
    ResumeVal->addIncoming(II.StartValue, BypassBlock);
    ResumeVal->addIncoming(EndValue, VecBody);

    // Fix the scalar body counter (PHI node).
    unsigned BlockIdx = OrigPhi->getBasicBlockIndex(ScalarPH);
    OrigPhi->setIncomingValue(BlockIdx, ResumeVal);
  }

  // If we are generating a new induction variable then we also need to
  // generate the code that calculates the exit value. This value is not
  // simply the end of the counter because we may skip the vectorized body
  // in case of a runtime check.
  if (!OldInduction){
    assert(!ResumeIndex && "Unexpected resume value found");
    ResumeIndex = PHINode::Create(IdxTy, 2, "new.indc.resume.val",
                                  MiddleBlock->getTerminator());
    ResumeIndex->addIncoming(StartIdx, BypassBlock);
    ResumeIndex->addIncoming(IdxEndRoundDown, VecBody);
  }

  // Make sure that we found the index where scalar loop needs to continue.
  assert(ResumeIndex && ResumeIndex->getType()->isIntegerTy() &&
         "Invalid resume Index");

  // Add a check in the middle block to see if we have completed
  // all of the iterations in the first vector loop.
  // If (N - N%VF) == N, then we *don't* need to run the remainder.
  Value *CmpN = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_EQ, IdxEnd,
                                ResumeIndex, "cmp.n",
                                MiddleBlock->getTerminator());

  BranchInst::Create(ExitBlock, ScalarPH, CmpN, MiddleBlock->getTerminator());
  // Remove the old terminator.
  MiddleBlock->getTerminator()->eraseFromParent();

  // Create i+1 and fill the PHINode.
  Value *NextIdx = Builder.CreateAdd(Induction, Step, "index.next");
  Induction->addIncoming(StartIdx, VectorPH);
  Induction->addIncoming(NextIdx, VecBody);
  // Create the compare.
  Value *ICmp = Builder.CreateICmpEQ(NextIdx, IdxEndRoundDown);
  Builder.CreateCondBr(ICmp, MiddleBlock, VecBody);

  // Now we have two terminators. Remove the old one from the block.
  VecBody->getTerminator()->eraseFromParent();

  // Get ready to start creating new instructions into the vectorized body.
  Builder.SetInsertPoint(VecBody->getFirstInsertionPt());

  // Create and register the new vector loop.
  Loop* Lp = new Loop();
  Loop *ParentLoop = OrigLoop->getParentLoop();

  // Insert the new loop into the loop nest and register the new basic blocks.
  if (ParentLoop) {
    ParentLoop->addChildLoop(Lp);
    ParentLoop->addBasicBlockToLoop(ScalarPH, LI->getBase());
    ParentLoop->addBasicBlockToLoop(VectorPH, LI->getBase());
    ParentLoop->addBasicBlockToLoop(MiddleBlock, LI->getBase());
  } else {
    LI->addTopLevelLoop(Lp);
  }

  Lp->addBasicBlockToLoop(VecBody, LI->getBase());

  // Save the state.
  LoopVectorPreHeader = VectorPH;
  LoopScalarPreHeader = ScalarPH;
  LoopMiddleBlock = MiddleBlock;
  LoopExitBlock = ExitBlock;
  LoopVectorBody = VecBody;
  LoopScalarBody = OldBasicBlock;
  LoopBypassBlock = BypassBlock;
}

/// This function returns the identity element (or neutral element) for
/// the operation K.
static Constant*
getReductionIdentity(LoopVectorizationLegality::ReductionKind K, Type *Tp) {
  switch (K) {
  case LoopVectorizationLegality:: RK_IntegerXor:
  case LoopVectorizationLegality:: RK_IntegerAdd:
  case LoopVectorizationLegality:: RK_IntegerOr:
    // Adding, Xoring, Oring zero to a number does not change it.
    return ConstantInt::get(Tp, 0);
  case LoopVectorizationLegality:: RK_IntegerMult:
    // Multiplying a number by 1 does not change it.
    return ConstantInt::get(Tp, 1);
  case LoopVectorizationLegality:: RK_IntegerAnd:
    // AND-ing a number with an all-1 value does not change it.
    return ConstantInt::get(Tp, -1, true);
  case LoopVectorizationLegality:: RK_FloatMult:
    // Multiplying a number by 1 does not change it.
    return ConstantFP::get(Tp, 1.0L);
  case LoopVectorizationLegality:: RK_FloatAdd:
    // Adding zero to a number does not change it.
    return ConstantFP::get(Tp, 0.0L);
  default:
    llvm_unreachable("Unknown reduction kind");
  }
}

static bool
isTriviallyVectorizableIntrinsic(Instruction *Inst) {
  IntrinsicInst *II = dyn_cast<IntrinsicInst>(Inst);
  if (!II)
    return false;
  switch (II->getIntrinsicID()) {
  case Intrinsic::sqrt:
  case Intrinsic::sin:
  case Intrinsic::cos:
  case Intrinsic::exp:
  case Intrinsic::exp2:
  case Intrinsic::log:
  case Intrinsic::log10:
  case Intrinsic::log2:
  case Intrinsic::fabs:
  case Intrinsic::floor:
  case Intrinsic::ceil:
  case Intrinsic::trunc:
  case Intrinsic::rint:
  case Intrinsic::nearbyint:
  case Intrinsic::pow:
  case Intrinsic::fma:
  case Intrinsic::fmuladd:
    return true;
  default:
    return false;
  }
  return false;
}

/// This function translates the reduction kind to an LLVM binary operator.
static Instruction::BinaryOps
getReductionBinOp(LoopVectorizationLegality::ReductionKind Kind) {
  switch (Kind) {
    case LoopVectorizationLegality::RK_IntegerAdd:
      return Instruction::Add;
    case LoopVectorizationLegality::RK_IntegerMult:
      return Instruction::Mul;
    case LoopVectorizationLegality::RK_IntegerOr:
      return Instruction::Or;
    case LoopVectorizationLegality::RK_IntegerAnd:
      return Instruction::And;
    case LoopVectorizationLegality::RK_IntegerXor:
      return Instruction::Xor;
    case LoopVectorizationLegality::RK_FloatMult:
      return Instruction::FMul;
    case LoopVectorizationLegality::RK_FloatAdd:
      return Instruction::FAdd;
    default:
      llvm_unreachable("Unknown reduction operation");
  }
}

void
InnerLoopVectorizer::vectorizeLoop(LoopVectorizationLegality *Legal) {
  //===------------------------------------------------===//
  //
  // Notice: any optimization or new instruction that go
  // into the code below should be also be implemented in
  // the cost-model.
  //
  //===------------------------------------------------===//
  BasicBlock &BB = *OrigLoop->getHeader();
  Constant *Zero =
  ConstantInt::get(IntegerType::getInt32Ty(BB.getContext()), 0);

  // In order to support reduction variables we need to be able to vectorize
  // Phi nodes. Phi nodes have cycles, so we need to vectorize them in two
  // stages. First, we create a new vector PHI node with no incoming edges.
  // We use this value when we vectorize all of the instructions that use the
  // PHI. Next, after all of the instructions in the block are complete we
  // add the new incoming edges to the PHI. At this point all of the
  // instructions in the basic block are vectorized, so we can use them to
  // construct the PHI.
  PhiVector RdxPHIsToFix;

  // Scan the loop in a topological order to ensure that defs are vectorized
  // before users.
  LoopBlocksDFS DFS(OrigLoop);
  DFS.perform(LI);

  // Vectorize all of the blocks in the original loop.
  for (LoopBlocksDFS::RPOIterator bb = DFS.beginRPO(),
       be = DFS.endRPO(); bb != be; ++bb)
    vectorizeBlockInLoop(Legal, *bb, &RdxPHIsToFix);

  // At this point every instruction in the original loop is widened to
  // a vector form. We are almost done. Now, we need to fix the PHI nodes
  // that we vectorized. The PHI nodes are currently empty because we did
  // not want to introduce cycles. Notice that the remaining PHI nodes
  // that we need to fix are reduction variables.

  // Create the 'reduced' values for each of the induction vars.
  // The reduced values are the vector values that we scalarize and combine
  // after the loop is finished.
  for (PhiVector::iterator it = RdxPHIsToFix.begin(), e = RdxPHIsToFix.end();
       it != e; ++it) {
    PHINode *RdxPhi = *it;
    assert(RdxPhi && "Unable to recover vectorized PHI");

    // Find the reduction variable descriptor.
    assert(Legal->getReductionVars()->count(RdxPhi) &&
           "Unable to find the reduction variable");
    LoopVectorizationLegality::ReductionDescriptor RdxDesc =
    (*Legal->getReductionVars())[RdxPhi];

    // We need to generate a reduction vector from the incoming scalar.
    // To do so, we need to generate the 'identity' vector and overide
    // one of the elements with the incoming scalar reduction. We need
    // to do it in the vector-loop preheader.
    Builder.SetInsertPoint(LoopBypassBlock->getTerminator());

    // This is the vector-clone of the value that leaves the loop.
    VectorParts &VectorExit = getVectorValue(RdxDesc.LoopExitInstr);
    Type *VecTy = VectorExit[0]->getType();

    // Find the reduction identity variable. Zero for addition, or, xor,
    // one for multiplication, -1 for And.
    Constant *Iden = getReductionIdentity(RdxDesc.Kind, VecTy->getScalarType());
    Constant *Identity = ConstantVector::getSplat(VF, Iden);

    // This vector is the Identity vector where the first element is the
    // incoming scalar reduction.
    Value *VectorStart = Builder.CreateInsertElement(Identity,
                                                     RdxDesc.StartValue, Zero);

    // Fix the vector-loop phi.
    // We created the induction variable so we know that the
    // preheader is the first entry.
    BasicBlock *VecPreheader = Induction->getIncomingBlock(0);

    // Reductions do not have to start at zero. They can start with
    // any loop invariant values.
    VectorParts &VecRdxPhi = WidenMap.get(RdxPhi);
    BasicBlock *Latch = OrigLoop->getLoopLatch();
    Value *LoopVal = RdxPhi->getIncomingValueForBlock(Latch);
    VectorParts &Val = getVectorValue(LoopVal);
    for (unsigned part = 0; part < UF; ++part) {
      // Make sure to add the reduction stat value only to the 
      // first unroll part.
      Value *StartVal = (part == 0) ? VectorStart : Identity;
      cast<PHINode>(VecRdxPhi[part])->addIncoming(StartVal, VecPreheader);
      cast<PHINode>(VecRdxPhi[part])->addIncoming(Val[part], LoopVectorBody);
    }

    // Before each round, move the insertion point right between
    // the PHIs and the values we are going to write.
    // This allows us to write both PHINodes and the extractelement
    // instructions.
    Builder.SetInsertPoint(LoopMiddleBlock->getFirstInsertionPt());

    VectorParts RdxParts;
    for (unsigned part = 0; part < UF; ++part) {
      // This PHINode contains the vectorized reduction variable, or
      // the initial value vector, if we bypass the vector loop.
      VectorParts &RdxExitVal = getVectorValue(RdxDesc.LoopExitInstr);
      PHINode *NewPhi = Builder.CreatePHI(VecTy, 2, "rdx.vec.exit.phi");
      Value *StartVal = (part == 0) ? VectorStart : Identity;
      NewPhi->addIncoming(StartVal, LoopBypassBlock);
      NewPhi->addIncoming(RdxExitVal[part], LoopVectorBody);
      RdxParts.push_back(NewPhi);
    }

    // Reduce all of the unrolled parts into a single vector.
    Value *ReducedPartRdx = RdxParts[0];
    for (unsigned part = 1; part < UF; ++part) {
      Instruction::BinaryOps Op = getReductionBinOp(RdxDesc.Kind);
      ReducedPartRdx = Builder.CreateBinOp(Op, RdxParts[part], ReducedPartRdx,
                                           "bin.rdx");
    }

    // VF is a power of 2 so we can emit the reduction using log2(VF) shuffles
    // and vector ops, reducing the set of values being computed by half each
    // round.
    assert(isPowerOf2_32(VF) &&
           "Reduction emission only supported for pow2 vectors!");
    Value *TmpVec = ReducedPartRdx;
    SmallVector<Constant*, 32> ShuffleMask(VF, 0);
    for (unsigned i = VF; i != 1; i >>= 1) {
      // Move the upper half of the vector to the lower half.
      for (unsigned j = 0; j != i/2; ++j)
        ShuffleMask[j] = Builder.getInt32(i/2 + j);

      // Fill the rest of the mask with undef.
      std::fill(&ShuffleMask[i/2], ShuffleMask.end(),
                UndefValue::get(Builder.getInt32Ty()));

      Value *Shuf =
        Builder.CreateShuffleVector(TmpVec,
                                    UndefValue::get(TmpVec->getType()),
                                    ConstantVector::get(ShuffleMask),
                                    "rdx.shuf");

      Instruction::BinaryOps Op = getReductionBinOp(RdxDesc.Kind);
      TmpVec = Builder.CreateBinOp(Op, TmpVec, Shuf, "bin.rdx");
    }

    // The result is in the first element of the vector.
    Value *Scalar0 = Builder.CreateExtractElement(TmpVec, Builder.getInt32(0));

    // Now, we need to fix the users of the reduction variable
    // inside and outside of the scalar remainder loop.
    // We know that the loop is in LCSSA form. We need to update the
    // PHI nodes in the exit blocks.
    for (BasicBlock::iterator LEI = LoopExitBlock->begin(),
         LEE = LoopExitBlock->end(); LEI != LEE; ++LEI) {
      PHINode *LCSSAPhi = dyn_cast<PHINode>(LEI);
      if (!LCSSAPhi) continue;

      // All PHINodes need to have a single entry edge, or two if
      // we already fixed them.
      assert(LCSSAPhi->getNumIncomingValues() < 3 && "Invalid LCSSA PHI");

      // We found our reduction value exit-PHI. Update it with the
      // incoming bypass edge.
      if (LCSSAPhi->getIncomingValue(0) == RdxDesc.LoopExitInstr) {
        // Add an edge coming from the bypass.
        LCSSAPhi->addIncoming(Scalar0, LoopMiddleBlock);
        break;
      }
    }// end of the LCSSA phi scan.

    // Fix the scalar loop reduction variable with the incoming reduction sum
    // from the vector body and from the backedge value.
    int IncomingEdgeBlockIdx =
    (RdxPhi)->getBasicBlockIndex(OrigLoop->getLoopLatch());
    assert(IncomingEdgeBlockIdx >= 0 && "Invalid block index");
    // Pick the other block.
    int SelfEdgeBlockIdx = (IncomingEdgeBlockIdx ? 0 : 1);
    (RdxPhi)->setIncomingValue(SelfEdgeBlockIdx, Scalar0);
    (RdxPhi)->setIncomingValue(IncomingEdgeBlockIdx, RdxDesc.LoopExitInstr);
  }// end of for each redux variable.

  // The Loop exit block may have single value PHI nodes where the incoming
  // value is 'undef'. While vectorizing we only handled real values that
  // were defined inside the loop. Here we handle the 'undef case'.
  // See PR14725.
  for (BasicBlock::iterator LEI = LoopExitBlock->begin(),
       LEE = LoopExitBlock->end(); LEI != LEE; ++LEI) {
    PHINode *LCSSAPhi = dyn_cast<PHINode>(LEI);
    if (!LCSSAPhi) continue;
    if (LCSSAPhi->getNumIncomingValues() == 1)
      LCSSAPhi->addIncoming(UndefValue::get(LCSSAPhi->getType()),
                            LoopMiddleBlock);
  }
}

InnerLoopVectorizer::VectorParts
InnerLoopVectorizer::createEdgeMask(BasicBlock *Src, BasicBlock *Dst) {
  assert(std::find(pred_begin(Dst), pred_end(Dst), Src) != pred_end(Dst) &&
         "Invalid edge");

  VectorParts SrcMask = createBlockInMask(Src);

  // The terminator has to be a branch inst!
  BranchInst *BI = dyn_cast<BranchInst>(Src->getTerminator());
  assert(BI && "Unexpected terminator found");

  if (BI->isConditional()) {
    VectorParts EdgeMask = getVectorValue(BI->getCondition());

    if (BI->getSuccessor(0) != Dst)
      for (unsigned part = 0; part < UF; ++part)
        EdgeMask[part] = Builder.CreateNot(EdgeMask[part]);

    for (unsigned part = 0; part < UF; ++part)
      EdgeMask[part] = Builder.CreateAnd(EdgeMask[part], SrcMask[part]);
    return EdgeMask;
  }

  return SrcMask;
}

InnerLoopVectorizer::VectorParts
InnerLoopVectorizer::createBlockInMask(BasicBlock *BB) {
  assert(OrigLoop->contains(BB) && "Block is not a part of a loop");

  // Loop incoming mask is all-one.
  if (OrigLoop->getHeader() == BB) {
    Value *C = ConstantInt::get(IntegerType::getInt1Ty(BB->getContext()), 1);
    return getVectorValue(C);
  }

  // This is the block mask. We OR all incoming edges, and with zero.
  Value *Zero = ConstantInt::get(IntegerType::getInt1Ty(BB->getContext()), 0);
  VectorParts BlockMask = getVectorValue(Zero);

  // For each pred:
  for (pred_iterator it = pred_begin(BB), e = pred_end(BB); it != e; ++it) {
    VectorParts EM = createEdgeMask(*it, BB);
    for (unsigned part = 0; part < UF; ++part)
      BlockMask[part] = Builder.CreateOr(BlockMask[part], EM[part]);
  }

  return BlockMask;
}

void
InnerLoopVectorizer::vectorizeBlockInLoop(LoopVectorizationLegality *Legal,
                                          BasicBlock *BB, PhiVector *PV) {
  Constant *Zero = Builder.getInt32(0);

  // For each instruction in the old loop.
  for (BasicBlock::iterator it = BB->begin(), e = BB->end(); it != e; ++it) {
    VectorParts &Entry = WidenMap.get(it);
    switch (it->getOpcode()) {
    case Instruction::Br:
      // Nothing to do for PHIs and BR, since we already took care of the
      // loop control flow instructions.
      continue;
    case Instruction::PHI:{
      PHINode* P = cast<PHINode>(it);
      // Handle reduction variables:
      if (Legal->getReductionVars()->count(P)) {
        for (unsigned part = 0; part < UF; ++part) {
          // This is phase one of vectorizing PHIs.
          Type *VecTy = VectorType::get(it->getType(), VF);
          Entry[part] = PHINode::Create(VecTy, 2, "vec.phi",
                                        LoopVectorBody-> getFirstInsertionPt());
        }
        PV->push_back(P);
        continue;
      }

      // Check for PHI nodes that are lowered to vector selects.
      if (P->getParent() != OrigLoop->getHeader()) {
        // We know that all PHIs in non header blocks are converted into
        // selects, so we don't have to worry about the insertion order and we
        // can just use the builder.

        // At this point we generate the predication tree. There may be
        // duplications since this is a simple recursive scan, but future
        // optimizations will clean it up.
        VectorParts Cond = createEdgeMask(P->getIncomingBlock(0),
                                               P->getParent());
        
        for (unsigned part = 0; part < UF; ++part) {
        VectorParts &In0 = getVectorValue(P->getIncomingValue(0));
        VectorParts &In1 = getVectorValue(P->getIncomingValue(1));
          Entry[part] = Builder.CreateSelect(Cond[part], In0[part], In1[part],
                                             "predphi");
        }
        continue;
      }

      // This PHINode must be an induction variable.
      // Make sure that we know about it.
      assert(Legal->getInductionVars()->count(P) &&
             "Not an induction variable");

      LoopVectorizationLegality::InductionInfo II =
        Legal->getInductionVars()->lookup(P);

      switch (II.IK) {
      case LoopVectorizationLegality::IK_NoInduction:
        llvm_unreachable("Unknown induction");
      case LoopVectorizationLegality::IK_IntInduction: {
        assert(P == OldInduction && "Unexpected PHI");
        Value *Broadcasted = getBroadcastInstrs(Induction);
        // After broadcasting the induction variable we need to make the
        // vector consecutive by adding 0, 1, 2 ...
        for (unsigned part = 0; part < UF; ++part)
          Entry[part] = getConsecutiveVector(Broadcasted, VF * part, false);
        continue;
      }
      case LoopVectorizationLegality::IK_ReverseIntInduction:
      case LoopVectorizationLegality::IK_PtrInduction:
        // Handle reverse integer and pointer inductions.
        Value *StartIdx = 0;
        // If we have a single integer induction variable then use it.
        // Otherwise, start counting at zero.
        if (OldInduction) {
          LoopVectorizationLegality::InductionInfo OldII =
            Legal->getInductionVars()->lookup(OldInduction);
          StartIdx = OldII.StartValue;
        } else {
          StartIdx = ConstantInt::get(Induction->getType(), 0);
        }
        // This is the normalized GEP that starts counting at zero.
        Value *NormalizedIdx = Builder.CreateSub(Induction, StartIdx,
                                                 "normalized.idx");

        // Handle the reverse integer induction variable case.
        if (LoopVectorizationLegality::IK_ReverseIntInduction == II.IK) {
          IntegerType *DstTy = cast<IntegerType>(II.StartValue->getType());
          Value *CNI = Builder.CreateSExtOrTrunc(NormalizedIdx, DstTy,
                                                 "resize.norm.idx");
          Value *ReverseInd  = Builder.CreateSub(II.StartValue, CNI,
                                                 "reverse.idx");

          // This is a new value so do not hoist it out.
          Value *Broadcasted = getBroadcastInstrs(ReverseInd);
          // After broadcasting the induction variable we need to make the
          // vector consecutive by adding  ... -3, -2, -1, 0.
          for (unsigned part = 0; part < UF; ++part)
            Entry[part] = getConsecutiveVector(Broadcasted, -VF * part, true);
          continue;
        }

        // Handle the pointer induction variable case.
        assert(P->getType()->isPointerTy() && "Unexpected type.");

        // This is the vector of results. Notice that we don't generate
        // vector geps because scalar geps result in better code.
        for (unsigned part = 0; part < UF; ++part) {
          Value *VecVal = UndefValue::get(VectorType::get(P->getType(), VF));
          for (unsigned int i = 0; i < VF; ++i) {
            Constant *Idx = ConstantInt::get(Induction->getType(),
                                             i + part * VF);
            Value *GlobalIdx = Builder.CreateAdd(NormalizedIdx, Idx,
                                                 "gep.idx");
            Value *SclrGep = Builder.CreateGEP(II.StartValue, GlobalIdx,
                                               "next.gep");
            VecVal = Builder.CreateInsertElement(VecVal, SclrGep,
                                                 Builder.getInt32(i),
                                                 "insert.gep");
          }
          Entry[part] = VecVal;
        }
        continue;
      }

    }// End of PHI.

    case Instruction::Add:
    case Instruction::FAdd:
    case Instruction::Sub:
    case Instruction::FSub:
    case Instruction::Mul:
    case Instruction::FMul:
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::FDiv:
    case Instruction::URem:
    case Instruction::SRem:
    case Instruction::FRem:
    case Instruction::Shl:
    case Instruction::LShr:
    case Instruction::AShr:
    case Instruction::And:
    case Instruction::Or:
    case Instruction::Xor: {
      // Just widen binops.
      BinaryOperator *BinOp = dyn_cast<BinaryOperator>(it);
      VectorParts &A = getVectorValue(it->getOperand(0));
      VectorParts &B = getVectorValue(it->getOperand(1));

      // Use this vector value for all users of the original instruction.
      for (unsigned Part = 0; Part < UF; ++Part) {
        Value *V = Builder.CreateBinOp(BinOp->getOpcode(), A[Part], B[Part]);

        // Update the NSW, NUW and Exact flags. Notice: V can be an Undef.
        BinaryOperator *VecOp = dyn_cast<BinaryOperator>(V);
        if (VecOp && isa<OverflowingBinaryOperator>(BinOp)) {
          VecOp->setHasNoSignedWrap(BinOp->hasNoSignedWrap());
          VecOp->setHasNoUnsignedWrap(BinOp->hasNoUnsignedWrap());
        }
        if (VecOp && isa<PossiblyExactOperator>(VecOp))
          VecOp->setIsExact(BinOp->isExact());

        Entry[Part] = V;
      }
      break;
    }
    case Instruction::Select: {
      // Widen selects.
      // If the selector is loop invariant we can create a select
      // instruction with a scalar condition. Otherwise, use vector-select.
      bool InvariantCond = SE->isLoopInvariant(SE->getSCEV(it->getOperand(0)),
                                               OrigLoop);

      // The condition can be loop invariant  but still defined inside the
      // loop. This means that we can't just use the original 'cond' value.
      // We have to take the 'vectorized' value and pick the first lane.
      // Instcombine will make this a no-op.
      VectorParts &Cond = getVectorValue(it->getOperand(0));
      VectorParts &Op0  = getVectorValue(it->getOperand(1));
      VectorParts &Op1  = getVectorValue(it->getOperand(2));
      Value *ScalarCond = Builder.CreateExtractElement(Cond[0],
                                                       Builder.getInt32(0));
      for (unsigned Part = 0; Part < UF; ++Part) {
        Entry[Part] = Builder.CreateSelect(
          InvariantCond ? ScalarCond : Cond[Part],
          Op0[Part],
          Op1[Part]);
      }
      break;
    }

    case Instruction::ICmp:
    case Instruction::FCmp: {
      // Widen compares. Generate vector compares.
      bool FCmp = (it->getOpcode() == Instruction::FCmp);
      CmpInst *Cmp = dyn_cast<CmpInst>(it);
      VectorParts &A = getVectorValue(it->getOperand(0));
      VectorParts &B = getVectorValue(it->getOperand(1));
      for (unsigned Part = 0; Part < UF; ++Part) {
        Value *C = 0;
        if (FCmp)
          C = Builder.CreateFCmp(Cmp->getPredicate(), A[Part], B[Part]);
        else
          C = Builder.CreateICmp(Cmp->getPredicate(), A[Part], B[Part]);
        Entry[Part] = C;
      }
      break;
    }

    case Instruction::Store: {
      // Attempt to issue a wide store.
      StoreInst *SI = dyn_cast<StoreInst>(it);
      Type *StTy = VectorType::get(SI->getValueOperand()->getType(), VF);
      Value *Ptr = SI->getPointerOperand();
      unsigned Alignment = SI->getAlignment();

      assert(!Legal->isUniform(Ptr) &&
             "We do not allow storing to uniform addresses");


      int Stride = Legal->isConsecutivePtr(Ptr);
      bool Reverse = Stride < 0;
      if (Stride == 0) {
        scalarizeInstruction(it);
        break;
      }

      // Handle consecutive stores.

      GetElementPtrInst *Gep = dyn_cast<GetElementPtrInst>(Ptr);
      if (Gep) {
        // The last index does not have to be the induction. It can be
        // consecutive and be a function of the index. For example A[I+1];
        unsigned NumOperands = Gep->getNumOperands();

        Value *LastGepOperand = Gep->getOperand(NumOperands - 1);
        VectorParts &GEPParts = getVectorValue(LastGepOperand);
        Value *LastIndex = GEPParts[0];
        LastIndex = Builder.CreateExtractElement(LastIndex, Zero);

        // Create the new GEP with the new induction variable.
        GetElementPtrInst *Gep2 = cast<GetElementPtrInst>(Gep->clone());
        Gep2->setOperand(NumOperands - 1, LastIndex);
        Ptr = Builder.Insert(Gep2);
      } else {
        // Use the induction element ptr.
        assert(isa<PHINode>(Ptr) && "Invalid induction ptr");
        VectorParts &PtrVal = getVectorValue(Ptr);
        Ptr = Builder.CreateExtractElement(PtrVal[0], Zero);
      }

      VectorParts &StoredVal = getVectorValue(SI->getValueOperand());
      for (unsigned Part = 0; Part < UF; ++Part) {
        // Calculate the pointer for the specific unroll-part.
        Value *PartPtr = Builder.CreateGEP(Ptr, Builder.getInt32(Part * VF));

        if (Reverse) {
          // If we store to reverse consecutive memory locations then we need
          // to reverse the order of elements in the stored value.
          StoredVal[Part] = reverseVector(StoredVal[Part]);
          // If the address is consecutive but reversed, then the
          // wide store needs to start at the last vector element.
          PartPtr = Builder.CreateGEP(Ptr, Builder.getInt32(-Part * VF));
          PartPtr = Builder.CreateGEP(PartPtr, Builder.getInt32(1 - VF));
        }

        Value *VecPtr = Builder.CreateBitCast(PartPtr, StTy->getPointerTo());
        Builder.CreateStore(StoredVal[Part], VecPtr)->setAlignment(Alignment);
      }
      break;
    }
    case Instruction::Load: {
      // Attempt to issue a wide load.
      LoadInst *LI = dyn_cast<LoadInst>(it);
      Type *RetTy = VectorType::get(LI->getType(), VF);
      Value *Ptr = LI->getPointerOperand();
      unsigned Alignment = LI->getAlignment();

      // If the pointer is loop invariant or if it is non consecutive,
      // scalarize the load.
      int Stride = Legal->isConsecutivePtr(Ptr);
      bool Reverse = Stride < 0;
      if (Legal->isUniform(Ptr) || Stride == 0) {
        scalarizeInstruction(it);
        break;
      }

      GetElementPtrInst *Gep = dyn_cast<GetElementPtrInst>(Ptr);
      if (Gep) {
        // The last index does not have to be the induction. It can be
        // consecutive and be a function of the index. For example A[I+1];
        unsigned NumOperands = Gep->getNumOperands();

        Value *LastGepOperand = Gep->getOperand(NumOperands - 1);
        VectorParts &GEPParts = getVectorValue(LastGepOperand);
        Value *LastIndex = GEPParts[0];
        LastIndex = Builder.CreateExtractElement(LastIndex, Zero);

        // Create the new GEP with the new induction variable.
        GetElementPtrInst *Gep2 = cast<GetElementPtrInst>(Gep->clone());
        Gep2->setOperand(NumOperands - 1, LastIndex);
        Ptr = Builder.Insert(Gep2);
      } else {
        // Use the induction element ptr.
        assert(isa<PHINode>(Ptr) && "Invalid induction ptr");
        VectorParts &PtrVal = getVectorValue(Ptr);
        Ptr = Builder.CreateExtractElement(PtrVal[0], Zero);
      }

      for (unsigned Part = 0; Part < UF; ++Part) {
        // Calculate the pointer for the specific unroll-part.
        Value *PartPtr = Builder.CreateGEP(Ptr, Builder.getInt32(Part * VF));

        if (Reverse) {
          // If the address is consecutive but reversed, then the
          // wide store needs to start at the last vector element.
          PartPtr = Builder.CreateGEP(Ptr, Builder.getInt32(-Part * VF));
          PartPtr = Builder.CreateGEP(PartPtr, Builder.getInt32(1 - VF));
        }

        Value *VecPtr = Builder.CreateBitCast(PartPtr, RetTy->getPointerTo());
        Value *LI = Builder.CreateLoad(VecPtr, "wide.load");
        cast<LoadInst>(LI)->setAlignment(Alignment);
        Entry[Part] = Reverse ? reverseVector(LI) :  LI;
      }
      break;
    }
    case Instruction::ZExt:
    case Instruction::SExt:
    case Instruction::FPToUI:
    case Instruction::FPToSI:
    case Instruction::FPExt:
    case Instruction::PtrToInt:
    case Instruction::IntToPtr:
    case Instruction::SIToFP:
    case Instruction::UIToFP:
    case Instruction::Trunc:
    case Instruction::FPTrunc:
    case Instruction::BitCast: {
      CastInst *CI = dyn_cast<CastInst>(it);
      /// Optimize the special case where the source is the induction
      /// variable. Notice that we can only optimize the 'trunc' case
      /// because: a. FP conversions lose precision, b. sext/zext may wrap,
      /// c. other casts depend on pointer size.
      if (CI->getOperand(0) == OldInduction &&
          it->getOpcode() == Instruction::Trunc) {
        Value *ScalarCast = Builder.CreateCast(CI->getOpcode(), Induction,
                                               CI->getType());
        Value *Broadcasted = getBroadcastInstrs(ScalarCast);
        for (unsigned Part = 0; Part < UF; ++Part)
          Entry[Part] = getConsecutiveVector(Broadcasted, VF * Part, false);
        break;
      }
      /// Vectorize casts.
      Type *DestTy = VectorType::get(CI->getType()->getScalarType(), VF);

      VectorParts &A = getVectorValue(it->getOperand(0));
      for (unsigned Part = 0; Part < UF; ++Part)
        Entry[Part] = Builder.CreateCast(CI->getOpcode(), A[Part], DestTy);
      break;
    }

    case Instruction::Call: {
      assert(isTriviallyVectorizableIntrinsic(it));
      Module *M = BB->getParent()->getParent();
      IntrinsicInst *II = cast<IntrinsicInst>(it);
      Intrinsic::ID ID = II->getIntrinsicID();
      for (unsigned Part = 0; Part < UF; ++Part) {
        SmallVector<Value*, 4> Args;
        for (unsigned i = 0, ie = II->getNumArgOperands(); i != ie; ++i) {
          VectorParts &Arg = getVectorValue(II->getArgOperand(i));
          Args.push_back(Arg[Part]);
        }
        Type *Tys[] = { VectorType::get(II->getType()->getScalarType(), VF) };
        Function *F = Intrinsic::getDeclaration(M, ID, Tys);
        Entry[Part] = Builder.CreateCall(F, Args);
      }
      break;
    }

    default:
      // All other instructions are unsupported. Scalarize them.
      scalarizeInstruction(it);
      break;
    }// end of switch.
  }// end of for_each instr.
}

void InnerLoopVectorizer::updateAnalysis() {
  // Forget the original basic block.
  SE->forgetLoop(OrigLoop);

  // Update the dominator tree information.
  assert(DT->properlyDominates(LoopBypassBlock, LoopExitBlock) &&
         "Entry does not dominate exit.");

  DT->addNewBlock(LoopVectorPreHeader, LoopBypassBlock);
  DT->addNewBlock(LoopVectorBody, LoopVectorPreHeader);
  DT->addNewBlock(LoopMiddleBlock, LoopBypassBlock);
  DT->addNewBlock(LoopScalarPreHeader, LoopMiddleBlock);
  DT->changeImmediateDominator(LoopScalarBody, LoopScalarPreHeader);
  DT->changeImmediateDominator(LoopExitBlock, LoopMiddleBlock);

  DEBUG(DT->verifyAnalysis());
}

bool LoopVectorizationLegality::canVectorizeWithIfConvert() {
  if (!EnableIfConversion)
    return false;

  assert(TheLoop->getNumBlocks() > 1 && "Single block loops are vectorizable");
  std::vector<BasicBlock*> &LoopBlocks = TheLoop->getBlocksVector();

  // Collect the blocks that need predication.
  for (unsigned i = 0, e = LoopBlocks.size(); i < e; ++i) {
    BasicBlock *BB = LoopBlocks[i];

    // We don't support switch statements inside loops.
    if (!isa<BranchInst>(BB->getTerminator()))
      return false;

    // We must have at most two predecessors because we need to convert
    // all PHIs to selects.
    unsigned Preds = std::distance(pred_begin(BB), pred_end(BB));
    if (Preds > 2)
      return false;

    // We must be able to predicate all blocks that need to be predicated.
    if (blockNeedsPredication(BB) && !blockCanBePredicated(BB))
      return false;
  }

  // We can if-convert this loop.
  return true;
}

bool LoopVectorizationLegality::canVectorize() {
  assert(TheLoop->getLoopPreheader() && "No preheader!!");

  // We can only vectorize innermost loops.
  if (TheLoop->getSubLoopsVector().size())
    return false;

  // We must have a single backedge.
  if (TheLoop->getNumBackEdges() != 1)
    return false;

  // We must have a single exiting block.
  if (!TheLoop->getExitingBlock())
    return false;

  unsigned NumBlocks = TheLoop->getNumBlocks();

  // Check if we can if-convert non single-bb loops.
  if (NumBlocks != 1 && !canVectorizeWithIfConvert()) {
    DEBUG(dbgs() << "LV: Can't if-convert the loop.\n");
    return false;
  }

  // We need to have a loop header.
  BasicBlock *Latch = TheLoop->getLoopLatch();
  DEBUG(dbgs() << "LV: Found a loop: " <<
        TheLoop->getHeader()->getName() << "\n");

  // ScalarEvolution needs to be able to find the exit count.
  const SCEV *ExitCount = SE->getExitCount(TheLoop, Latch);
  if (ExitCount == SE->getCouldNotCompute()) {
    DEBUG(dbgs() << "LV: SCEV could not compute the loop exit count.\n");
    return false;
  }

  // Do not loop-vectorize loops with a tiny trip count.
  unsigned TC = SE->getSmallConstantTripCount(TheLoop, Latch);
  if (TC > 0u && TC < TinyTripCountVectorThreshold) {
    DEBUG(dbgs() << "LV: Found a loop with a very small trip count. " <<
          "This loop is not worth vectorizing.\n");
    return false;
  }

  // Check if we can vectorize the instructions and CFG in this loop.
  if (!canVectorizeInstrs()) {
    DEBUG(dbgs() << "LV: Can't vectorize the instructions or CFG\n");
    return false;
  }

  // Go over each instruction and look at memory deps.
  if (!canVectorizeMemory()) {
    DEBUG(dbgs() << "LV: Can't vectorize due to memory conflicts\n");
    return false;
  }

  // Collect all of the variables that remain uniform after vectorization.
  collectLoopUniforms();

  DEBUG(dbgs() << "LV: We can vectorize this loop" <<
        (PtrRtCheck.Need ? " (with a runtime bound check)" : "")
        <<"!\n");

  // Okay! We can vectorize. At this point we don't have any other mem analysis
  // which may limit our maximum vectorization factor, so just return true with
  // no restrictions.
  return true;
}

bool LoopVectorizationLegality::canVectorizeInstrs() {
  BasicBlock *PreHeader = TheLoop->getLoopPreheader();
  BasicBlock *Header = TheLoop->getHeader();

  // For each block in the loop.
  for (Loop::block_iterator bb = TheLoop->block_begin(),
       be = TheLoop->block_end(); bb != be; ++bb) {

    // Scan the instructions in the block and look for hazards.
    for (BasicBlock::iterator it = (*bb)->begin(), e = (*bb)->end(); it != e;
         ++it) {

      if (PHINode *Phi = dyn_cast<PHINode>(it)) {
        // This should not happen because the loop should be normalized.
        if (Phi->getNumIncomingValues() != 2) {
          DEBUG(dbgs() << "LV: Found an invalid PHI.\n");
          return false;
        }

        // Check that this PHI type is allowed.
        if (!Phi->getType()->isIntegerTy() &&
            !Phi->getType()->isFloatingPointTy() &&
            !Phi->getType()->isPointerTy()) {
          DEBUG(dbgs() << "LV: Found an non-int non-pointer PHI.\n");
          return false;
        }

        // If this PHINode is not in the header block, then we know that we
        // can convert it to select during if-conversion. No need to check if
        // the PHIs in this block are induction or reduction variables.
        if (*bb != Header)
          continue;

        // This is the value coming from the preheader.
        Value *StartValue = Phi->getIncomingValueForBlock(PreHeader);
        // Check if this is an induction variable.
        InductionKind IK = isInductionVariable(Phi);

        if (IK_NoInduction != IK) {
          // Int inductions are special because we only allow one IV.
          if (IK == IK_IntInduction) {
            if (Induction) {
              DEBUG(dbgs() << "LV: Found too many inductions."<< *Phi <<"\n");
              return false;
            }
            Induction = Phi;
          }

          DEBUG(dbgs() << "LV: Found an induction variable.\n");
          Inductions[Phi] = InductionInfo(StartValue, IK);
          continue;
        }

        if (AddReductionVar(Phi, RK_IntegerAdd)) {
          DEBUG(dbgs() << "LV: Found an ADD reduction PHI."<< *Phi <<"\n");
          continue;
        }
        if (AddReductionVar(Phi, RK_IntegerMult)) {
          DEBUG(dbgs() << "LV: Found a MUL reduction PHI."<< *Phi <<"\n");
          continue;
        }
        if (AddReductionVar(Phi, RK_IntegerOr)) {
          DEBUG(dbgs() << "LV: Found an OR reduction PHI."<< *Phi <<"\n");
          continue;
        }
        if (AddReductionVar(Phi, RK_IntegerAnd)) {
          DEBUG(dbgs() << "LV: Found an AND reduction PHI."<< *Phi <<"\n");
          continue;
        }
        if (AddReductionVar(Phi, RK_IntegerXor)) {
          DEBUG(dbgs() << "LV: Found a XOR reduction PHI."<< *Phi <<"\n");
          continue;
        }
        if (AddReductionVar(Phi, RK_FloatMult)) {
          DEBUG(dbgs() << "LV: Found an FMult reduction PHI."<< *Phi <<"\n");
          continue;
        }
        if (AddReductionVar(Phi, RK_FloatAdd)) {
          DEBUG(dbgs() << "LV: Found an FAdd reduction PHI."<< *Phi <<"\n");
          continue;
        }

        DEBUG(dbgs() << "LV: Found an unidentified PHI."<< *Phi <<"\n");
        return false;
      }// end of PHI handling

      // We still don't handle functions.
      CallInst *CI = dyn_cast<CallInst>(it);
      if (CI && !isTriviallyVectorizableIntrinsic(it)) {
        DEBUG(dbgs() << "LV: Found a call site.\n");
        return false;
      }

      // Check that the instruction return type is vectorizable.
      if (!VectorType::isValidElementType(it->getType()) &&
          !it->getType()->isVoidTy()) {
        DEBUG(dbgs() << "LV: Found unvectorizable type." << "\n");
        return false;
      }

      // Check that the stored type is vectorizable.
      if (StoreInst *ST = dyn_cast<StoreInst>(it)) {
        Type *T = ST->getValueOperand()->getType();
        if (!VectorType::isValidElementType(T))
          return false;
      }

      // Reduction instructions are allowed to have exit users.
      // All other instructions must not have external users.
      if (!AllowedExit.count(it))
        //Check that all of the users of the loop are inside the BB.
        for (Value::use_iterator I = it->use_begin(), E = it->use_end();
             I != E; ++I) {
          Instruction *U = cast<Instruction>(*I);
          // This user may be a reduction exit value.
          if (!TheLoop->contains(U)) {
            DEBUG(dbgs() << "LV: Found an outside user for : "<< *U << "\n");
            return false;
          }
        }
    } // next instr.

  }

  if (!Induction) {
    DEBUG(dbgs() << "LV: Did not find one integer induction var.\n");
    assert(getInductionVars()->size() && "No induction variables");
  }

  return true;
}

void LoopVectorizationLegality::collectLoopUniforms() {
  // We now know that the loop is vectorizable!
  // Collect variables that will remain uniform after vectorization.
  std::vector<Value*> Worklist;
  BasicBlock *Latch = TheLoop->getLoopLatch();

  // Start with the conditional branch and walk up the block.
  Worklist.push_back(Latch->getTerminator()->getOperand(0));

  while (Worklist.size()) {
    Instruction *I = dyn_cast<Instruction>(Worklist.back());
    Worklist.pop_back();

    // Look at instructions inside this loop.
    // Stop when reaching PHI nodes.
    // TODO: we need to follow values all over the loop, not only in this block.
    if (!I || !TheLoop->contains(I) || isa<PHINode>(I))
      continue;

    // This is a known uniform.
    Uniforms.insert(I);

    // Insert all operands.
    for (int i = 0, Op = I->getNumOperands(); i < Op; ++i) {
      Worklist.push_back(I->getOperand(i));
    }
  }
}

bool LoopVectorizationLegality::canVectorizeMemory() {
  typedef SmallVector<Value*, 16> ValueVector;
  typedef SmallPtrSet<Value*, 16> ValueSet;
  // Holds the Load and Store *instructions*.
  ValueVector Loads;
  ValueVector Stores;
  PtrRtCheck.Pointers.clear();
  PtrRtCheck.Need = false;

  // For each block.
  for (Loop::block_iterator bb = TheLoop->block_begin(),
       be = TheLoop->block_end(); bb != be; ++bb) {

    // Scan the BB and collect legal loads and stores.
    for (BasicBlock::iterator it = (*bb)->begin(), e = (*bb)->end(); it != e;
         ++it) {

      // If this is a load, save it. If this instruction can read from memory
      // but is not a load, then we quit. Notice that we don't handle function
      // calls that read or write.
      if (it->mayReadFromMemory()) {
        LoadInst *Ld = dyn_cast<LoadInst>(it);
        if (!Ld) return false;
        if (!Ld->isSimple()) {
          DEBUG(dbgs() << "LV: Found a non-simple load.\n");
          return false;
        }
        Loads.push_back(Ld);
        continue;
      }

      // Save 'store' instructions. Abort if other instructions write to memory.
      if (it->mayWriteToMemory()) {
        StoreInst *St = dyn_cast<StoreInst>(it);
        if (!St) return false;
        if (!St->isSimple()) {
          DEBUG(dbgs() << "LV: Found a non-simple store.\n");
          return false;
        }
        Stores.push_back(St);
      }
    } // next instr.
  } // next block.

  // Now we have two lists that hold the loads and the stores.
  // Next, we find the pointers that they use.

  // Check if we see any stores. If there are no stores, then we don't
  // care if the pointers are *restrict*.
  if (!Stores.size()) {
    DEBUG(dbgs() << "LV: Found a read-only loop!\n");
    return true;
  }

  // Holds the read and read-write *pointers* that we find.
  ValueVector Reads;
  ValueVector ReadWrites;

  // Holds the analyzed pointers. We don't want to call GetUnderlyingObjects
  // multiple times on the same object. If the ptr is accessed twice, once
  // for read and once for write, it will only appear once (on the write
  // list). This is okay, since we are going to check for conflicts between
  // writes and between reads and writes, but not between reads and reads.
  ValueSet Seen;

  ValueVector::iterator I, IE;
  for (I = Stores.begin(), IE = Stores.end(); I != IE; ++I) {
    StoreInst *ST = cast<StoreInst>(*I);
    Value* Ptr = ST->getPointerOperand();

    if (isUniform(Ptr)) {
      DEBUG(dbgs() << "LV: We don't allow storing to uniform addresses\n");
      return false;
    }

    // If we did *not* see this pointer before, insert it to
    // the read-write list. At this phase it is only a 'write' list.
    if (Seen.insert(Ptr))
      ReadWrites.push_back(Ptr);
  }

  for (I = Loads.begin(), IE = Loads.end(); I != IE; ++I) {
    LoadInst *LD = cast<LoadInst>(*I);
    Value* Ptr = LD->getPointerOperand();
    // If we did *not* see this pointer before, insert it to the
    // read list. If we *did* see it before, then it is already in
    // the read-write list. This allows us to vectorize expressions
    // such as A[i] += x;  Because the address of A[i] is a read-write
    // pointer. This only works if the index of A[i] is consecutive.
    // If the address of i is unknown (for example A[B[i]]) then we may
    // read a few words, modify, and write a few words, and some of the
    // words may be written to the same address.
    if (Seen.insert(Ptr) || 0 == isConsecutivePtr(Ptr))
      Reads.push_back(Ptr);
  }

  // If we write (or read-write) to a single destination and there are no
  // other reads in this loop then is it safe to vectorize.
  if (ReadWrites.size() == 1 && Reads.size() == 0) {
    DEBUG(dbgs() << "LV: Found a write-only loop!\n");
    return true;
  }

  // Find pointers with computable bounds. We are going to use this information
  // to place a runtime bound check.
  bool CanDoRT = true;
  for (I = ReadWrites.begin(), IE = ReadWrites.end(); I != IE; ++I)
    if (hasComputableBounds(*I)) {
      PtrRtCheck.insert(SE, TheLoop, *I);
      DEBUG(dbgs() << "LV: Found a runtime check ptr:" << **I <<"\n");
    } else {
      CanDoRT = false;
      break;
    }
  for (I = Reads.begin(), IE = Reads.end(); I != IE; ++I)
    if (hasComputableBounds(*I)) {
      PtrRtCheck.insert(SE, TheLoop, *I);
      DEBUG(dbgs() << "LV: Found a runtime check ptr:" << **I <<"\n");
    } else {
      CanDoRT = false;
      break;
    }

  // Check that we did not collect too many pointers or found a
  // unsizeable pointer.
  if (!CanDoRT || PtrRtCheck.Pointers.size() > RuntimeMemoryCheckThreshold) {
    PtrRtCheck.reset();
    CanDoRT = false;
  }

  if (CanDoRT) {
    DEBUG(dbgs() << "LV: We can perform a memory runtime check if needed.\n");
  }

  bool NeedRTCheck = false;

  // Now that the pointers are in two lists (Reads and ReadWrites), we
  // can check that there are no conflicts between each of the writes and
  // between the writes to the reads.
  ValueSet WriteObjects;
  ValueVector TempObjects;

  // Check that the read-writes do not conflict with other read-write
  // pointers.
  bool AllWritesIdentified = true;
  for (I = ReadWrites.begin(), IE = ReadWrites.end(); I != IE; ++I) {
    GetUnderlyingObjects(*I, TempObjects, DL);
    for (ValueVector::iterator it=TempObjects.begin(), e=TempObjects.end();
         it != e; ++it) {
      if (!isIdentifiedObject(*it)) {
        DEBUG(dbgs() << "LV: Found an unidentified write ptr:"<< **it <<"\n");
        NeedRTCheck = true;
        AllWritesIdentified = false;
      }
      if (!WriteObjects.insert(*it)) {
        DEBUG(dbgs() << "LV: Found a possible write-write reorder:"
              << **it <<"\n");
        return false;
      }
    }
    TempObjects.clear();
  }

  /// Check that the reads don't conflict with the read-writes.
  for (I = Reads.begin(), IE = Reads.end(); I != IE; ++I) {
    GetUnderlyingObjects(*I, TempObjects, DL);
    for (ValueVector::iterator it=TempObjects.begin(), e=TempObjects.end();
         it != e; ++it) {
      // If all of the writes are identified then we don't care if the read
      // pointer is identified or not.
      if (!AllWritesIdentified && !isIdentifiedObject(*it)) {
        DEBUG(dbgs() << "LV: Found an unidentified read ptr:"<< **it <<"\n");
        NeedRTCheck = true;
      }
      if (WriteObjects.count(*it)) {
        DEBUG(dbgs() << "LV: Found a possible read/write reorder:"
              << **it <<"\n");
        return false;
      }
    }
    TempObjects.clear();
  }

  PtrRtCheck.Need = NeedRTCheck;
  if (NeedRTCheck && !CanDoRT) {
    DEBUG(dbgs() << "LV: We can't vectorize because we can't find " <<
          "the array bounds.\n");
    PtrRtCheck.reset();
    return false;
  }

  DEBUG(dbgs() << "LV: We "<< (NeedRTCheck ? "" : "don't") <<
        " need a runtime memory check.\n");
  return true;
}

bool LoopVectorizationLegality::AddReductionVar(PHINode *Phi,
                                                ReductionKind Kind) {
  if (Phi->getNumIncomingValues() != 2)
    return false;

  // Reduction variables are only found in the loop header block.
  if (Phi->getParent() != TheLoop->getHeader())
    return false;

  // Obtain the reduction start value from the value that comes from the loop
  // preheader.
  Value *RdxStart = Phi->getIncomingValueForBlock(TheLoop->getLoopPreheader());

  // ExitInstruction is the single value which is used outside the loop.
  // We only allow for a single reduction value to be used outside the loop.
  // This includes users of the reduction, variables (which form a cycle
  // which ends in the phi node).
  Instruction *ExitInstruction = 0;
  // Indicates that we found a binary operation in our scan.
  bool FoundBinOp = false;

  // Iter is our iterator. We start with the PHI node and scan for all of the
  // users of this instruction. All users must be instructions that can be
  // used as reduction variables (such as ADD). We may have a single
  // out-of-block user. The cycle must end with the original PHI.
  Instruction *Iter = Phi;
  while (true) {
    // If the instruction has no users then this is a broken
    // chain and can't be a reduction variable.
    if (Iter->use_empty())
      return false;

    // Did we find a user inside this loop already ?
    bool FoundInBlockUser = false;
    // Did we reach the initial PHI node already ?
    bool FoundStartPHI = false;

    // Is this a bin op ?
    FoundBinOp |= !isa<PHINode>(Iter);

    // For each of the *users* of iter.
    for (Value::use_iterator it = Iter->use_begin(), e = Iter->use_end();
         it != e; ++it) {
      Instruction *U = cast<Instruction>(*it);
      // We already know that the PHI is a user.
      if (U == Phi) {
        FoundStartPHI = true;
        continue;
      }

      // Check if we found the exit user.
      BasicBlock *Parent = U->getParent();
      if (!TheLoop->contains(Parent)) {
        // Exit if you find multiple outside users.
        if (ExitInstruction != 0)
          return false;
        ExitInstruction = Iter;
      }

      // We allow in-loop PHINodes which are not the original reduction PHI
      // node. If this PHI is the only user of Iter (happens in IF w/ no ELSE
      // structure) then don't skip this PHI.
      if (isa<PHINode>(Iter) && isa<PHINode>(U) &&
          U->getParent() != TheLoop->getHeader() &&
          TheLoop->contains(U) &&
          Iter->getNumUses() > 1)
        continue;

      // We can't have multiple inside users.
      if (FoundInBlockUser)
        return false;
      FoundInBlockUser = true;

      // Any reduction instr must be of one of the allowed kinds.
      if (!isReductionInstr(U, Kind))
        return false;

      // Reductions of instructions such as Div, and Sub is only
      // possible if the LHS is the reduction variable.
      if (!U->isCommutative() && !isa<PHINode>(U) && U->getOperand(0) != Iter)
        return false;

      Iter = U;
    }

    // We found a reduction var if we have reached the original
    // phi node and we only have a single instruction with out-of-loop
    // users.
    if (FoundStartPHI) {
      // This instruction is allowed to have out-of-loop users.
      AllowedExit.insert(ExitInstruction);

      // Save the description of this reduction variable.
      ReductionDescriptor RD(RdxStart, ExitInstruction, Kind);
      Reductions[Phi] = RD;
      // We've ended the cycle. This is a reduction variable if we have an
      // outside user and it has a binary op.
      return FoundBinOp && ExitInstruction;
    }
  }
}

bool
LoopVectorizationLegality::isReductionInstr(Instruction *I,
                                            ReductionKind Kind) {
  bool FP = I->getType()->isFloatingPointTy();
  bool FastMath = (FP && I->isCommutative() && I->isAssociative());

  switch (I->getOpcode()) {
  default:
    return false;
  case Instruction::PHI:
      if (FP && (Kind != RK_FloatMult && Kind != RK_FloatAdd))
        return false;
    // possibly.
    return true;
  case Instruction::Sub:
  case Instruction::Add:
    return Kind == RK_IntegerAdd;
  case Instruction::SDiv:
  case Instruction::UDiv:
  case Instruction::Mul:
    return Kind == RK_IntegerMult;
  case Instruction::And:
    return Kind == RK_IntegerAnd;
  case Instruction::Or:
    return Kind == RK_IntegerOr;
  case Instruction::Xor:
    return Kind == RK_IntegerXor;
  case Instruction::FMul:
    return Kind == RK_FloatMult && FastMath;
  case Instruction::FAdd:
    return Kind == RK_FloatAdd && FastMath;
   }
}

LoopVectorizationLegality::InductionKind
LoopVectorizationLegality::isInductionVariable(PHINode *Phi) {
  Type *PhiTy = Phi->getType();
  // We only handle integer and pointer inductions variables.
  if (!PhiTy->isIntegerTy() && !PhiTy->isPointerTy())
    return IK_NoInduction;

  // Check that the PHI is consecutive and starts at zero.
  const SCEV *PhiScev = SE->getSCEV(Phi);
  const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(PhiScev);
  if (!AR) {
    DEBUG(dbgs() << "LV: PHI is not a poly recurrence.\n");
    return IK_NoInduction;
  }
  const SCEV *Step = AR->getStepRecurrence(*SE);

  // Integer inductions need to have a stride of one.
  if (PhiTy->isIntegerTy()) {
    if (Step->isOne())
      return IK_IntInduction;
    if (Step->isAllOnesValue())
      return IK_ReverseIntInduction;
    return IK_NoInduction;
  }

  // Calculate the pointer stride and check if it is consecutive.
  const SCEVConstant *C = dyn_cast<SCEVConstant>(Step);
  if (!C)
    return IK_NoInduction;

  assert(PhiTy->isPointerTy() && "The PHI must be a pointer");
  uint64_t Size = DL->getTypeAllocSize(PhiTy->getPointerElementType());
  if (C->getValue()->equalsInt(Size))
    return IK_PtrInduction;

  return IK_NoInduction;
}

bool LoopVectorizationLegality::isInductionVariable(const Value *V) {
  Value *In0 = const_cast<Value*>(V);
  PHINode *PN = dyn_cast_or_null<PHINode>(In0);
  if (!PN)
    return false;

  return Inductions.count(PN);
}

bool LoopVectorizationLegality::blockNeedsPredication(BasicBlock *BB)  {
  assert(TheLoop->contains(BB) && "Unknown block used");

  // Blocks that do not dominate the latch need predication.
  BasicBlock* Latch = TheLoop->getLoopLatch();
  return !DT->dominates(BB, Latch);
}

bool LoopVectorizationLegality::blockCanBePredicated(BasicBlock *BB) {
  for (BasicBlock::iterator it = BB->begin(), e = BB->end(); it != e; ++it) {
    // We don't predicate loads/stores at the moment.
    if (it->mayReadFromMemory() || it->mayWriteToMemory() || it->mayThrow())
      return false;

    // The instructions below can trap.
    switch (it->getOpcode()) {
    default: continue;
    case Instruction::UDiv:
    case Instruction::SDiv:
    case Instruction::URem:
    case Instruction::SRem:
             return false;
    }
  }

  return true;
}

bool LoopVectorizationLegality::hasComputableBounds(Value *Ptr) {
  const SCEV *PhiScev = SE->getSCEV(Ptr);
  const SCEVAddRecExpr *AR = dyn_cast<SCEVAddRecExpr>(PhiScev);
  if (!AR)
    return false;

  return AR->isAffine();
}

unsigned
LoopVectorizationCostModel::selectVectorizationFactor(bool OptForSize,
                                                      unsigned UserVF) {
  if (OptForSize && Legal->getRuntimePointerCheck()->Need) {
    DEBUG(dbgs() << "LV: Aborting. Runtime ptr check is required in Os.\n");
    return 1;
  }

  // Find the trip count.
  unsigned TC = SE->getSmallConstantTripCount(TheLoop, TheLoop->getLoopLatch());
  DEBUG(dbgs() << "LV: Found trip count:"<<TC<<"\n");

  unsigned WidestType = getWidestType();
  unsigned WidestRegister = TTI.getRegisterBitWidth(true);
  unsigned MaxVectorSize = WidestRegister / WidestType;
  DEBUG(dbgs() << "LV: The Widest type: " << WidestType << " bits.\n");
  DEBUG(dbgs() << "LV: The Widest register is:" << WidestRegister << "bits.\n");

  if (MaxVectorSize == 0) {
    DEBUG(dbgs() << "LV: The target has no vector registers.\n");
    MaxVectorSize = 1;
  }

  assert(MaxVectorSize <= 32 && "Did not expect to pack so many elements"
         " into one vector.");

  unsigned VF = MaxVectorSize;

  // If we optimize the program for size, avoid creating the tail loop.
  if (OptForSize) {
    // If we are unable to calculate the trip count then don't try to vectorize.
    if (TC < 2) {
      DEBUG(dbgs() << "LV: Aborting. A tail loop is required in Os.\n");
      return 1;
    }

    // Find the maximum SIMD width that can fit within the trip count.
    VF = TC % MaxVectorSize;

    if (VF == 0)
      VF = MaxVectorSize;

    // If the trip count that we found modulo the vectorization factor is not
    // zero then we require a tail.
    if (VF < 2) {
      DEBUG(dbgs() << "LV: Aborting. A tail loop is required in Os.\n");
      return 1;
    }
  }

  if (UserVF != 0) {
    assert(isPowerOf2_32(UserVF) && "VF needs to be a power of two");
    DEBUG(dbgs() << "LV: Using user VF "<<UserVF<<".\n");

    return UserVF;
  }

  float Cost = expectedCost(1);
  unsigned Width = 1;
  DEBUG(dbgs() << "LV: Scalar loop costs: "<< (int)Cost << ".\n");
  for (unsigned i=2; i <= VF; i*=2) {
    // Notice that the vector loop needs to be executed less times, so
    // we need to divide the cost of the vector loops by the width of
    // the vector elements.
    float VectorCost = expectedCost(i) / (float)i;
    DEBUG(dbgs() << "LV: Vector loop of width "<< i << " costs: " <<
          (int)VectorCost << ".\n");
    if (VectorCost < Cost) {
      Cost = VectorCost;
      Width = i;
    }
  }

  DEBUG(dbgs() << "LV: Selecting VF = : "<< Width << ".\n");
  return Width;
}

unsigned LoopVectorizationCostModel::getWidestType() {
  unsigned MaxWidth = 8;

  // For each block.
  for (Loop::block_iterator bb = TheLoop->block_begin(),
       be = TheLoop->block_end(); bb != be; ++bb) {
    BasicBlock *BB = *bb;

    // For each instruction in the loop.
    for (BasicBlock::iterator it = BB->begin(), e = BB->end(); it != e; ++it) {
      Type *T = it->getType();

      // Only examine Loads, Stores and PHINodes.
      if (!isa<LoadInst>(it) && !isa<StoreInst>(it) && !isa<PHINode>(it))
        continue;

      // Examine PHI nodes that are reduction variables.
      if (PHINode *PN = dyn_cast<PHINode>(it))
        if (!Legal->getReductionVars()->count(PN))
          continue;

      // Examine the stored values.
      if (StoreInst *ST = dyn_cast<StoreInst>(it))
        T = ST->getValueOperand()->getType();

      // Ignore stored/loaded pointer types.
      if (T->isPointerTy())
        continue;

      MaxWidth = std::max(MaxWidth, T->getScalarSizeInBits());
    }
  }

  return MaxWidth;
}

unsigned
LoopVectorizationCostModel::selectUnrollFactor(bool OptForSize,
                                               unsigned UserUF) {
  // Use the user preference, unless 'auto' is selected.
  if (UserUF != 0)
    return UserUF;

  // When we optimize for size we don't unroll.
  if (OptForSize)
    return 1;

  // Do not unroll loops with a relatively small trip count.
  unsigned TC = SE->getSmallConstantTripCount(TheLoop,
                                              TheLoop->getLoopLatch());
  if (TC > 1 && TC < TinyTripCountUnrollThreshold)
    return 1;

  unsigned TargetVectorRegisters = TTI.getNumberOfRegisters(true);
  DEBUG(dbgs() << "LV: The target has " << TargetVectorRegisters <<
        " vector registers\n");

  LoopVectorizationCostModel::RegisterUsage R = calculateRegisterUsage();
  // We divide by these constants so assume that we have at least one
  // instruction that uses at least one register.
  R.MaxLocalUsers = std::max(R.MaxLocalUsers, 1U);
  R.NumInstructions = std::max(R.NumInstructions, 1U);

  // We calculate the unroll factor using the following formula.
  // Subtract the number of loop invariants from the number of available
  // registers. These registers are used by all of the unrolled instances.
  // Next, divide the remaining registers by the number of registers that is
  // required by the loop, in order to estimate how many parallel instances
  // fit without causing spills.
  unsigned UF = (TargetVectorRegisters - R.LoopInvariantRegs) / R.MaxLocalUsers;

  // We don't want to unroll the loops to the point where they do not fit into
  // the decoded cache. Assume that we only allow 32 IR instructions.
  UF = std::min(UF, (MaxLoopSizeThreshold / R.NumInstructions));

  // Clamp the unroll factor ranges to reasonable factors.
  unsigned MaxUnrollSize = TTI.getMaximumUnrollFactor();
  
  if (UF > MaxUnrollSize)
    UF = MaxUnrollSize;
  else if (UF < 1)
    UF = 1;

  return UF;
}

LoopVectorizationCostModel::RegisterUsage
LoopVectorizationCostModel::calculateRegisterUsage() {
  // This function calculates the register usage by measuring the highest number
  // of values that are alive at a single location. Obviously, this is a very
  // rough estimation. We scan the loop in a topological order in order and
  // assign a number to each instruction. We use RPO to ensure that defs are
  // met before their users. We assume that each instruction that has in-loop
  // users starts an interval. We record every time that an in-loop value is
  // used, so we have a list of the first and last occurrences of each
  // instruction. Next, we transpose this data structure into a multi map that
  // holds the list of intervals that *end* at a specific location. This multi
  // map allows us to perform a linear search. We scan the instructions linearly
  // and record each time that a new interval starts, by placing it in a set.
  // If we find this value in the multi-map then we remove it from the set.
  // The max register usage is the maximum size of the set.
  // We also search for instructions that are defined outside the loop, but are
  // used inside the loop. We need this number separately from the max-interval
  // usage number because when we unroll, loop-invariant values do not take
  // more register.
  LoopBlocksDFS DFS(TheLoop);
  DFS.perform(LI);

  RegisterUsage R;
  R.NumInstructions = 0;

  // Each 'key' in the map opens a new interval. The values
  // of the map are the index of the 'last seen' usage of the
  // instruction that is the key.
  typedef DenseMap<Instruction*, unsigned> IntervalMap;
  // Maps instruction to its index.
  DenseMap<unsigned, Instruction*> IdxToInstr;
  // Marks the end of each interval.
  IntervalMap EndPoint;
  // Saves the list of instruction indices that are used in the loop.
  SmallSet<Instruction*, 8> Ends;
  // Saves the list of values that are used in the loop but are
  // defined outside the loop, such as arguments and constants.
  SmallPtrSet<Value*, 8> LoopInvariants;

  unsigned Index = 0;
  for (LoopBlocksDFS::RPOIterator bb = DFS.beginRPO(),
       be = DFS.endRPO(); bb != be; ++bb) {
    R.NumInstructions += (*bb)->size();
    for (BasicBlock::iterator it = (*bb)->begin(), e = (*bb)->end(); it != e;
         ++it) {
      Instruction *I = it;
      IdxToInstr[Index++] = I;

      // Save the end location of each USE.
      for (unsigned i = 0; i < I->getNumOperands(); ++i) {
        Value *U = I->getOperand(i);
        Instruction *Instr = dyn_cast<Instruction>(U);

        // Ignore non-instruction values such as arguments, constants, etc.
        if (!Instr) continue;

        // If this instruction is outside the loop then record it and continue.
        if (!TheLoop->contains(Instr)) {
          LoopInvariants.insert(Instr);
          continue;
        }

        // Overwrite previous end points.
        EndPoint[Instr] = Index;
        Ends.insert(Instr);
      }
    }
  }

  // Saves the list of intervals that end with the index in 'key'.
  typedef SmallVector<Instruction*, 2> InstrList;
  DenseMap<unsigned, InstrList> TransposeEnds;

  // Transpose the EndPoints to a list of values that end at each index.
  for (IntervalMap::iterator it = EndPoint.begin(), e = EndPoint.end();
       it != e; ++it)
    TransposeEnds[it->second].push_back(it->first);

  SmallSet<Instruction*, 8> OpenIntervals;
  unsigned MaxUsage = 0;


  DEBUG(dbgs() << "LV(REG): Calculating max register usage:\n");
  for (unsigned int i = 0; i < Index; ++i) {
    Instruction *I = IdxToInstr[i];
    // Ignore instructions that are never used within the loop.
    if (!Ends.count(I)) continue;

    // Remove all of the instructions that end at this location.
    InstrList &List = TransposeEnds[i];
    for (unsigned int j=0, e = List.size(); j < e; ++j)
      OpenIntervals.erase(List[j]);

    // Count the number of live interals.
    MaxUsage = std::max(MaxUsage, OpenIntervals.size());

    DEBUG(dbgs() << "LV(REG): At #" << i << " Interval # " <<
          OpenIntervals.size() <<"\n");

    // Add the current instruction to the list of open intervals.
    OpenIntervals.insert(I);
  }

  unsigned Invariant = LoopInvariants.size();
  DEBUG(dbgs() << "LV(REG): Found max usage: " << MaxUsage << " \n");
  DEBUG(dbgs() << "LV(REG): Found invariant usage: " << Invariant << " \n");
  DEBUG(dbgs() << "LV(REG): LoopSize: " << R.NumInstructions << " \n");

  R.LoopInvariantRegs = Invariant;
  R.MaxLocalUsers = MaxUsage;
  return R;
}

unsigned LoopVectorizationCostModel::expectedCost(unsigned VF) {
  unsigned Cost = 0;

  // For each block.
  for (Loop::block_iterator bb = TheLoop->block_begin(),
       be = TheLoop->block_end(); bb != be; ++bb) {
    unsigned BlockCost = 0;
    BasicBlock *BB = *bb;

    // For each instruction in the old loop.
    for (BasicBlock::iterator it = BB->begin(), e = BB->end(); it != e; ++it) {
      unsigned C = getInstructionCost(it, VF);
      Cost += C;
      DEBUG(dbgs() << "LV: Found an estimated cost of "<< C <<" for VF " <<
            VF << " For instruction: "<< *it << "\n");
    }

    // We assume that if-converted blocks have a 50% chance of being executed.
    // When the code is scalar then some of the blocks are avoided due to CF.
    // When the code is vectorized we execute all code paths.
    if (Legal->blockNeedsPredication(*bb) && VF == 1)
      BlockCost /= 2;

    Cost += BlockCost;
  }

  return Cost;
}

unsigned
LoopVectorizationCostModel::getInstructionCost(Instruction *I, unsigned VF) {
  // If we know that this instruction will remain uniform, check the cost of
  // the scalar version.
  if (Legal->isUniformAfterVectorization(I))
    VF = 1;

  Type *RetTy = I->getType();
  Type *VectorTy = ToVectorTy(RetTy, VF);

  // TODO: We need to estimate the cost of intrinsic calls.
  switch (I->getOpcode()) {
  case Instruction::GetElementPtr:
    // We mark this instruction as zero-cost because scalar GEPs are usually
    // lowered to the intruction addressing mode. At the moment we don't
    // generate vector geps.
    return 0;
  case Instruction::Br: {
    return TTI.getCFInstrCost(I->getOpcode());
  }
  case Instruction::PHI:
    //TODO: IF-converted IFs become selects.
    return 0;
  case Instruction::Add:
  case Instruction::FAdd:
  case Instruction::Sub:
  case Instruction::FSub:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::FRem:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor:
    return TTI.getArithmeticInstrCost(I->getOpcode(), VectorTy);
  case Instruction::Select: {
    SelectInst *SI = cast<SelectInst>(I);
    const SCEV *CondSCEV = SE->getSCEV(SI->getCondition());
    bool ScalarCond = (SE->isLoopInvariant(CondSCEV, TheLoop));
    Type *CondTy = SI->getCondition()->getType();
    if (ScalarCond)
      CondTy = VectorType::get(CondTy, VF);

    return TTI.getCmpSelInstrCost(I->getOpcode(), VectorTy, CondTy);
  }
  case Instruction::ICmp:
  case Instruction::FCmp: {
    Type *ValTy = I->getOperand(0)->getType();
    VectorTy = ToVectorTy(ValTy, VF);
    return TTI.getCmpSelInstrCost(I->getOpcode(), VectorTy);
  }
  case Instruction::Store: {
    StoreInst *SI = cast<StoreInst>(I);
    Type *ValTy = SI->getValueOperand()->getType();
    VectorTy = ToVectorTy(ValTy, VF);

    if (VF == 1)
      return TTI.getMemoryOpCost(I->getOpcode(), VectorTy,
                                   SI->getAlignment(),
                                   SI->getPointerAddressSpace());

    // Scalarized stores.
    int Stride = Legal->isConsecutivePtr(SI->getPointerOperand());
    bool Reverse = Stride < 0;
    if (0 == Stride) {
      unsigned Cost = 0;

      // The cost of extracting from the value vector and pointer vector.
      Type *PtrTy = ToVectorTy(I->getOperand(0)->getType(), VF);
      for (unsigned i = 0; i < VF; ++i) {
        Cost += TTI.getVectorInstrCost(Instruction::ExtractElement, VectorTy,
                                       i);
        Cost += TTI.getVectorInstrCost(Instruction::ExtractElement, PtrTy, i);
      }

      // The cost of the scalar stores.
      Cost += VF * TTI.getMemoryOpCost(I->getOpcode(), ValTy->getScalarType(),
                                       SI->getAlignment(),
                                       SI->getPointerAddressSpace());
      return Cost;
    }

    // Wide stores.
    unsigned Cost = TTI.getMemoryOpCost(I->getOpcode(), VectorTy,
                                        SI->getAlignment(),
                                        SI->getPointerAddressSpace());
    if (Reverse)
      Cost += TTI.getShuffleCost(TargetTransformInfo::SK_Reverse,
                                  VectorTy, 0);
    return Cost;
  }
  case Instruction::Load: {
    LoadInst *LI = cast<LoadInst>(I);

    if (VF == 1)
      return TTI.getMemoryOpCost(I->getOpcode(), VectorTy, LI->getAlignment(),
                                 LI->getPointerAddressSpace());

    // Scalarized loads.
    int Stride = Legal->isConsecutivePtr(LI->getPointerOperand());
    bool Reverse = Stride < 0;
    if (0 == Stride) {
      unsigned Cost = 0;
      Type *PtrTy = ToVectorTy(I->getOperand(0)->getType(), VF);

      // The cost of extracting from the pointer vector.
      for (unsigned i = 0; i < VF; ++i)
        Cost += TTI.getVectorInstrCost(Instruction::ExtractElement, PtrTy, i);

      // The cost of inserting data to the result vector.
      for (unsigned i = 0; i < VF; ++i)
        Cost += TTI.getVectorInstrCost(Instruction::InsertElement, VectorTy, i);

      // The cost of the scalar stores.
      Cost += VF * TTI.getMemoryOpCost(I->getOpcode(), RetTy->getScalarType(),
                                       LI->getAlignment(),
                                       LI->getPointerAddressSpace());
      return Cost;
    }

    // Wide loads.
    unsigned Cost = TTI.getMemoryOpCost(I->getOpcode(), VectorTy,
                                        LI->getAlignment(),
                                        LI->getPointerAddressSpace());
    if (Reverse)
      Cost += TTI.getShuffleCost(TargetTransformInfo::SK_Reverse, VectorTy, 0);
    return Cost;
  }
  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::FPExt:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::SIToFP:
  case Instruction::UIToFP:
  case Instruction::Trunc:
  case Instruction::FPTrunc:
  case Instruction::BitCast: {
    // We optimize the truncation of induction variable.
    // The cost of these is the same as the scalar operation.
    if (I->getOpcode() == Instruction::Trunc &&
        Legal->isInductionVariable(I->getOperand(0)))
      return TTI.getCastInstrCost(I->getOpcode(), I->getType(),
                                  I->getOperand(0)->getType());

    Type *SrcVecTy = ToVectorTy(I->getOperand(0)->getType(), VF);
    return TTI.getCastInstrCost(I->getOpcode(), VectorTy, SrcVecTy);
  }
  case Instruction::Call: {
    assert(isTriviallyVectorizableIntrinsic(I));
    IntrinsicInst *II = cast<IntrinsicInst>(I);
    Type *RetTy = ToVectorTy(II->getType(), VF);
    SmallVector<Type*, 4> Tys;
    for (unsigned i = 0, ie = II->getNumArgOperands(); i != ie; ++i)
      Tys.push_back(ToVectorTy(II->getArgOperand(i)->getType(), VF));
    return TTI.getIntrinsicInstrCost(II->getIntrinsicID(), RetTy, Tys);
  }
  default: {
    // We are scalarizing the instruction. Return the cost of the scalar
    // instruction, plus the cost of insert and extract into vector
    // elements, times the vector width.
    unsigned Cost = 0;

    if (!RetTy->isVoidTy() && VF != 1) {
      unsigned InsCost = TTI.getVectorInstrCost(Instruction::InsertElement,
                                                VectorTy);
      unsigned ExtCost = TTI.getVectorInstrCost(Instruction::ExtractElement,
                                                VectorTy);

      // The cost of inserting the results plus extracting each one of the
      // operands.
      Cost += VF * (InsCost + ExtCost * I->getNumOperands());
    }

    // The cost of executing VF copies of the scalar instruction. This opcode
    // is unknown. Assume that it is the same as 'mul'.
    Cost += VF * TTI.getArithmeticInstrCost(Instruction::Mul, VectorTy);
    return Cost;
  }
  }// end of switch.
}

Type* LoopVectorizationCostModel::ToVectorTy(Type *Scalar, unsigned VF) {
  if (Scalar->isVoidTy() || VF == 1)
    return Scalar;
  return VectorType::get(Scalar, VF);
}

char LoopVectorize::ID = 0;
static const char lv_name[] = "Loop Vectorization";
INITIALIZE_PASS_BEGIN(LoopVectorize, LV_NAME, lv_name, false, false)
INITIALIZE_AG_DEPENDENCY(AliasAnalysis)
INITIALIZE_AG_DEPENDENCY(TargetTransformInfo)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_END(LoopVectorize, LV_NAME, lv_name, false, false)

namespace llvm {
  Pass *createLoopVectorizePass() {
    return new LoopVectorize();
  }
}


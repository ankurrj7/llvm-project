//===--- CodeGenPGO.cpp - PGO Instrumentation for LLVM CodeGen --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Instrumentation-based profile-guided optimization
//
//===----------------------------------------------------------------------===//

#include "CodeGenPGO.h"
#include "CGCXXABI.h"
#include "CGDebugInfo.h"
#include "CodeGenFunction.h"
#include "CoverageMappingGen.h"
#include "clang/AST/Attr.h"
#include "clang/AST/EvaluatedExprVisitor.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Basic/DiagnosticFrontend.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/SaveAndRestore.h"
#include <limits>
#include <optional>

namespace llvm {
extern cl::opt<bool> EnableSingleByteCoverage;
} // namespace llvm

static llvm::cl::opt<bool>
    EnableValueProfiling("enable-value-profiling",
                         llvm::cl::desc("Enable value profiling"),
                         llvm::cl::Hidden, llvm::cl::init(false));

using namespace clang;
using namespace CodeGen;

void clang::CodeGen::visitVLATypeEvaluations(
    QualType QTy, ASTContext &Context,
    llvm::function_ref<void(const VLATypeEvaluation &)> Visit) {
  assert(QTy->isVariablyModifiedType() && "expected a variably-modified type");

  do {
    assert(QTy->isVariablyModifiedType());
    const Type *Ty = QTy.getTypePtr();
    switch (Ty->getTypeClass()) {
#define TYPE(Class, Base)
#define ABSTRACT_TYPE(Class, Base)
#define NON_CANONICAL_TYPE(Class, Base)
#define DEPENDENT_TYPE(Class, Base) case clang::Type::Class:
#define NON_CANONICAL_UNLESS_DEPENDENT_TYPE(Class, Base)
#include "clang/AST/TypeNodes.inc"
      llvm_unreachable("unexpected dependent type");

    case clang::Type::Builtin:
    case clang::Type::Complex:
    case clang::Type::Vector:
    case clang::Type::ExtVector:
    case clang::Type::ConstantMatrix:
    case clang::Type::Record:
    case clang::Type::Enum:
    case clang::Type::Using:
    case clang::Type::TemplateSpecialization:
    case clang::Type::ObjCTypeParam:
    case clang::Type::ObjCObject:
    case clang::Type::ObjCInterface:
    case clang::Type::ObjCObjectPointer:
    case clang::Type::BitInt:
    case clang::Type::HLSLInlineSpirv:
    case clang::Type::PredefinedSugar:
      llvm_unreachable("type class is never variably-modified");

    case clang::Type::Adjusted:
      QTy = cast<AdjustedType>(Ty)->getAdjustedType();
      break;
    case clang::Type::Decayed:
      QTy = cast<DecayedType>(Ty)->getPointeeType();
      break;
    case clang::Type::Pointer:
      QTy = cast<PointerType>(Ty)->getPointeeType();
      break;
    case clang::Type::BlockPointer:
      QTy = cast<BlockPointerType>(Ty)->getPointeeType();
      break;
    case clang::Type::LValueReference:
    case clang::Type::RValueReference:
      QTy = cast<ReferenceType>(Ty)->getPointeeType();
      break;
    case clang::Type::MemberPointer:
      QTy = cast<MemberPointerType>(Ty)->getPointeeType();
      break;

    case clang::Type::ArrayParameter:
    case clang::Type::ConstantArray:
    case clang::Type::IncompleteArray:
      QTy = cast<ArrayType>(Ty)->getElementType();
      break;
    case clang::Type::VariableArray: {
      const auto *VAT = cast<VariableArrayType>(Ty);
      if (const Expr *Size = VAT->getSizeExpr())
        Visit({VLATypeEvaluationKind::ArrayBound, Size});
      QTy = VAT->getElementType();
      break;
    }

    case clang::Type::FunctionProto:
    case clang::Type::FunctionNoProto:
      QTy = cast<FunctionType>(Ty)->getReturnType();
      break;

    case clang::Type::Paren:
    case clang::Type::TypeOf:
    case clang::Type::UnaryTransform:
    case clang::Type::Attributed:
    case clang::Type::BTFTagAttributed:
    case clang::Type::OverflowBehavior:
    case clang::Type::HLSLAttributedResource:
    case clang::Type::SubstTemplateTypeParm:
    case clang::Type::MacroQualified:
    case clang::Type::CountAttributed:
      QTy = QTy.getSingleStepDesugaredType(Context);
      break;

    case clang::Type::Typedef:
    case clang::Type::Decltype:
    case clang::Type::Auto:
    case clang::Type::DeducedTemplateSpecialization:
    case clang::Type::PackIndexing:
      return;

    case clang::Type::TypeOfExpr: {
      const Expr *Underlying = cast<TypeOfExprType>(Ty)->getUnderlyingExpr();
      Visit({VLATypeEvaluationKind::TypeOfExpression, Underlying});
      return;
    }

    case clang::Type::Atomic:
      QTy = cast<AtomicType>(Ty)->getValueType();
      break;
    case clang::Type::Pipe:
      QTy = cast<PipeType>(Ty)->getElementType();
      break;
    }
  } while (QTy->isVariablyModifiedType());
}

void clang::CodeGen::visitCallContinuationCallChildren(
    const CallExpr *E, bool ReverseDefaultArgs,
    llvm::function_ref<void(const Stmt *)> Visit) {
  SmallVector<const Stmt *, 8> Children(E->children());
  assert(Children.size() >= E->getNumArgs());
  unsigned PrefixSize = Children.size() - E->getNumArgs();
  for (const Stmt *Child : ArrayRef(Children).take_front(PrefixSize))
    if (Child)
      Visit(Child);

  SmallVector<const Expr *, 8> Args(E->arguments());
  const auto *OperatorCall = dyn_cast<CXXOperatorCallExpr>(E);
  const auto *Method =
      OperatorCall
          ? dyn_cast_if_present<CXXMethodDecl>(OperatorCall->getCalleeDecl())
          : nullptr;

  if (OperatorCall && OperatorCall->isAssignmentOp() && Method &&
      Method->isImplicitObjectMemberFunction()) {
    for (const Expr *Arg : llvm::reverse(llvm::drop_begin(Args)))
      Visit(Arg);
    if (!Args.empty())
      Visit(Args.front());
    return;
  }

  // IR generation always forms an implicit member operator's object before
  // its explicit arguments. Static operators have the same source-level
  // object slot in CXXOperatorCallExpr and emit it first as well. Only the
  // remaining explicit arguments use the ABI-default or forced operator order.
  if (OperatorCall && Method &&
      (Method->isImplicitObjectMemberFunction() || Method->isStatic()) &&
      !Args.empty()) {
    Visit(Args.front());
    Args.erase(Args.begin());
  }

  bool ReverseArgs = ReverseDefaultArgs;
  if (OperatorCall) {
    if (OperatorCall->isAssignmentOp())
      ReverseArgs = true;
    else
      switch (OperatorCall->getOperator()) {
      case OO_LessLess:
      case OO_GreaterGreater:
      case OO_AmpAmp:
      case OO_PipePipe:
      case OO_Comma:
      case OO_ArrowStar:
        ReverseArgs = false;
        break;
      default:
        break;
      }
  }

  if (ReverseArgs)
    for (const Expr *Arg : llvm::reverse(Args))
      Visit(Arg);
  else
    for (const Expr *Arg : Args)
      Visit(Arg);
}

void clang::CodeGen::visitCallContinuationAtomicExprChildren(
    const AtomicExpr *E, llvm::function_ref<void(const Stmt *)> Visit) {
  Visit(E->getPtr());

  if (E->getOp() == AtomicExpr::AO__c11_atomic_init ||
      E->getOp() == AtomicExpr::AO__opencl_atomic_init) {
    Visit(E->getVal1());
    return;
  }

  Visit(E->getOrder());
  if (E->getScopeModel())
    Visit(E->getScope());

  if (!E->hasVal1Operand())
    return;
  Visit(E->getVal1());

  if (E->getOp() == AtomicExpr::AO__atomic_exchange ||
      E->getOp() == AtomicExpr::AO__scoped_atomic_exchange || E->isCmpXChg())
    Visit(E->getVal2());

  if (!E->isCmpXChg())
    return;
  Visit(E->getOrderFail());
  if (E->getOp() == AtomicExpr::AO__atomic_compare_exchange ||
      E->getOp() == AtomicExpr::AO__atomic_compare_exchange_n ||
      E->getOp() == AtomicExpr::AO__scoped_atomic_compare_exchange ||
      E->getOp() == AtomicExpr::AO__scoped_atomic_compare_exchange_n)
    Visit(E->getWeak());
}

/// Whether \p E can denote storage within a __block variable. Keep this in the
/// shared ordering helper so aggregate emission, counter allocation, and
/// coverage mapping cannot disagree about the special RHS-first lowering.
static bool isBlockVarRef(const Expr *E) {
  E = E->IgnoreParens();

  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
    return VD && VD->hasAttr<BlocksAttr>();
  }

  if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->isAssignmentOp() || BO->isPtrMemOp())
      return isBlockVarRef(BO->getLHS());
    if (BO->getOpcode() == BO_Comma)
      return isBlockVarRef(BO->getRHS());
    return false;
  }
  if (const auto *ACO = dyn_cast<AbstractConditionalOperator>(E))
    return isBlockVarRef(ACO->getTrueExpr()) ||
           isBlockVarRef(ACO->getFalseExpr());
  if (const auto *OVE = dyn_cast<OpaqueValueExpr>(E))
    return OVE->getSourceExpr() && isBlockVarRef(OVE->getSourceExpr());
  if (const auto *Cast = dyn_cast<CastExpr>(E)) {
    if (Cast->getCastKind() == CK_LValueToRValue)
      return false;
    return isBlockVarRef(Cast->getSubExpr());
  }
  if (const auto *UO = dyn_cast<UnaryOperator>(E))
    return isBlockVarRef(UO->getSubExpr());
  if (const auto *ME = dyn_cast<MemberExpr>(E))
    return isBlockVarRef(ME->getBase());
  if (const auto *ASE = dyn_cast<ArraySubscriptExpr>(E))
    return isBlockVarRef(ASE->getBase());
  return false;
}

bool clang::CodeGen::callContinuationAssignmentEvaluatesRHSFirst(
    const BinaryOperator *E, const ASTContext &Context) {
  QualType LHSType = E->getLHS()->getType();
  QualType LHSValueType = LHSType;
  if (const auto *Atomic = LHSType->getAs<AtomicType>())
    LHSValueType = Atomic->getValueType();
  // Aggregate assignment is emitted directly into an already-computed
  // destination, except for a side-effecting assignment through __block
  // storage: that fragile lowering deliberately computes the RHS first.
  if (LHSValueType->isAggregateType())
    return isBlockVarRef(E->getLHS()) && E->getRHS()->HasSideEffects(Context);

  // Pointer-authenticated assignment needs the destination address to qualify
  // the value. Scalar, complex, ARC, and compound-assignment lowering evaluate
  // the RHS first.
  return !LHSType.getPointerAuth();
}

static bool callContinuationNeedsNonTrivialPrimitiveCopy(QualType Type) {
  switch (Type.isNonTrivialToPrimitiveCopy()) {
  case QualType::PCK_ARCStrong:
  case QualType::PCK_ARCWeak:
  case QualType::PCK_Struct:
    return true;
  case QualType::PCK_Trivial:
  case QualType::PCK_VolatileTrivial:
  case QualType::PCK_PtrAuth:
    return false;
  }
  llvm_unreachable("unknown primitive copy kind");
}

bool clang::CodeGen::callContinuationAssignmentNeedsCounter(
    const BinaryOperator *E) {
  if (!E->isAssignmentOp())
    return false;
  if (E->getLHS()->getType()->isAtomicType())
    return true;
  return E->getOpcode() == BO_Assign &&
         callContinuationNeedsNonTrivialPrimitiveCopy(E->getLHS()->getType());
}

bool clang::CodeGen::callContinuationComplexOperationNeedsCounter(
    const BinaryOperator *E) {
  if (E->isAssignmentOp() && callContinuationAssignmentNeedsCounter(E))
    return false;

  auto IsFloatingComplex = [](QualType Type) {
    const auto *Complex = Type->getAs<ComplexType>();
    return Complex && Complex->getElementType()->isFloatingType();
  };

  switch (E->getOpcode()) {
  case BO_Mul:
  case BO_MulAssign:
    return IsFloatingComplex(E->getLHS()->getType()) &&
           IsFloatingComplex(E->getRHS()->getType());
  case BO_Div:
  case BO_DivAssign:
    return IsFloatingComplex(E->getRHS()->getType());
  default:
    return false;
  }
}

bool clang::CodeGen::callContinuationOverflowOperationNeedsCounter(
    const Expr *E, const LangOptions &Opts) {
  QualType Type;
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getSubExpr()->getType()->isAtomicType())
      return false;
    switch (UO->getOpcode()) {
    case UO_Minus:
    case UO_PostInc:
    case UO_PostDec:
    case UO_PreInc:
    case UO_PreDec:
      Type = UO->getType();
      break;
    default:
      return false;
    }
  } else if (const auto *BO = dyn_cast<BinaryOperator>(E)) {
    if (BO->isAssignmentOp() && BO->getLHS()->getType()->isAtomicType())
      return false;
    switch (BO->getOpcode()) {
    case BO_Add:
    case BO_Sub:
    case BO_Mul:
      Type = BO->getType();
      break;
    case BO_AddAssign:
    case BO_SubAssign:
    case BO_MulAssign:
      Type = cast<CompoundAssignOperator>(BO)->getComputationResultType();
      break;
    default:
      return false;
    }
  } else {
    return false;
  }

  if (const auto *OBT = Type->getAs<OverflowBehaviorType>())
    return OBT->getBehaviorKind() ==
           OverflowBehaviorType::OverflowBehaviorKind::Trap;
  return Type->isSignedIntegerOrEnumerationType() &&
         Opts.getSignedOverflowBehavior() == LangOptions::SOB_Trapping;
}

bool clang::CodeGen::callContinuationBlockLiteralNeedsCounter(
    const BlockExpr *E) {
  for (const BlockDecl::Capture &Capture : E->getBlockDecl()->captures()) {
    if (Capture.isByRef())
      continue;
    if (Capture.hasCopyExpr() || callContinuationNeedsNonTrivialPrimitiveCopy(
                                     Capture.getVariable()->getType()))
      return true;
  }
  return false;
}

bool clang::CodeGen::callContinuationBlockLiteralHasCleanup(
    const BlockExpr *E) {
  return llvm::any_of(
      E->getBlockDecl()->captures(), [](const BlockDecl::Capture &Capture) {
        return !Capture.isByRef() &&
               Capture.getVariable()->getType().isDestructedType() !=
                   QualType::DK_none;
      });
}

bool clang::CodeGen::callContinuationTLSAccessNeedsCounter(const Expr *E,
                                                           CodeGenModule &) {
  const VarDecl *VD = nullptr;
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E))
    VD = dyn_cast<VarDecl>(DRE->getDecl());
  else if (const auto *ME = dyn_cast<MemberExpr>(E))
    VD = dyn_cast<VarDecl>(ME->getMemberDecl());

  if (!VD || VD->hasAttr<WeakRefAttr>() || VD->isStaticLocal() ||
      (!VD->hasLinkage() && !VD->isStaticDataMember()))
    return false;
  // Keep inline/COMDAT counter layouts source-stable across translation units.
  // A defining TU can know that TLS is constant-initialized and access it
  // directly while an extern-only TU calls a wrapper. Reserve and emit the
  // continuation for global TLS on both paths; the direct-path increment is
  // harmless and preserves one ODR profile identity.
  return VD->getTLSKind() != VarDecl::TLS_None;
}

bool clang::CodeGen::callContinuationEvaluatesVLAExtent(
    const UnaryExprOrTypeTraitExpr *E, const ASTContext &Context) {
  UnaryExprOrTypeTrait Kind = E->getKind();
  if (Kind != UETT_SizeOf && Kind != UETT_DataSizeOf && Kind != UETT_CountOf)
    return false;

  const VariableArrayType *VAT =
      Context.getAsVariableArrayType(E->getTypeOfArgument());
  if (!VAT)
    return false;

  // Match ScalarExprEmitter exactly: _Countof does not evaluate a constant
  // outer extent merely because an inner array extent is variable.
  if (Kind == UETT_CountOf && VAT->getElementType()->isArrayType())
    return !VAT->getSizeExpr()->isIntegerConstantExpr(Context);
  return true;
}

void clang::CodeGen::visitCallContinuationConstructChildren(
    const CXXConstructExpr *E, bool ReverseDefaultArgs,
    llvm::function_ref<void(const Stmt *)> Visit) {
  bool ReverseArgs = ReverseDefaultArgs && !E->isListInitialization();
  if (ReverseArgs)
    for (const Expr *Arg : llvm::reverse(E->arguments()))
      Visit(Arg);
  else
    for (const Expr *Arg : E->arguments())
      Visit(Arg);
}

void clang::CodeGen::visitCallContinuationRewrittenOperatorChildren(
    const CXXRewrittenBinaryOperator *E, bool ReverseDefaultArgs,
    llvm::function_ref<void(const Stmt *)> Visit) {
  const Expr *Inner = E->getDecomposedForm().InnerBinOp;
  if (const auto *Call = dyn_cast<CallExpr>(Inner)) {
    visitCallContinuationCallChildren(Call, ReverseDefaultArgs, Visit);
    return;
  }

  // Built-in rewritten comparisons are unusual, but their semantic inner
  // expression is still the exact expression CodeGen evaluates.
  for (const Stmt *Child : Inner->children())
    if (Child)
      Visit(Child);
}

void clang::CodeGen::visitCallContinuationObjCMessageChildren(
    const ObjCMessageExpr *E, bool ReverseDefaultArgs,
    llvm::function_ref<void(const Stmt *)> Visit) {
  if (E->getReceiverKind() == ObjCMessageExpr::Instance)
    Visit(E->getInstanceReceiver());

  if (ReverseDefaultArgs)
    for (const Expr *Arg : llvm::reverse(E->arguments()))
      Visit(Arg);
  else
    for (const Expr *Arg : E->arguments())
      Visit(Arg);
}

void clang::CodeGen::visitCallContinuationPseudoObjectSemantics(
    const PseudoObjectExpr *E, llvm::function_ref<void(const Expr *)> Visit) {
  for (const Expr *Semantic : E->semantics()) {
    const auto *OVE = dyn_cast<OpaqueValueExpr>(Semantic);
    if (!OVE) {
      Visit(Semantic);
      continue;
    }

    // This is the point at which CodeGen binds a shared opaque value and
    // evaluates its source. A unique opaque value has no binding; its source
    // is evaluated only if and when a semantic expression actually uses it.
    if (!OVE->isUnique())
      Visit(OVE->getSourceExpr());
  }
}

static bool requiresCanonicalCoverageMappingSPIName(
    llvm::GlobalValue::LinkageTypes Linkage) {
  // ODR definitions emitted in multiple translation units must retain the same
  // profile name. The name determines both the profile-counter symbol and the
  // coverage-mapping COMDAT identity, which must follow the function selected
  // by the linker. Available-externally name variables use linkonce_odr
  // linkage, so they require the same treatment.
  return llvm::GlobalValue::isLinkOnceODRLinkage(Linkage) ||
         llvm::GlobalValue::isWeakODRLinkage(Linkage) ||
         llvm::GlobalValue::isAvailableExternallyLinkage(Linkage);
}

void CodeGenPGO::setFuncName(StringRef Name,
                             llvm::GlobalValue::LinkageTypes Linkage) {
  llvm::IndexedInstrProfReader *PGOReader = CGM.getPGOReader();
  FuncName = llvm::getPGOFuncName(
      Name, Linkage, CGM.getCodeGenOpts().MainFileName,
      PGOReader ? PGOReader->getVersion() : llvm::IndexedInstrProf::Version);
  if (!CGM.getCodeGenOpts().CoverageMappingSPIKey.empty() &&
      !requiresCanonicalCoverageMappingSPIName(Linkage))
    FuncName = llvm::getPGOFuncNameWithCoverageMappingSPIUnit(
        FuncName, CGM.getCodeGenOpts().CoverageMappingSPIKey);

  // If we're generating a profile, create a variable for the name.
  if (CGM.getCodeGenOpts().hasProfileClangInstr())
    FuncNameVar =
        llvm::createPGOFuncNameVar(CGM.getModule(), Linkage, FuncName);
}

void CodeGenPGO::setFuncName(llvm::Function *Fn) {
  setFuncName(Fn->getName(), Fn->getLinkage());
  // Create PGOFuncName meta data.
  llvm::createPGOFuncNameMetadata(*Fn, FuncName);
}

/// The version of the PGO hash algorithm.
enum PGOHashVersion : unsigned {
  PGO_HASH_V1,
  PGO_HASH_V2,
  PGO_HASH_V3,
  PGO_HASH_V4,

  // Keep this set to the latest hash version.
  PGO_HASH_LATEST = PGO_HASH_V4
};

namespace {
/// Stable hasher for PGO region counters.
///
/// PGOHash produces a stable hash of a given function's control flow.
///
/// Changing the output of this hash will invalidate all previously generated
/// profiles -- i.e., don't do it.
///
/// \note  When this hash does eventually change (years?), we still need to
/// support old hashes.  We'll need to pull in the version number from the
/// profile data format and use the matching hash function.
class PGOHash {
  uint64_t Working;
  unsigned Count;
  PGOHashVersion HashVersion;
  llvm::MD5 MD5;

  static const int NumBitsPerType = 6;
  static const unsigned NumTypesPerWord = sizeof(uint64_t) * 8 / NumBitsPerType;
  static const unsigned TooBig = 1u << NumBitsPerType;

public:
  /// Hash values for AST nodes.
  ///
  /// Distinct values for AST nodes that have region counters attached.
  ///
  /// These values must be stable.  All new members must be added at the end,
  /// and no members should be removed.  Changing the enumeration value for an
  /// AST node will affect the hash of every function that contains that node.
  enum HashType : unsigned char {
    None = 0,
    LabelStmt = 1,
    WhileStmt,
    DoStmt,
    ForStmt,
    CXXForRangeStmt,
    ObjCForCollectionStmt,
    SwitchStmt,
    CaseStmt,
    DefaultStmt,
    IfStmt,
    CXXTryStmt,
    CXXCatchStmt,
    ConditionalOperator,
    BinaryOperatorLAnd,
    BinaryOperatorLOr,
    BinaryConditionalOperator,
    // The preceding values are available with PGO_HASH_V1.

    EndOfScope,
    IfThenBranch,
    IfElseBranch,
    GotoStmt,
    IndirectGotoStmt,
    BreakStmt,
    ContinueStmt,
    ReturnStmt,
    ThrowExpr,
    UnaryOperatorLNot,
    BinaryOperatorLT,
    BinaryOperatorGT,
    BinaryOperatorLE,
    BinaryOperatorGE,
    BinaryOperatorEQ,
    BinaryOperatorNE,
    // The preceding values are available since PGO_HASH_V2.

    CallContinuationCounters,
    CallContinuationCounter,
    CallContinuationConstruct,
    CallContinuationDefaultArgument,
    CallContinuationDefaultInitializer,
    CallContinuationArrayInitialization,
    CallContinuationVLAEvaluation,
    CallContinuationDeclaration,
    CallContinuationFullExpression,
    CallContinuationNewExpression,
    CallContinuationNewInitializer,
    CallContinuationDeleteExpression,
    CallContinuationDynamicCast,
    CallContinuationTypeidExpression,
    CallContinuationRewrittenOperator,
    CallContinuationObjCMessage,
    CallContinuationPseudoObject,
    CallContinuationConstructorPrologue,
    CallContinuationCoroutineSuspend,
    CallContinuationCoroutineBody,
    CallContinuationCompoundFallthrough,
    CallContinuationIfExit,
    CallContinuationLoopBody,
    CallContinuationLoopContinue,
    CallContinuationLoopBackedge,
    CallContinuationLoopExit,
    CallContinuationLayout,
    CallContinuationImplicitOperation,
    // Keep this last.  It's for the static assert that follows.
    LastHashType
  };
  static_assert(LastHashType <= TooBig, "Too many types in HashType");

  PGOHash(PGOHashVersion HashVersion)
      : Working(0), Count(0), HashVersion(HashVersion) {}
  void combine(HashType Type);
  void combineStableValue(uint32_t Value);
  uint64_t finalize();
  PGOHashVersion getHashVersion() const { return HashVersion; }
};
const int PGOHash::NumBitsPerType;
const unsigned PGOHash::NumTypesPerWord;
const unsigned PGOHash::TooBig;

/// Get the PGO hash version used in the given indexed profile.
static PGOHashVersion getPGOHashVersion(llvm::IndexedInstrProfReader *PGOReader,
                                        CodeGenModule &CGM) {
  if (PGOReader->getVersion() <= 4)
    return PGO_HASH_V1;
  if (PGOReader->getVersion() <= 5)
    return PGO_HASH_V2;
  if (PGOReader->getVersion() <= 12)
    return PGO_HASH_V3;
  return PGO_HASH_V4;
}

/// A RecursiveASTVisitor that fills a map of statements to PGO counters.
struct MapRegionCounters : public RecursiveASTVisitor<MapRegionCounters> {
  using Base = RecursiveASTVisitor<MapRegionCounters>;

  /// The next counter value to assign.
  unsigned NextCounter;
  /// The function hash.
  PGOHash Hash;
  ASTContext &Context;
  CodeGenModule &CGM;
  /// The map of statements to counters.
  llvm::DenseMap<const Stmt *, CounterPair> &CounterMap;
  /// Normal-completion counters for calls and implicit C++ operations.
  CallContinuationCounterMap *CallContinuationCounters;
  /// Exact, ordered run-time expressions evaluated by each VM-type site.
  VLATypeEvaluationMap *VLATypeEvaluations;
  /// Continuations are assigned after ordinary region counters.
  SmallVector<CallContinuationKey, 8> CallContinuations;
  /// Prevent shared or rewritten AST nodes from reserving a counter twice.
  llvm::DenseSet<CallContinuationKey> CallContinuationSet;
  /// Kinds owned by each AST node. The ordinary structural hash traversal
  /// interleaves these markers with its stable branch/scope tokens.
  llvm::DenseMap<CallContinuationOwner, uint64_t> CallContinuationKindsByOwner;
  /// Stable preorder among only nodes that own continuation counters. This is
  /// independent of source offsets and unrelated AST nodes.
  llvm::DenseMap<CallContinuationOwner, uint32_t> CallContinuationNodeOrdinal;
  uint32_t NextCallContinuationNodeOrdinal = 0;
  /// The state of MC/DC Coverage in this function.
  MCDC::State &MCDCState;
  /// Maximum number of supported MC/DC conditions in a boolean expression.
  unsigned MCDCMaxCond;
  /// Whether call-continuation coverage counters are enabled.
  bool CoverageCallContinuations;
  /// Whether the emitted function uses the swiftasynccall convention.
  bool IsSwiftAsyncFunction;
  /// Whether this constructor variant emits virtual-base initializers.
  bool EmitVirtualBaseInitializers;
  /// Whether the target's default C++ argument order is right-to-left.
  bool ReverseDefaultCallArgs;
  /// The call currently marked as musttail, if any.
  const CallExpr *MustTailCall = nullptr;
  /// The profile version.
  uint64_t ProfileVersion;
  /// Diagnostics Engine used to report warnings.
  DiagnosticsEngine &Diag;

  MapRegionCounters(PGOHashVersion HashVersion, uint64_t ProfileVersion,
                    llvm::DenseMap<const Stmt *, CounterPair> &CounterMap,
                    CallContinuationCounterMap *CallContinuationCounters,
                    VLATypeEvaluationMap *VLATypeEvaluations,
                    MCDC::State &MCDCState, unsigned MCDCMaxCond,
                    bool CoverageCallContinuations, bool IsSwiftAsyncFunction,
                    bool EmitVirtualBaseInitializers,
                    bool ReverseDefaultCallArgs, ASTContext &Context,
                    DiagnosticsEngine &Diag, CodeGenModule &CGM)
      : NextCounter(0), Hash(HashVersion), Context(Context), CGM(CGM),
        CounterMap(CounterMap),
        CallContinuationCounters(CallContinuationCounters),
        VLATypeEvaluations(VLATypeEvaluations), MCDCState(MCDCState),
        MCDCMaxCond(MCDCMaxCond),
        CoverageCallContinuations(CoverageCallContinuations),
        IsSwiftAsyncFunction(IsSwiftAsyncFunction),
        EmitVirtualBaseInitializers(EmitVirtualBaseInitializers),
        ReverseDefaultCallArgs(ReverseDefaultCallArgs),
        ProfileVersion(ProfileVersion), Diag(Diag) {}

  // Block bodies are separate functions. With continuation coverage enabled,
  // the literal itself can own a capture-construction completion counter in
  // the parent function, but its body and synthetic copy expressions remain
  // opaque here.
  bool TraverseBlockExpr(BlockExpr *BE) {
    if (!CoverageCallContinuations)
      return true;
    return WalkUpFromBlockExpr(BE);
  }
  bool TraverseLambdaExpr(LambdaExpr *LE) {
    // Traverse the captures, but not the body.
    for (auto C : zip(LE->captures(), LE->capture_inits()))
      TraverseLambdaCapture(LE, &std::get<0>(C), std::get<1>(C));
    return true;
  }
  bool TraverseCapturedStmt(CapturedStmt *CS) { return true; }

  bool TraverseInitListExpr(InitListExpr *S,
                            DataRecursionQueue *Queue = nullptr) {
    if (!CoverageCallContinuations)
      return Base::TraverseInitListExpr(S, Queue);

    // Collection and CodeGen evaluate the semantic form, which can contain
    // implicit constructors and fillers absent from the written form. Visit
    // that form exactly once so every emitted continuation receives a stable
    // structural-hash ordinal.
    InitListExpr *Semantic = S->isSemanticForm() ? S : S->getSemanticForm();
    if (!Semantic)
      Semantic = S;
    if (!WalkUpFromInitListExpr(Semantic))
      return false;
    for (Stmt *Child : Semantic->children())
      if (Child && !TraverseStmt(Child, Queue))
        return false;
    return true;
  }

  static const CallExpr *getMustTailCall(const AttributedStmt *S) {
    for (const Attr *A : S->getAttrs()) {
      if (A->getKind() != attr::MustTail)
        continue;

      const auto *R = dyn_cast<ReturnStmt>(S->getSubStmt());
      if (!R || !R->getRetValue())
        return nullptr;
      return dyn_cast<CallExpr>(R->getRetValue()->IgnoreParens());
    }
    return nullptr;
  }

  bool TraverseAttributedStmt(AttributedStmt *S) {
    const CallExpr *NewMustTailCall = getMustTailCall(S);
    if (!NewMustTailCall)
      return Base::TraverseAttributedStmt(S);

    llvm::SaveAndRestore<const CallExpr *> SaveMustTail(MustTailCall,
                                                        NewMustTailCall);
    return Base::TraverseAttributedStmt(S);
  }

  bool TraverseCXXDefaultArgExpr(CXXDefaultArgExpr *E) {
    return Base::TraverseCXXDefaultArgExpr(E);
  }

  bool TraverseCXXDefaultInitExpr(CXXDefaultInitExpr *E) {
    if (!CoverageCallContinuations)
      return Base::TraverseCXXDefaultInitExpr(E);
    if (!Base::TraverseCXXDefaultInitExpr(E))
      return false;
    return TraverseStmt(E->getExpr());
  }

  ArrayRef<const Expr *>
  getVLATypeEvaluationPlan(CallContinuationOwner Owner) const {
    if (!VLATypeEvaluations)
      return {};
    auto I = VLATypeEvaluations->find(Owner);
    if (I == VLATypeEvaluations->end())
      return {};
    return I->second;
  }

  bool traverseVLATypeEvaluationPlan(CallContinuationOwner Owner) {
    for (const Expr *Expression : getVLATypeEvaluationPlan(Owner))
      if (!TraverseStmt(const_cast<Expr *>(Expression)))
        return false;
    return true;
  }

  bool TraverseVariableArrayTypeLoc(VariableArrayTypeLoc TL,
                                    bool TraverseQualifier) {
    if (!CoverageCallContinuations)
      return Base::TraverseVariableArrayTypeLoc(TL, TraverseQualifier);
    // Runtime bounds are traversed from the recorded per-site evaluation plan.
    // Keep walking the written element type, but do not independently
    // rediscover or reorder its size expression here.
    return TraverseTypeLoc(TL.getElementLoc(), TraverseQualifier);
  }

  bool TraverseTypeOfExprTypeLoc(TypeOfExprTypeLoc TL, bool TraverseQualifier) {
    if (!CoverageCallContinuations)
      return Base::TraverseTypeOfExprTypeLoc(TL, TraverseQualifier);
    return true;
  }

  bool TraverseVarDecl(VarDecl *D) {
    if (!CoverageCallContinuations)
      return Base::TraverseVarDecl(D);
    if (!WalkUpFromVarDecl(D))
      return false;
    if (TypeSourceInfo *TSI = D->getTypeSourceInfo())
      if (!TraverseTypeLoc(TSI->getTypeLoc()))
        return false;
    if (!traverseVLATypeEvaluationPlan(D))
      return false;
    if (!isa<ParmVarDecl>(D) &&
        (!D->isCXXForRangeDecl() || shouldVisitImplicitCode()))
      return TraverseStmt(D->getInit());
    return true;
  }

  bool TraverseParmVarDecl(ParmVarDecl *D) {
    if (!CoverageCallContinuations)
      return Base::TraverseParmVarDecl(D);
    if (!WalkUpFromParmVarDecl(D))
      return false;
    if (TypeSourceInfo *TSI = D->getTypeSourceInfo())
      if (!TraverseTypeLoc(TSI->getTypeLoc()))
        return false;
    return traverseVLATypeEvaluationPlan(D);
  }

  bool TraverseTypedefDecl(TypedefDecl *D) {
    if (!CoverageCallContinuations)
      return Base::TraverseTypedefDecl(D);
    if (!WalkUpFromTypedefDecl(D) ||
        !TraverseTypeLoc(D->getTypeSourceInfo()->getTypeLoc()))
      return false;
    return traverseVLATypeEvaluationPlan(D);
  }

  bool TraverseTypeAliasDecl(TypeAliasDecl *D) {
    if (!CoverageCallContinuations)
      return Base::TraverseTypeAliasDecl(D);
    if (!WalkUpFromTypeAliasDecl(D) ||
        !TraverseTypeLoc(D->getTypeSourceInfo()->getTypeLoc()))
      return false;
    return traverseVLATypeEvaluationPlan(D);
  }

#define TRAVERSE_EXPLICIT_VLA_CAST(Class)                                      \
  bool Traverse##Class(Class *E) {                                             \
    if (getVLATypeEvaluationPlan(E).empty())                                   \
      return Base::Traverse##Class(E);                                         \
    if (!WalkUpFrom##Class(E) || !traverseVLATypeEvaluationPlan(E))            \
      return false;                                                            \
    return TraverseStmt(E->getSubExpr());                                      \
  }

  TRAVERSE_EXPLICIT_VLA_CAST(CStyleCastExpr)
  TRAVERSE_EXPLICIT_VLA_CAST(CXXFunctionalCastExpr)
  TRAVERSE_EXPLICIT_VLA_CAST(CXXAddrspaceCastExpr)
  TRAVERSE_EXPLICIT_VLA_CAST(CXXConstCastExpr)
  TRAVERSE_EXPLICIT_VLA_CAST(CXXDynamicCastExpr)
  TRAVERSE_EXPLICIT_VLA_CAST(CXXReinterpretCastExpr)
  TRAVERSE_EXPLICIT_VLA_CAST(CXXStaticCastExpr)
  TRAVERSE_EXPLICIT_VLA_CAST(BuiltinBitCastExpr)
  TRAVERSE_EXPLICIT_VLA_CAST(ObjCBridgedCastExpr)

#undef TRAVERSE_EXPLICIT_VLA_CAST

  bool TraverseCompoundLiteralExpr(CompoundLiteralExpr *E) {
    if (getVLATypeEvaluationPlan(E).empty())
      return Base::TraverseCompoundLiteralExpr(E);
    if (!WalkUpFromCompoundLiteralExpr(E) || !traverseVLATypeEvaluationPlan(E))
      return false;
    return TraverseStmt(E->getInitializer());
  }

  bool TraverseVAArgExpr(VAArgExpr *E) {
    if (getVLATypeEvaluationPlan(E).empty())
      return Base::TraverseVAArgExpr(E);
    if (!WalkUpFromVAArgExpr(E) || !TraverseStmt(E->getSubExpr()))
      return false;
    return traverseVLATypeEvaluationPlan(E);
  }

  bool TraverseUnaryExprOrTypeTraitExpr(UnaryExprOrTypeTraitExpr *E) {
    if (getVLATypeEvaluationPlan(E).empty())
      return Base::TraverseUnaryExprOrTypeTraitExpr(E);
    if (!WalkUpFromUnaryExprOrTypeTraitExpr(E))
      return false;
    return traverseVLATypeEvaluationPlan(E);
  }

  bool VisitDecl(const Decl *D) {
    switch (D->getKind()) {
    default:
      break;
    case Decl::Function:
    case Decl::CXXMethod:
    case Decl::CXXConstructor:
    case Decl::CXXDestructor:
    case Decl::CXXConversion:
    case Decl::ObjCMethod:
    case Decl::Block:
    case Decl::Captured:
      CounterMap[D->getBody()] = NextCounter++;
      break;
    }
    hashCallContinuationMarkers(D);
    return true;
  }

  /// If \p S gets a fresh counter, update the counter mappings. Return the
  /// V1 hash of \p S.
  PGOHash::HashType updateCounterMappings(Stmt *S) {
    auto Type = getHashType(PGO_HASH_V1, S);
    if (Type != PGOHash::None)
      CounterMap[S] = NextCounter++;
    return Type;
  }

  /// The following stacks are used with dataTraverseStmtPre() and
  /// dataTraverseStmtPost() to track the depth of nested logical operators in a
  /// boolean expression in a function.  The ultimate purpose is to keep track
  /// of the number of leaf-level conditions in the boolean expression so that a
  /// profile bitmap can be allocated based on that number.
  ///
  /// The stacks are also used to find error cases and notify the user.  A
  /// standard logical operator nest for a boolean expression could be in a form
  /// similar to this: "x = a && b && c && (d || f)"
  struct DecisionState {
    llvm::DenseSet<const Stmt *> Leaves; // Not BinOp
    const Expr *DecisionExpr;            // Root
    bool Split;                          // In splitting with Leaves.

    DecisionState() = delete;
    DecisionState(const Expr *E, bool Split = false)
        : DecisionExpr(E), Split(Split) {}
  };

  SmallVector<DecisionState, 1> DecisionStack;

  // Hook: dataTraverseStmtPre() is invoked prior to visiting an AST Stmt node.
  bool dataTraverseStmtPre(Stmt *S) {
    /// If MC/DC is not enabled, MCDCMaxCond will be set to 0. Do nothing.
    if (MCDCMaxCond == 0)
      return true;

    /// Mark "in splitting" when a leaf is met.
    if (!DecisionStack.empty()) {
      auto &StackTop = DecisionStack.back();
      if (!StackTop.Split) {
        if (StackTop.Leaves.contains(S)) {
          assert(!StackTop.Split);
          StackTop.Split = true;
        }
        return true;
      }

      // Split
      assert(StackTop.Split);
      assert(!StackTop.Leaves.contains(S));
    }

    if (const auto *E = dyn_cast<Expr>(S)) {
      if (const auto *BinOp =
              dyn_cast<BinaryOperator>(CodeGenFunction::stripCond(E));
          BinOp && BinOp->isLogicalOp())
        DecisionStack.emplace_back(E);
    }

    return true;
  }

  // Hook: dataTraverseStmtPost() is invoked by the AST visitor after visiting
  // an AST Stmt node.  MC/DC will use it to to signal when the top of a
  // logical operation (boolean expression) nest is encountered.
  bool dataTraverseStmtPost(Stmt *S) {
    if (DecisionStack.empty())
      return true;

    /// If MC/DC is not enabled, MCDCMaxCond will be set to 0. Do nothing.
    assert(MCDCMaxCond > 0);

    auto &StackTop = DecisionStack.back();

    if (StackTop.DecisionExpr != S) {
      if (StackTop.Leaves.contains(S)) {
        assert(StackTop.Split);
        StackTop.Split = false;
      }

      return true;
    }

    /// Allocate the entry (with Valid=false)
    auto &DecisionEntry =
        MCDCState
            .DecisionByStmt[CodeGenFunction::stripCond(StackTop.DecisionExpr)];

    /// Was the maximum number of conditions encountered?
    auto NumCond = StackTop.Leaves.size();
    if (NumCond > MCDCMaxCond) {
      Diag.Report(S->getBeginLoc(), diag::warn_pgo_condition_limit)
          << NumCond << MCDCMaxCond;
      DecisionStack.pop_back();
      return true;
    }

    // The Decision is validated.
    DecisionEntry.ID = MCDCState.DecisionByStmt.size() - 1;

    DecisionStack.pop_back();

    return true;
  }

  /// The RHS of all logical operators gets a fresh counter in order to count
  /// how many times the RHS evaluates to true or false, depending on the
  /// semantics of the operator. This is only valid for ">= v7" of the profile
  /// version so that we facilitate backward compatibility. In addition, in
  /// order to use MC/DC, count the number of total LHS and RHS conditions.
  bool VisitBinaryOperator(BinaryOperator *S) {
    if (S->isLogicalOp()) {
      if (CodeGenFunction::isInstrumentedCondition(S->getLHS())) {
        if (!DecisionStack.empty())
          DecisionStack.back().Leaves.insert(S->getLHS());
      }

      if (CodeGenFunction::isInstrumentedCondition(S->getRHS())) {
        if (ProfileVersion >= llvm::IndexedInstrProf::Version7)
          CounterMap[S->getRHS()] = NextCounter++;

        if (!DecisionStack.empty())
          DecisionStack.back().Leaves.insert(S->getRHS());
      }
    }
    return Base::VisitBinaryOperator(S);
  }

  static bool isNoReturnCall(const CallExpr *E) {
    QualType CalleeType = E->getCallee()->getType();
    return getFunctionExtInfo(*CalleeType).getNoReturn();
  }

  bool shouldEmitCXXConstructContinuation(const CXXConstructExpr *E) const {
    const CXXConstructorDecl *CD = E->getConstructor();
    if (CD->isNoReturn())
      return false;
    if (CD->isTrivial() && CD->isDefaultConstructor())
      return false;
    if (Context.getLangOpts().ElideConstructors && E->isElidable())
      return false;
    return true;
  }

  static PGOHash::HashType
  getCallContinuationHashType(CallContinuationKind Kind) {
    switch (Kind) {
    case CallContinuationKind::Call:
      return PGOHash::CallContinuationCounter;
    case CallContinuationKind::Construct:
      return PGOHash::CallContinuationConstruct;
    case CallContinuationKind::DefaultArgument:
      return PGOHash::CallContinuationDefaultArgument;
    case CallContinuationKind::DefaultInitializer:
      return PGOHash::CallContinuationDefaultInitializer;
    case CallContinuationKind::ArrayInitialization:
      return PGOHash::CallContinuationArrayInitialization;
    case CallContinuationKind::VLAEvaluation:
      return PGOHash::CallContinuationVLAEvaluation;
    case CallContinuationKind::Declaration:
      return PGOHash::CallContinuationDeclaration;
    case CallContinuationKind::FullExpression:
      return PGOHash::CallContinuationFullExpression;
    case CallContinuationKind::NewExpression:
      return PGOHash::CallContinuationNewExpression;
    case CallContinuationKind::NewInitializer:
      return PGOHash::CallContinuationNewInitializer;
    case CallContinuationKind::DeleteExpression:
      return PGOHash::CallContinuationDeleteExpression;
    case CallContinuationKind::DynamicCast:
      return PGOHash::CallContinuationDynamicCast;
    case CallContinuationKind::TypeidExpression:
      return PGOHash::CallContinuationTypeidExpression;
    case CallContinuationKind::RewrittenOperator:
      return PGOHash::CallContinuationRewrittenOperator;
    case CallContinuationKind::ObjCMessage:
      return PGOHash::CallContinuationObjCMessage;
    case CallContinuationKind::PseudoObject:
      return PGOHash::CallContinuationPseudoObject;
    case CallContinuationKind::ConstructorPrologue:
      return PGOHash::CallContinuationConstructorPrologue;
    case CallContinuationKind::CoroutineSuspend:
      return PGOHash::CallContinuationCoroutineSuspend;
    case CallContinuationKind::CoroutineBody:
      return PGOHash::CallContinuationCoroutineBody;
    case CallContinuationKind::CompoundFallthrough:
      return PGOHash::CallContinuationCompoundFallthrough;
    case CallContinuationKind::IfExit:
      return PGOHash::CallContinuationIfExit;
    case CallContinuationKind::LoopBody:
      return PGOHash::CallContinuationLoopBody;
    case CallContinuationKind::LoopContinue:
      return PGOHash::CallContinuationLoopContinue;
    case CallContinuationKind::LoopBackedge:
      return PGOHash::CallContinuationLoopBackedge;
    case CallContinuationKind::LoopExit:
      return PGOHash::CallContinuationLoopExit;
    case CallContinuationKind::Assignment:
    case CallContinuationKind::BlockLiteral:
    case CallContinuationKind::AtomicOperation:
    case CallContinuationKind::TLSAccess:
    case CallContinuationKind::ComplexOperation:
    case CallContinuationKind::OverflowOperation:
      return PGOHash::CallContinuationImplicitOperation;
    }
    llvm_unreachable("unknown call continuation kind");
  }

  void
  addCallContinuation(CallContinuationOwner Owner,
                      CallContinuationKind Kind = CallContinuationKind::Call) {
    CallContinuationKey Key(Owner, Kind);
    if (!CoverageCallContinuations || !CallContinuationCounters ||
        !CallContinuationSet.insert(Key).second)
      return;

    CallContinuations.push_back(Key);
    static_assert(llvm::to_underlying(CallContinuationKind::Last) < 64);
    CallContinuationKindsByOwner[Owner] |= uint64_t(1)
                                           << llvm::to_underlying(Kind);
  }

  void addCallContinuation(const CallExpr *S, const CallExpr *CurrentMustTail) {
    if (!isNoReturnCall(S) && S != CurrentMustTail)
      addCallContinuation(S, CallContinuationKind::Call);
  }

  void addCallContinuation(const CXXConstructExpr *S) {
    if (shouldEmitCXXConstructContinuation(S))
      addCallContinuation(S, CallContinuationKind::Construct);
  }

  // Continuation counters are emitted from codegen, so collection has to match
  // codegen's notion of evaluated calls. The normal counter walk sees the full
  // AST; this pass avoids reserving counters for calls in sizeof(f())-style
  // unevaluated contexts that will never be emitted.
  struct CollectCallContinuations
      : public ConstEvaluatedExprVisitor<CollectCallContinuations> {
    using Base = ConstEvaluatedExprVisitor<CollectCallContinuations>;

    MapRegionCounters &Owner;
    const Stmt *RootBody;
    const CallExpr *MustTailCall = nullptr;
    bool InPseudoObjectSemantics = false;
    llvm::DenseSet<const Expr *> SeenVLASizeExpressions;
    llvm::DenseSet<const VarDecl *> ConditionVariables;

    CollectCallContinuations(ASTContext &Context, MapRegionCounters &Owner,
                             const Stmt *RootBody)
        : Base(Context), Owner(Owner), RootBody(RootBody) {}

    bool shouldVisitDiscardedStmt() const { return false; }

    void recordVLATypeEvaluation(CallContinuationOwner EvaluationOwner,
                                 QualType Type) {
      if (!Type->isVariablyModifiedType())
        return;

      VLATypeEvaluationPlan Plan;
      visitVLATypeEvaluations(
          Type, Owner.Context, [&](const VLATypeEvaluation &Evaluation) {
            if (Evaluation.Kind == VLATypeEvaluationKind::ArrayBound &&
                SeenVLASizeExpressions.contains(Evaluation.Expression))
              return;

            Plan.push_back(Evaluation.Expression);
            Visit(Evaluation.Expression);
            if (Evaluation.Kind == VLATypeEvaluationKind::ArrayBound)
              SeenVLASizeExpressions.insert(Evaluation.Expression);
          });

      if (Plan.empty())
        return;
      bool Inserted = Owner.VLATypeEvaluations
                          ->try_emplace(EvaluationOwner, std::move(Plan))
                          .second;
      assert(Inserted && "VM-type site collected more than once");
      if (Inserted)
        Owner.addCallContinuation(EvaluationOwner,
                                  CallContinuationKind::VLAEvaluation);
    }

    void VisitExplicitCastExpr(const ExplicitCastExpr *S) {
      recordVLATypeEvaluation(S, S->getType());
      Visit(S->getSubExpr());
    }

    void VisitCompoundLiteralExpr(const CompoundLiteralExpr *S) {
      if (!S->isFileScope())
        recordVLATypeEvaluation(S, S->getType());
      Visit(S->getInitializer());
    }

    void VisitVAArgExpr(const VAArgExpr *S) {
      Visit(S->getSubExpr());
      recordVLATypeEvaluation(S, S->getType());
    }

    void seedParameterVLAEvaluations(const Decl *D) {
      const auto *FD = dyn_cast<FunctionDecl>(D);
      if (!FD || FD->hasAttr<NakedAttr>())
        return;

      if (const auto *MD = dyn_cast<CXXMethodDecl>(FD);
          MD && MD->getParent()->isLambda() &&
          MD->getOverloadedOperator() == OO_Call)
        for (const FieldDecl *Field : MD->getParent()->fields())
          if (Field->hasCapturedVLAType())
            SeenVLASizeExpressions.insert(
                Field->getCapturedVLAType()->getSizeExpr());

      for (const ParmVarDecl *PVD : FD->parameters()) {
        QualType Type = PVD->getOriginalType();
        if (!Type->isVariablyModifiedType())
          continue;
        visitVLATypeEvaluations(
            Type, Owner.Context, [&](const VLATypeEvaluation &Evaluation) {
              if (Evaluation.Kind == VLATypeEvaluationKind::ArrayBound)
                SeenVLASizeExpressions.insert(Evaluation.Expression);
            });
      }
    }

    static bool stmtHasLifetimeExtendedCleanup(const Stmt *S) {
      if (!S)
        return false;
      if (const auto *MTE = dyn_cast<MaterializeTemporaryExpr>(S)) {
        QualType TemporaryType = MTE->getSubExpr()->getType();
        if (MTE->getStorageDuration() == SD_Automatic &&
            (TemporaryType.isDestructedType() != QualType::DK_none ||
             MTE->getType().getObjCLifetime() == Qualifiers::OCL_Strong ||
             MTE->getType().getObjCLifetime() == Qualifiers::OCL_Weak))
          return true;
      }
      if (isa<LambdaExpr>(S))
        return false;
      if (const auto *BE = dyn_cast<BlockExpr>(S))
        return callContinuationBlockLiteralHasCleanup(BE);
      return llvm::any_of(S->children(), stmtHasLifetimeExtendedCleanup);
    }

    static bool declNeedsCallCleanup(const Decl *D, ASTContext &Context) {
      const auto *VD = dyn_cast<VarDecl>(D);
      return VD && (VD->needsDestruction(Context) != QualType::DK_none ||
                    VD->isEscapingByref() || VD->hasAttr<CleanupAttr>() ||
                    stmtHasLifetimeExtendedCleanup(VD->getInit()));
    }

    static bool stmtHasCleanupInCurrentCompound(const Stmt *S,
                                                ASTContext &Context) {
      if (const auto *DS = dyn_cast_or_null<DeclStmt>(S))
        return llvm::any_of(DS->decls(), [&](const Decl *D) {
          return declNeedsCallCleanup(D, Context);
        });

      // Labels and attributes do not introduce a cleanup scope. A declaration
      // directly beneath one is still destroyed at the enclosing compound's
      // fallthrough. Stop at nested compounds/control statements, which own
      // their own normal-completion boundary.
      if (const auto *LS = dyn_cast_or_null<LabelStmt>(S))
        return stmtHasCleanupInCurrentCompound(LS->getSubStmt(), Context);
      if (const auto *AS = dyn_cast_or_null<AttributedStmt>(S))
        return stmtHasCleanupInCurrentCompound(AS->getSubStmt(), Context);
      if (const auto *SC = dyn_cast_or_null<SwitchCase>(S))
        return stmtHasCleanupInCurrentCompound(SC->getSubStmt(), Context);
      return false;
    }

    bool compoundNeedsCallCleanup(const CompoundStmt *S) const {
      return llvm::any_of(S->body(), [&](const Stmt *Child) {
        return stmtHasCleanupInCurrentCompound(Child, Owner.Context);
      });
    }

    bool varDeclNeedsContinuation(const VarDecl *VD) const {
      if (!VD || VD->isImplicit() || VD->hasExternalStorage())
        return false;

      // A condition declaration is not complete until contextual conversion
      // and any deferred structured-binding initializers have completed.
      if (ConditionVariables.contains(VD))
        return true;

      // A local static or thread_local declaration can perform hidden guard
      // release or destructor registration after its written initializer.
      if (VD->getStorageDuration() != SD_Automatic)
        return VD->hasInit() && (!VD->hasConstantInitialization() ||
                                 VD->needsDestruction(Owner.Context) ==
                                     QualType::DK_cxx_destructor);

      // Tuple-like structured bindings synthesize get<I> calls in holding
      // variables that are not children of the written DeclStmt.
      if (const auto *DD = dyn_cast<DecompositionDecl>(VD))
        if (llvm::any_of(DD->flat_bindings(), [](const BindingDecl *Binding) {
              return Binding->getHoldingVar() != nullptr;
            }))
          return true;

      // Non-trivial C structs can require a compiler-generated default
      // initializer even though the VarDecl has no AST initializer.
      return !VD->hasInit() &&
             VD->getType().isNonTrivialToPrimitiveDefaultInitialize() ==
                 QualType::PDIK_Struct;
    }

    bool stmtContainsCallCleanup(const Stmt *S) const {
      if (!S)
        return false;
      if (const auto *DS = dyn_cast<DeclStmt>(S))
        for (const Decl *D : DS->decls())
          if (declNeedsCallCleanup(D, Owner.Context))
            return true;
      if (const auto *EWC = dyn_cast<ExprWithCleanups>(S);
          EWC && EWC->cleanupsHaveSideEffects())
        return true;
      if (isa<LambdaExpr>(S) || isa<BlockExpr>(S))
        return false;
      if (const auto *If = dyn_cast<IfStmt>(S); If && If->isConsteval()) {
        const Stmt *Executed =
            If->isNegatedConsteval() ? If->getThen() : If->getElse();
        return stmtContainsCallCleanup(Executed);
      }
      if (const auto *If = dyn_cast<IfStmt>(S); If && If->isConstexpr()) {
        if (stmtContainsCallCleanup(If->getInit()))
          return true;
        if (std::optional<const Stmt *> Executed =
                If->getNondiscardedCase(this->Context))
          return *Executed && stmtContainsCallCleanup(*Executed);
        return false;
      }
      return llvm::any_of(S->children(), [&](const Stmt *Child) {
        return stmtContainsCallCleanup(Child);
      });
    }

    void VisitAttributedStmt(const AttributedStmt *S) {
      const CallExpr *NewMustTailCall = getMustTailCall(S);
      if (!NewMustTailCall)
        return VisitStmt(S);

      llvm::SaveAndRestore<const CallExpr *> SaveMustTail(MustTailCall,
                                                          NewMustTailCall);
      Visit(S->getSubStmt());
    }

    void VisitReturnStmt(const ReturnStmt *S) {
      const CallExpr *NewMustTailCall =
          getImplicitSwiftAsyncMustTailCall(*S, Owner.IsSwiftAsyncFunction);
      if (!NewMustTailCall)
        return VisitStmt(S);

      llvm::SaveAndRestore<const CallExpr *> SaveMustTail(MustTailCall,
                                                          NewMustTailCall);
      VisitStmt(S);
    }

    void VisitCallExpr(const CallExpr *S) {
      if (S->isUnevaluatedBuiltinCall(this->Context))
        return;

      Owner.addCallContinuation(S, MustTailCall);
      visitCallContinuationCallChildren(
          S, Owner.ReverseDefaultCallArgs,
          [&](const Stmt *Child) { Visit(Child); });
    }

    void VisitCastExpr(const CastExpr *S) {
      VisitStmt(S);
      if (S->getCastKind() == CK_AtomicToNonAtomic)
        Owner.addCallContinuation(S, CallContinuationKind::AtomicOperation);
    }

    void VisitUnaryOperator(const UnaryOperator *S) {
      VisitStmt(S);
      if (S->isIncrementDecrementOp() &&
          S->getSubExpr()->getType()->isAtomicType())
        Owner.addCallContinuation(S, CallContinuationKind::AtomicOperation);
      if (callContinuationOverflowOperationNeedsCounter(
              S, Owner.CGM.getLangOpts()))
        Owner.addCallContinuation(S, CallContinuationKind::OverflowOperation);
    }

    void VisitAtomicExpr(const AtomicExpr *S) {
      visitCallContinuationAtomicExprChildren(
          S, [&](const Stmt *Child) { Visit(Child); });
      if (S->getOp() != AtomicExpr::AO__c11_atomic_init &&
          S->getOp() != AtomicExpr::AO__opencl_atomic_init)
        Owner.addCallContinuation(S, CallContinuationKind::AtomicOperation);
    }

    void VisitDeclRefExpr(const DeclRefExpr *S) {
      if (callContinuationTLSAccessNeedsCounter(S, Owner.CGM))
        Owner.addCallContinuation(S, CallContinuationKind::TLSAccess);
    }

    void VisitMemberExpr(const MemberExpr *S) {
      Visit(S->getBase());
      if (callContinuationTLSAccessNeedsCounter(S, Owner.CGM))
        Owner.addCallContinuation(S, CallContinuationKind::TLSAccess);
    }

    void VisitBinaryOperator(const BinaryOperator *S) {
      if (!S->isAssignmentOp()) {
        VisitStmt(S);
        if (callContinuationComplexOperationNeedsCounter(S))
          Owner.addCallContinuation(S, CallContinuationKind::ComplexOperation);
        if (callContinuationOverflowOperationNeedsCounter(
                S, Owner.CGM.getLangOpts()))
          Owner.addCallContinuation(S, CallContinuationKind::OverflowOperation);
        return;
      }
      // Match the evaluation order selected by the expression emitters.
      if (callContinuationAssignmentEvaluatesRHSFirst(S, Owner.Context)) {
        Visit(S->getRHS());
        Visit(S->getLHS());
      } else {
        Visit(S->getLHS());
        Visit(S->getRHS());
      }
      if (callContinuationAssignmentNeedsCounter(S))
        Owner.addCallContinuation(S, CallContinuationKind::Assignment);
      if (callContinuationOverflowOperationNeedsCounter(
              S, Owner.CGM.getLangOpts()))
        Owner.addCallContinuation(S, CallContinuationKind::OverflowOperation);
    }

    void VisitBlockExpr(const BlockExpr *S) {
      if (callContinuationBlockLiteralNeedsCounter(S))
        Owner.addCallContinuation(S, CallContinuationKind::BlockLiteral);
    }

    void VisitCompoundAssignOperator(const CompoundAssignOperator *S) {
      Visit(S->getRHS());
      Visit(S->getLHS());
      if (callContinuationAssignmentNeedsCounter(S))
        Owner.addCallContinuation(S, CallContinuationKind::Assignment);
      if (callContinuationComplexOperationNeedsCounter(S))
        Owner.addCallContinuation(S, CallContinuationKind::ComplexOperation);
      if (callContinuationOverflowOperationNeedsCounter(
              S, Owner.CGM.getLangOpts()))
        Owner.addCallContinuation(S, CallContinuationKind::OverflowOperation);
    }

    void VisitCXXConstructExpr(const CXXConstructExpr *S) {
      Owner.addCallContinuation(S);
      visitCallContinuationConstructChildren(
          S, Owner.ReverseDefaultCallArgs,
          [&](const Stmt *Child) { Visit(Child); });
    }

    void VisitCXXDefaultInitExpr(const CXXDefaultInitExpr *S) {
      Visit(S->getExpr());
      Owner.addCallContinuation(S, CallContinuationKind::DefaultInitializer);
    }

    void VisitCXXDefaultArgExpr(const CXXDefaultArgExpr *S) {
      // The default argument's spelling belongs to the callee declaration,
      // while this wrapper executes at the caller's use location. LLVM's
      // caller mapping intentionally treats it as opaque; use one atomic
      // completion edge rather than importing out-of-order source regions.
      Owner.addCallContinuation(S, CallContinuationKind::DefaultArgument);
    }

    void VisitArrayInitLoopExpr(const ArrayInitLoopExpr *S) {
      // The per-element expression is synthetic and executes in a generated
      // loop. Keep it out of the source counter walk and expose one completion
      // edge for the whole implicit array copy/move.
      Visit(S->getCommonExpr()->getSourceExpr());
      Owner.addCallContinuation(S, CallContinuationKind::ArrayInitialization);
    }

    void VisitUnaryExprOrTypeTraitExpr(const UnaryExprOrTypeTraitExpr *S) {
      if (!callContinuationEvaluatesVLAExtent(S, Owner.Context))
        return;
      if (S->isArgumentType())
        recordVLATypeEvaluation(S, S->getTypeOfArgument());
      else {
        Visit(S->getArgumentExpr());
        Owner.addCallContinuation(S, CallContinuationKind::VLAEvaluation);
      }
    }

    void VisitDeclStmt(const DeclStmt *S) {
      for (const Decl *D : S->decls()) {
        if (const auto *TND = dyn_cast<TypedefNameDecl>(D)) {
          recordVLATypeEvaluation(TND, TND->getUnderlyingType());
          continue;
        }
        const auto *VD = dyn_cast<VarDecl>(D);
        if (!VD || VD->hasExternalStorage())
          continue;
        recordVLATypeEvaluation(VD, VD->getType());
        if (VD->hasInit() && !VD->hasConstantInitialization())
          Visit(VD->getInit());
        if (varDeclNeedsContinuation(VD))
          Owner.addCallContinuation(VD, CallContinuationKind::Declaration);
      }
    }

    void VisitCaseStmt(const CaseStmt *S) { Visit(S->getSubStmt()); }

    // ConstantExpr is a required compile-time context. Keep it opaque so
    // constexpr calls beneath it do not acquire run-time continuations.
    void VisitConstantExpr(const ConstantExpr *) {}

    void VisitCXXRewrittenBinaryOperator(const CXXRewrittenBinaryOperator *S) {
      // The semantic operator call is compiler-generated and is intentionally
      // hidden by RecursiveASTVisitor. Visit only its evaluated operands and
      // own one completion edge on the stable written wrapper.
      visitCallContinuationRewrittenOperatorChildren(
          S, Owner.ReverseDefaultCallArgs,
          [&](const Stmt *Child) { Visit(Child); });
      Owner.addCallContinuation(S, CallContinuationKind::RewrittenOperator);
    }

    void VisitObjCMessageExpr(const ObjCMessageExpr *S) {
      visitCallContinuationObjCMessageChildren(
          S, Owner.ReverseDefaultCallArgs,
          [&](const Stmt *Child) { Visit(Child); });
      const ObjCMethodDecl *Method = S->getMethodDecl();
      if ((!Method || !Method->hasAttr<NoReturnAttr>()) &&
          !(InPseudoObjectSemantics && S->isImplicit()))
        Owner.addCallContinuation(S, CallContinuationKind::ObjCMessage);
    }

    void VisitExprWithCleanups(const ExprWithCleanups *S) {
      Visit(S->getSubExpr());
      if (S->cleanupsHaveSideEffects())
        Owner.addCallContinuation(S, CallContinuationKind::FullExpression);
    }

    void VisitCXXNewExpr(const CXXNewExpr *S) {
      if (std::optional<const Expr *> ArraySize = S->getArraySize())
        Visit(*ArraySize);

      SmallVector<const Expr *, 4> PlacementArgs(S->placement_arguments());
      if (Owner.ReverseDefaultCallArgs)
        for (const Expr *Arg : llvm::reverse(PlacementArgs))
          Visit(Arg);
      else
        for (const Expr *Arg : PlacementArgs)
          Visit(Arg);

      if (const Expr *Init = S->getInitializer()) {
        Owner.addCallContinuation(S, CallContinuationKind::NewInitializer);
        Visit(Init);
      }
      Owner.addCallContinuation(S, CallContinuationKind::NewExpression);
    }

    void VisitCXXDeleteExpr(const CXXDeleteExpr *S) {
      VisitExpr(S);
      Owner.addCallContinuation(S, CallContinuationKind::DeleteExpression);
    }

    void VisitCXXDynamicCastExpr(const CXXDynamicCastExpr *S) {
      Visit(S->getSubExpr());
      // Statically resolved reference upcasts do not call EmitDynamicCast and
      // therefore have no matching completion emission. Only a genuine
      // run-time reference cast can throw and needs a success continuation.
      if (S->getCastKind() == CK_Dynamic &&
          S->getTypeAsWritten()->isReferenceType())
        Owner.addCallContinuation(S, CallContinuationKind::DynamicCast);
    }

    void VisitCXXTypeidExpr(const CXXTypeidExpr *S) {
      if (!S->isTypeOperand() && S->isPotentiallyEvaluated())
        Visit(S->getExprOperand());
      if (!S->isTypeOperand() && S->hasNullCheck())
        Owner.addCallContinuation(S, CallContinuationKind::TypeidExpression);
    }

    void VisitPseudoObjectExpr(const PseudoObjectExpr *S) {
      llvm::SaveAndRestore<bool> SavePseudoObjectState(InPseudoObjectSemantics,
                                                       true);
      visitCallContinuationPseudoObjectSemantics(
          S, [&](const Expr *Semantic) { Visit(Semantic); });
      Owner.addCallContinuation(S, CallContinuationKind::PseudoObject);
    }

    void VisitOpaqueValueExpr(const OpaqueValueExpr *S) {
      if (S->isUnique())
        Visit(S->getSourceExpr());
    }

    void VisitCoroutineSuspendExpr(const CoroutineSuspendExpr *S) {
      // await_ready/await_suspend/await_resume are implementation details. A
      // single counter after await_resume is the source continuation.
      Visit(S->getOperand());
      if (const auto *Await = dyn_cast<CoawaitExpr>(S);
          !Await || !Await->isImplicit())
        Owner.addCallContinuation(S, CallContinuationKind::CoroutineSuspend);
    }

    void VisitCoroutineBodyStmt(const CoroutineBodyStmt *S) {
      // CoroutineBodyStmt::children() also exposes compiler-generated promise,
      // allocation, suspend, parameter-move, and deallocation nodes. They do
      // not belong to the written function mapping. Model the one observable
      // boundary after initial_suspend, then visit only the written body.
      Owner.addCallContinuation(S, CallContinuationKind::CoroutineBody);
      Visit(S->getBody());
    }

    void VisitCoreturnStmt(const CoreturnStmt *S) {
      // PromiseCall is synthetic and has no independent source region.
      if (S->getOperand())
        Visit(S->getOperand());
    }

    void VisitCompoundStmt(const CompoundStmt *S) {
      VisitStmt(S);
      if (S != RootBody && compoundNeedsCallCleanup(S))
        Owner.addCallContinuation(S, CallContinuationKind::CompoundFallthrough);
    }

    void VisitWhileStmt(const WhileStmt *S) {
      if (const VarDecl *VD = S->getConditionVariable())
        ConditionVariables.insert(VD);
      VisitStmt(S);
      if (stmtContainsCallCleanup(S)) {
        Owner.addCallContinuation(S, CallContinuationKind::LoopBackedge);
        Owner.addCallContinuation(S, CallContinuationKind::LoopExit);
      }
    }

    void VisitDoStmt(const DoStmt *S) {
      VisitStmt(S);
      if (stmtContainsCallCleanup(S)) {
        Owner.addCallContinuation(S, CallContinuationKind::LoopBackedge);
        Owner.addCallContinuation(S, CallContinuationKind::LoopExit);
      }
    }

    void VisitForStmt(const ForStmt *S) {
      if (const VarDecl *VD = S->getConditionVariable())
        ConditionVariables.insert(VD);
      VisitStmt(S);
      if (stmtContainsCallCleanup(S)) {
        if (S->getInc())
          Owner.addCallContinuation(S, CallContinuationKind::LoopContinue);
        Owner.addCallContinuation(S, CallContinuationKind::LoopBackedge);
        Owner.addCallContinuation(S, CallContinuationKind::LoopExit);
      }
    }

    void VisitCXXForRangeStmt(const CXXForRangeStmt *S) {
      // Visit the written pieces. The generated begin/end/comparison,
      // dereference, and increment calls are covered by the whole lowering
      // boundaries below rather than receiving unmapped call counters.
      if (S->getInit())
        Visit(S->getInit());
      Visit(S->getRangeStmt());
      Visit(S->getBody());
      // Range-for lowering contains implicit begin/end/comparison/increment
      // operations that the source mapping does not visit individually.
      Owner.addCallContinuation(S, CallContinuationKind::LoopBody);
      Owner.addCallContinuation(S, CallContinuationKind::LoopBackedge);
      Owner.addCallContinuation(S, CallContinuationKind::LoopExit);
    }

    void VisitSwitchStmt(const SwitchStmt *S) {
      if (const VarDecl *VD = S->getConditionVariable())
        ConditionVariables.insert(VD);
      VisitStmt(S);
    }

    void VisitIfStmt(const IfStmt *S) {
      if (S->isConsteval()) {
        const Stmt *Executed =
            S->isNegatedConsteval() ? S->getThen() : S->getElse();
        if (Executed)
          Visit(Executed);
      } else if (S->isConstexpr()) {
        // The init-statement is evaluated even though one arm is discarded.
        if (const Stmt *Init = S->getInit())
          Visit(Init);
        if (std::optional<const Stmt *> Executed =
                S->getNondiscardedCase(this->Context))
          if (*Executed)
            Visit(*Executed);
      } else {
        if (const VarDecl *VD = S->getConditionVariable())
          ConditionVariables.insert(VD);
        Base::VisitIfStmt(S);
      }
      if (stmtContainsCallCleanup(S))
        Owner.addCallContinuation(S, CallContinuationKind::IfExit);
    }
  };

  void collectCallContinuations(const Decl *D) {
    if (!CoverageCallContinuations || !CallContinuationCounters)
      return;

    CollectCallContinuations Collector(Context, *this, D->getBody());
    Collector.seedParameterVLAEvaluations(D);
    if (const auto *Ctor = dyn_cast_or_null<CXXConstructorDecl>(D)) {
      for (const CXXCtorInitializer *Initializer : Ctor->inits())
        if ((Initializer->isWritten() ||
             Initializer->isInClassMemberInitializer()) &&
            (!Initializer->isBaseInitializer() || EmitVirtualBaseInitializers ||
             !Initializer->isBaseVirtual()))
          Collector.Visit(Initializer->getInit());
      addCallContinuation(Ctor->getBody(),
                          CallContinuationKind::ConstructorPrologue);
      if (!Ctor->isDefaulted())
        Collector.Visit(Ctor->getBody());
    } else if (const auto *FD = dyn_cast_or_null<FunctionDecl>(D)) {
      if (const auto *Method = dyn_cast<CXXMethodDecl>(FD);
          !Method || !Method->isDefaulted())
        Collector.Visit(FD->getBody());
    } else if (const auto *MD = dyn_cast_or_null<ObjCMethodDecl>(D))
      Collector.Visit(MD->getBody());
    else if (const auto *BD = dyn_cast_or_null<BlockDecl>(D))
      Collector.Visit(BD->getBody());
    else if (const auto *CD = dyn_cast_or_null<CapturedDecl>(D))
      Collector.Visit(CD->getBody());
  }

  void assignCallContinuationCounters() {
    if (!CallContinuationCounters)
      return;

    for (CallContinuationKey Key : CallContinuations)
      (*CallContinuationCounters)[Key] = NextCounter++;
  }

  void hashCallContinuationLayout() {
    if (CallContinuations.empty())
      return;

    // The structural traversal above records where each continuation-owning
    // node occurs. Encode the actual counter allocation order as well: C++
    // evaluation order can differ from AST preorder (assignment, ABI argument
    // order, and parenthesized versus list construction).
    Hash.combine(PGOHash::CallContinuationLayout);
    Hash.combineStableValue(static_cast<uint32_t>(CallContinuations.size()));
    for (const auto &[Owner, Kind] : CallContinuations) {
      auto I = CallContinuationNodeOrdinal.find(Owner);
      assert(I != CallContinuationNodeOrdinal.end() &&
             "continuation node absent from structural hash traversal");
      Hash.combineStableValue(I == CallContinuationNodeOrdinal.end()
                                  ? std::numeric_limits<uint32_t>::max()
                                  : I->second);
      Hash.combineStableValue(llvm::to_underlying(Kind));
    }
  }

  /// Hash the typed markers owned by \p Owner without traversing its children.
  /// This is also used for synthetic bodies of explicitly defaulted
  /// constructors: RecursiveASTVisitor intentionally skips those bodies, but
  /// the constructor-prologue continuation still needs a stable owner.
  void hashCallContinuationMarkers(CallContinuationOwner Owner) {
    if (auto I = CallContinuationKindsByOwner.find(Owner);
        I != CallContinuationKindsByOwner.end()) {
      bool Inserted = CallContinuationNodeOrdinal
                          .try_emplace(Owner, NextCallContinuationNodeOrdinal)
                          .second;
      if (!Inserted)
        return;
      ++NextCallContinuationNodeOrdinal;
      uint64_t Kinds = I->second;
      for (unsigned Kind = 0;
           Kind <= llvm::to_underlying(CallContinuationKind::Last); ++Kind)
        if (Kinds & (uint64_t(1) << Kind))
          Hash.combine(getCallContinuationHashType(
              static_cast<CallContinuationKind>(Kind)));
    }
  }

  /// Include \p S in the function hash.
  bool VisitStmt(Stmt *S) {
    auto Type = updateCounterMappings(S);
    if (Hash.getHashVersion() != PGO_HASH_V1)
      Type = getHashType(Hash.getHashVersion(), S);
    if (Type != PGOHash::None)
      Hash.combine(Type);
    hashCallContinuationMarkers(S);
    return true;
  }

  bool TraverseIfStmt(IfStmt *If) {
    // If we used the V1 hash, use the default traversal.
    if (Hash.getHashVersion() == PGO_HASH_V1)
      return Base::TraverseIfStmt(If);

    // Otherwise, keep track of which branch we're in while traversing.
    VisitStmt(If);

    for (Stmt *CS : If->children()) {
      if (!CS)
        continue;
      if (CS == If->getThen())
        Hash.combine(PGOHash::IfThenBranch);
      else if (CS == If->getElse())
        Hash.combine(PGOHash::IfElseBranch);
      TraverseStmt(CS);
    }
    Hash.combine(PGOHash::EndOfScope);
    return true;
  }

// If the statement type \p N is nestable, and its nesting impacts profile
// stability, define a custom traversal which tracks the end of the statement
// in the hash (provided we're not using the V1 hash).
#define DEFINE_NESTABLE_TRAVERSAL(N)                                           \
  bool Traverse##N(N *S) {                                                     \
    Base::Traverse##N(S);                                                      \
    if (Hash.getHashVersion() != PGO_HASH_V1)                                  \
      Hash.combine(PGOHash::EndOfScope);                                       \
    return true;                                                               \
  }

  DEFINE_NESTABLE_TRAVERSAL(WhileStmt)
  DEFINE_NESTABLE_TRAVERSAL(DoStmt)
  DEFINE_NESTABLE_TRAVERSAL(ForStmt)
  DEFINE_NESTABLE_TRAVERSAL(CXXForRangeStmt)
  DEFINE_NESTABLE_TRAVERSAL(ObjCForCollectionStmt)
  DEFINE_NESTABLE_TRAVERSAL(CXXTryStmt)
  DEFINE_NESTABLE_TRAVERSAL(CXXCatchStmt)

  /// Get version \p HashVersion of the PGO hash for \p S.
  PGOHash::HashType getHashType(PGOHashVersion HashVersion, const Stmt *S) {
    switch (S->getStmtClass()) {
    default:
      break;
    case Stmt::LabelStmtClass:
      return PGOHash::LabelStmt;
    case Stmt::WhileStmtClass:
      return PGOHash::WhileStmt;
    case Stmt::DoStmtClass:
      return PGOHash::DoStmt;
    case Stmt::ForStmtClass:
      return PGOHash::ForStmt;
    case Stmt::CXXForRangeStmtClass:
      return PGOHash::CXXForRangeStmt;
    case Stmt::ObjCForCollectionStmtClass:
      return PGOHash::ObjCForCollectionStmt;
    case Stmt::SwitchStmtClass:
      return PGOHash::SwitchStmt;
    case Stmt::CaseStmtClass:
      return PGOHash::CaseStmt;
    case Stmt::DefaultStmtClass:
      return PGOHash::DefaultStmt;
    case Stmt::IfStmtClass:
      return PGOHash::IfStmt;
    case Stmt::CXXTryStmtClass:
      return PGOHash::CXXTryStmt;
    case Stmt::CXXCatchStmtClass:
      return PGOHash::CXXCatchStmt;
    case Stmt::ConditionalOperatorClass:
      return PGOHash::ConditionalOperator;
    case Stmt::BinaryConditionalOperatorClass:
      return PGOHash::BinaryConditionalOperator;
    case Stmt::BinaryOperatorClass: {
      const BinaryOperator *BO = cast<BinaryOperator>(S);
      if (BO->getOpcode() == BO_LAnd)
        return PGOHash::BinaryOperatorLAnd;
      if (BO->getOpcode() == BO_LOr)
        return PGOHash::BinaryOperatorLOr;
      if (HashVersion >= PGO_HASH_V2) {
        switch (BO->getOpcode()) {
        default:
          break;
        case BO_LT:
          return PGOHash::BinaryOperatorLT;
        case BO_GT:
          return PGOHash::BinaryOperatorGT;
        case BO_LE:
          return PGOHash::BinaryOperatorLE;
        case BO_GE:
          return PGOHash::BinaryOperatorGE;
        case BO_EQ:
          return PGOHash::BinaryOperatorEQ;
        case BO_NE:
          return PGOHash::BinaryOperatorNE;
        }
      }
      break;
    }
    }

    if (HashVersion >= PGO_HASH_V2) {
      switch (S->getStmtClass()) {
      default:
        break;
      case Stmt::GotoStmtClass:
        return PGOHash::GotoStmt;
      case Stmt::IndirectGotoStmtClass:
        return PGOHash::IndirectGotoStmt;
      case Stmt::BreakStmtClass:
        return PGOHash::BreakStmt;
      case Stmt::ContinueStmtClass:
        return PGOHash::ContinueStmt;
      case Stmt::ReturnStmtClass:
        return PGOHash::ReturnStmt;
      case Stmt::CXXThrowExprClass:
        return PGOHash::ThrowExpr;
      case Stmt::UnaryOperatorClass: {
        const UnaryOperator *UO = cast<UnaryOperator>(S);
        if (UO->getOpcode() == UO_LNot)
          return PGOHash::UnaryOperatorLNot;
        break;
      }
      }
    }

    return PGOHash::None;
  }
};

/// A StmtVisitor that propagates the raw counts through the AST and
/// records the count at statements where the value may change.
struct ComputeRegionCounts : public ConstStmtVisitor<ComputeRegionCounts> {
  /// PGO state.
  CodeGenPGO &PGO;

  /// A flag that is set when the current count should be recorded on the
  /// next statement, such as at the exit of a loop.
  bool RecordNextStmtCount;

  /// The count at the current location in the traversal.
  uint64_t CurrentCount;

  /// The map of statements to count values.
  llvm::DenseMap<const Stmt *, uint64_t> &CountMap;

  /// BreakContinueStack - Keep counts of breaks and continues inside loops.
  struct BreakContinue {
    uint64_t BreakCount = 0;
    uint64_t ContinueCount = 0;
    BreakContinue() = default;
  };
  SmallVector<BreakContinue, 8> BreakContinueStack;

  ComputeRegionCounts(llvm::DenseMap<const Stmt *, uint64_t> &CountMap,
                      CodeGenPGO &PGO)
      : PGO(PGO), RecordNextStmtCount(false), CountMap(CountMap) {}

  void RecordStmtCount(const Stmt *S) {
    if (RecordNextStmtCount) {
      CountMap[S] = CurrentCount;
      RecordNextStmtCount = false;
    }
  }

  /// Set and return the current count.
  uint64_t setCount(uint64_t Count) {
    CurrentCount = Count;
    return Count;
  }

  void VisitStmt(const Stmt *S) {
    RecordStmtCount(S);
    for (const Stmt *Child : S->children())
      if (Child)
        this->Visit(Child);
  }

  void VisitFunctionDecl(const FunctionDecl *D) {
    // Counter tracks entry to the function body.
    uint64_t BodyCount = setCount(PGO.getRegionCount(D->getBody()));
    CountMap[D->getBody()] = BodyCount;
    Visit(D->getBody());
  }

  // Skip lambda expressions. We visit these as FunctionDecls when we're
  // generating them and aren't interested in the body when generating a
  // parent context.
  void VisitLambdaExpr(const LambdaExpr *LE) {}

  void VisitCapturedDecl(const CapturedDecl *D) {
    // Counter tracks entry to the capture body.
    uint64_t BodyCount = setCount(PGO.getRegionCount(D->getBody()));
    CountMap[D->getBody()] = BodyCount;
    Visit(D->getBody());
  }

  void VisitObjCMethodDecl(const ObjCMethodDecl *D) {
    // Counter tracks entry to the method body.
    uint64_t BodyCount = setCount(PGO.getRegionCount(D->getBody()));
    CountMap[D->getBody()] = BodyCount;
    Visit(D->getBody());
  }

  void VisitBlockDecl(const BlockDecl *D) {
    // Counter tracks entry to the block body.
    uint64_t BodyCount = setCount(PGO.getRegionCount(D->getBody()));
    CountMap[D->getBody()] = BodyCount;
    Visit(D->getBody());
  }

  void VisitReturnStmt(const ReturnStmt *S) {
    RecordStmtCount(S);
    if (S->getRetValue())
      Visit(S->getRetValue());
    CurrentCount = 0;
    RecordNextStmtCount = true;
  }

  void VisitCXXThrowExpr(const CXXThrowExpr *E) {
    RecordStmtCount(E);
    if (E->getSubExpr())
      Visit(E->getSubExpr());
    CurrentCount = 0;
    RecordNextStmtCount = true;
  }

  void VisitGotoStmt(const GotoStmt *S) {
    RecordStmtCount(S);
    CurrentCount = 0;
    RecordNextStmtCount = true;
  }

  void VisitLabelStmt(const LabelStmt *S) {
    RecordNextStmtCount = false;
    // Counter tracks the block following the label.
    uint64_t BlockCount = setCount(PGO.getRegionCount(S));
    CountMap[S] = BlockCount;
    Visit(S->getSubStmt());
  }

  void VisitBreakStmt(const BreakStmt *S) {
    RecordStmtCount(S);
    assert(!BreakContinueStack.empty() && "break not in a loop or switch!");
    BreakContinueStack.back().BreakCount += CurrentCount;
    CurrentCount = 0;
    RecordNextStmtCount = true;
  }

  void VisitContinueStmt(const ContinueStmt *S) {
    RecordStmtCount(S);
    assert(!BreakContinueStack.empty() && "continue stmt not in a loop!");
    BreakContinueStack.back().ContinueCount += CurrentCount;
    CurrentCount = 0;
    RecordNextStmtCount = true;
  }

  void VisitWhileStmt(const WhileStmt *S) {
    RecordStmtCount(S);
    uint64_t ParentCount = CurrentCount;

    BreakContinueStack.push_back(BreakContinue());
    // Visit the body region first so the break/continue adjustments can be
    // included when visiting the condition.
    uint64_t BodyCount = setCount(PGO.getRegionCount(S));
    CountMap[S->getBody()] = CurrentCount;
    Visit(S->getBody());
    uint64_t BackedgeCount = CurrentCount;

    // ...then go back and propagate counts through the condition. The count
    // at the start of the condition is the sum of the incoming edges,
    // the backedge from the end of the loop body, and the edges from
    // continue statements.
    BreakContinue BC = BreakContinueStack.pop_back_val();
    uint64_t CondCount =
        setCount(ParentCount + BackedgeCount + BC.ContinueCount);
    CountMap[S->getCond()] = CondCount;
    Visit(S->getCond());
    setCount(BC.BreakCount + CondCount - BodyCount);
    RecordNextStmtCount = true;
  }

  void VisitDoStmt(const DoStmt *S) {
    RecordStmtCount(S);
    uint64_t LoopCount = PGO.getRegionCount(S);

    BreakContinueStack.push_back(BreakContinue());
    // The count doesn't include the fallthrough from the parent scope. Add it.
    uint64_t BodyCount = setCount(LoopCount + CurrentCount);
    CountMap[S->getBody()] = BodyCount;
    Visit(S->getBody());
    uint64_t BackedgeCount = CurrentCount;

    BreakContinue BC = BreakContinueStack.pop_back_val();
    // The count at the start of the condition is equal to the count at the
    // end of the body, plus any continues.
    uint64_t CondCount = setCount(BackedgeCount + BC.ContinueCount);
    CountMap[S->getCond()] = CondCount;
    Visit(S->getCond());
    setCount(BC.BreakCount + CondCount - LoopCount);
    RecordNextStmtCount = true;
  }

  void VisitForStmt(const ForStmt *S) {
    RecordStmtCount(S);
    if (S->getInit())
      Visit(S->getInit());

    uint64_t ParentCount = CurrentCount;

    BreakContinueStack.push_back(BreakContinue());
    // Visit the body region first. (This is basically the same as a while
    // loop; see further comments in VisitWhileStmt.)
    uint64_t BodyCount = setCount(PGO.getRegionCount(S));
    CountMap[S->getBody()] = BodyCount;
    Visit(S->getBody());
    uint64_t BackedgeCount = CurrentCount;
    BreakContinue BC = BreakContinueStack.pop_back_val();

    // The increment is essentially part of the body but it needs to include
    // the count for all the continue statements.
    if (S->getInc()) {
      uint64_t IncCount = setCount(BackedgeCount + BC.ContinueCount);
      CountMap[S->getInc()] = IncCount;
      Visit(S->getInc());
    }

    // ...then go back and propagate counts through the condition.
    uint64_t CondCount =
        setCount(ParentCount + BackedgeCount + BC.ContinueCount);
    if (S->getCond()) {
      CountMap[S->getCond()] = CondCount;
      Visit(S->getCond());
    }
    setCount(BC.BreakCount + CondCount - BodyCount);
    RecordNextStmtCount = true;
  }

  void VisitCXXForRangeStmt(const CXXForRangeStmt *S) {
    RecordStmtCount(S);
    if (S->getInit())
      Visit(S->getInit());
    Visit(S->getLoopVarStmt());
    Visit(S->getRangeStmt());
    Visit(S->getBeginStmt());
    Visit(S->getEndStmt());

    uint64_t ParentCount = CurrentCount;
    BreakContinueStack.push_back(BreakContinue());
    // Visit the body region first. (This is basically the same as a while
    // loop; see further comments in VisitWhileStmt.)
    uint64_t BodyCount = setCount(PGO.getRegionCount(S));
    CountMap[S->getBody()] = BodyCount;
    Visit(S->getBody());
    uint64_t BackedgeCount = CurrentCount;
    BreakContinue BC = BreakContinueStack.pop_back_val();

    // The increment is essentially part of the body but it needs to include
    // the count for all the continue statements.
    uint64_t IncCount = setCount(BackedgeCount + BC.ContinueCount);
    CountMap[S->getInc()] = IncCount;
    Visit(S->getInc());

    // ...then go back and propagate counts through the condition.
    uint64_t CondCount =
        setCount(ParentCount + BackedgeCount + BC.ContinueCount);
    CountMap[S->getCond()] = CondCount;
    Visit(S->getCond());
    setCount(BC.BreakCount + CondCount - BodyCount);
    RecordNextStmtCount = true;
  }

  void VisitObjCForCollectionStmt(const ObjCForCollectionStmt *S) {
    RecordStmtCount(S);
    Visit(S->getElement());
    uint64_t ParentCount = CurrentCount;
    BreakContinueStack.push_back(BreakContinue());
    // Counter tracks the body of the loop.
    uint64_t BodyCount = setCount(PGO.getRegionCount(S));
    CountMap[S->getBody()] = BodyCount;
    Visit(S->getBody());
    uint64_t BackedgeCount = CurrentCount;
    BreakContinue BC = BreakContinueStack.pop_back_val();

    setCount(BC.BreakCount + ParentCount + BackedgeCount + BC.ContinueCount -
             BodyCount);
    RecordNextStmtCount = true;
  }

  void VisitSwitchStmt(const SwitchStmt *S) {
    RecordStmtCount(S);
    if (S->getInit())
      Visit(S->getInit());
    Visit(S->getCond());
    CurrentCount = 0;
    BreakContinueStack.push_back(BreakContinue());
    Visit(S->getBody());
    // If the switch is inside a loop, add the continue counts.
    BreakContinue BC = BreakContinueStack.pop_back_val();
    if (!BreakContinueStack.empty())
      BreakContinueStack.back().ContinueCount += BC.ContinueCount;
    // Counter tracks the exit block of the switch.
    setCount(PGO.getRegionCount(S));
    RecordNextStmtCount = true;
  }

  void VisitSwitchCase(const SwitchCase *S) {
    RecordNextStmtCount = false;
    // Counter for this particular case. This counts only jumps from the
    // switch header and does not include fallthrough from the case before
    // this one.
    uint64_t CaseCount = PGO.getRegionCount(S);
    setCount(CurrentCount + CaseCount);
    // We need the count without fallthrough in the mapping, so it's more useful
    // for branch probabilities.
    CountMap[S] = CaseCount;
    RecordNextStmtCount = true;
    Visit(S->getSubStmt());
  }

  void VisitIfStmt(const IfStmt *S) {
    RecordStmtCount(S);

    if (S->isConsteval()) {
      const Stmt *Stm = S->isNegatedConsteval() ? S->getThen() : S->getElse();
      if (Stm)
        Visit(Stm);
      return;
    }

    uint64_t ParentCount = CurrentCount;
    if (S->getInit())
      Visit(S->getInit());
    Visit(S->getCond());

    // Counter tracks the "then" part of an if statement. The count for
    // the "else" part, if it exists, will be calculated from this counter.
    uint64_t ThenCount = setCount(PGO.getRegionCount(S));
    CountMap[S->getThen()] = ThenCount;
    Visit(S->getThen());
    uint64_t OutCount = CurrentCount;

    uint64_t ElseCount = ParentCount - ThenCount;
    if (S->getElse()) {
      setCount(ElseCount);
      CountMap[S->getElse()] = ElseCount;
      Visit(S->getElse());
      OutCount += CurrentCount;
    } else
      OutCount += ElseCount;
    setCount(OutCount);
    RecordNextStmtCount = true;
  }

  void VisitCXXTryStmt(const CXXTryStmt *S) {
    RecordStmtCount(S);
    Visit(S->getTryBlock());
    for (unsigned I = 0, E = S->getNumHandlers(); I < E; ++I)
      Visit(S->getHandler(I));
    // Counter tracks the continuation block of the try statement.
    setCount(PGO.getRegionCount(S));
    RecordNextStmtCount = true;
  }

  void VisitCXXCatchStmt(const CXXCatchStmt *S) {
    RecordNextStmtCount = false;
    // Counter tracks the catch statement's handler block.
    uint64_t CatchCount = setCount(PGO.getRegionCount(S));
    CountMap[S] = CatchCount;
    Visit(S->getHandlerBlock());
  }

  void VisitAbstractConditionalOperator(const AbstractConditionalOperator *E) {
    RecordStmtCount(E);
    uint64_t ParentCount = CurrentCount;
    Visit(E->getCond());

    // Counter tracks the "true" part of a conditional operator. The
    // count in the "false" part will be calculated from this counter.
    uint64_t TrueCount = setCount(PGO.getRegionCount(E));
    CountMap[E->getTrueExpr()] = TrueCount;
    Visit(E->getTrueExpr());
    uint64_t OutCount = CurrentCount;

    uint64_t FalseCount = setCount(ParentCount - TrueCount);
    CountMap[E->getFalseExpr()] = FalseCount;
    Visit(E->getFalseExpr());
    OutCount += CurrentCount;

    setCount(OutCount);
    RecordNextStmtCount = true;
  }

  void VisitBinLAnd(const BinaryOperator *E) {
    RecordStmtCount(E);
    uint64_t ParentCount = CurrentCount;
    Visit(E->getLHS());
    // Counter tracks the right hand side of a logical and operator.
    uint64_t RHSCount = setCount(PGO.getRegionCount(E));
    CountMap[E->getRHS()] = RHSCount;
    Visit(E->getRHS());
    setCount(ParentCount + RHSCount - CurrentCount);
    RecordNextStmtCount = true;
  }

  void VisitBinLOr(const BinaryOperator *E) {
    RecordStmtCount(E);
    uint64_t ParentCount = CurrentCount;
    Visit(E->getLHS());
    // Counter tracks the right hand side of a logical or operator.
    uint64_t RHSCount = setCount(PGO.getRegionCount(E));
    CountMap[E->getRHS()] = RHSCount;
    Visit(E->getRHS());
    setCount(ParentCount + RHSCount - CurrentCount);
    RecordNextStmtCount = true;
  }
};
} // end anonymous namespace

void PGOHash::combine(HashType Type) {
  // Check that we never combine 0 and only have six bits.
  assert(Type && "Hash is invalid: unexpected type 0");
  assert(unsigned(Type) < TooBig && "Hash is invalid: too many types");

  // Pass through MD5 if enough work has built up.
  if (Count && Count % NumTypesPerWord == 0) {
    using namespace llvm::support;
    uint64_t Swapped =
        endian::byte_swap<uint64_t>(Working, llvm::endianness::little);
    MD5.update(llvm::ArrayRef((uint8_t *)&Swapped, sizeof(Swapped)));
    Working = 0;
  }

  // Accumulate the current type.
  ++Count;
  Working = Working << NumBitsPerType | Type;
}

void PGOHash::combineStableValue(uint32_t Value) {
  // Zero is reserved by HashType. Encode seven fixed five-bit chunks so every
  // uint32_t has a unique, source-independent token sequence.
  for (unsigned Shift = 0; Shift < 35; Shift += 5)
    combine(static_cast<HashType>(((Value >> Shift) & 0x1f) + 1));
}

uint64_t PGOHash::finalize() {
  // Use Working as the hash directly if we never used MD5.
  if (Count <= NumTypesPerWord)
    // No need to byte swap here, since none of the math was endian-dependent.
    // This number will be byte-swapped as required on endianness transitions,
    // so we will see the same value on the other side.
    return Working;

  // Check for remaining work in Working.
  if (Working) {
    // Keep the buggy behavior from v1 and v2 for backward-compatibility. This
    // is buggy because it converts a uint64_t into an array of uint8_t.
    if (HashVersion < PGO_HASH_V3) {
      MD5.update({(uint8_t)Working});
    } else {
      using namespace llvm::support;
      uint64_t Swapped =
          endian::byte_swap<uint64_t>(Working, llvm::endianness::little);
      MD5.update(llvm::ArrayRef((uint8_t *)&Swapped, sizeof(Swapped)));
    }
  }

  // Finalize the MD5 and return the hash.
  llvm::MD5::MD5Result Result;
  MD5.final(Result);
  return Result.low();
}

void CodeGenPGO::assignRegionCounters(GlobalDecl GD, llvm::Function *Fn) {
  const Decl *D = GD.getDecl();
  if (!D->hasBody())
    return;

  // Skip CUDA/HIP kernel launch stub functions.
  if (CGM.getLangOpts().CUDA && !CGM.getLangOpts().CUDAIsDevice &&
      D->hasAttr<CUDAGlobalAttr>())
    return;

  bool InstrumentRegions = CGM.getCodeGenOpts().hasProfileClangInstr();
  llvm::IndexedInstrProfReader *PGOReader = CGM.getPGOReader();
  if (!InstrumentRegions && !PGOReader)
    return;
  if (D->isImplicit())
    return;
  // Constructors and destructors may be represented by several functions in IR.
  // If so, instrument only base variant, others are implemented by delegation
  // to the base one, it would be counted twice otherwise.
  if (CGM.getTarget().getCXXABI().hasConstructorVariants()) {
    if (const auto *CCD = dyn_cast<CXXConstructorDecl>(D))
      if (GD.getCtorType() != Ctor_Base &&
          CodeGenFunction::IsConstructorDelegationValid(CCD))
        return;
  }
  if (isa<CXXDestructorDecl>(D) && GD.getDtorType() != Dtor_Base)
    return;

  CGM.ClearUnusedCoverageMapping(D);
  if (Fn->hasFnAttribute(llvm::Attribute::NoProfile))
    return;
  if (Fn->hasFnAttribute(llvm::Attribute::SkipProfile))
    return;

  SourceManager &SM = CGM.getContext().getSourceManager();
  if (!llvm::coverage::SystemHeadersCoverage &&
      SM.isInSystemHeader(D->getLocation()))
    return;

  setFuncName(Fn);

  mapRegionCounters(GD, Fn->getCallingConv() == llvm::CallingConv::SwiftTail);
  if (CGM.getCodeGenOpts().CoverageMapping)
    emitCounterRegionMapping(GD);
  if (PGOReader) {
    loadRegionCounts(PGOReader, SM.isInMainFile(D->getLocation()));
    computeRegionCounts(D);
    applyFunctionAttributes(PGOReader, Fn);
  }
}

void CodeGenPGO::mapRegionCounters(GlobalDecl GD, bool IsSwiftAsyncFunction) {
  const Decl *D = GD.getDecl();
  // Use the latest hash version when inserting instrumentation, but use the
  // version in the indexed profile if we're reading PGO data.
  PGOHashVersion HashVersion = PGO_HASH_LATEST;
  uint64_t ProfileVersion = llvm::IndexedInstrProf::Version;
  if (auto *PGOReader = CGM.getPGOReader()) {
    HashVersion = getPGOHashVersion(PGOReader, CGM);
    ProfileVersion = PGOReader->getVersion();
  }

  // If MC/DC is enabled, set the MaxConditions to a preset value. Otherwise,
  // set it to zero. This value impacts the number of conditions accepted in a
  // given boolean expression, which impacts the size of the bitmap used to
  // track test vector execution for that boolean expression.  Because the
  // bitmap scales exponentially (2^n) based on the number of conditions seen,
  // the maximum value is hard-coded at 6 conditions, which is more than enough
  // for most embedded applications. Setting a maximum value prevents the
  // bitmap footprint from growing too large without the user's knowledge. In
  // the future, this value could be adjusted with a command-line option.
  unsigned MCDCMaxConditions =
      (CGM.getCodeGenOpts().MCDCCoverage ? CGM.getCodeGenOpts().MCDCMaxConds
                                         : 0);
  bool CoverageCallContinuations =
      CGM.getCodeGenOpts().CoverageMapping &&
      CGM.getCodeGenOpts().CoverageCallContinuations;
  bool EmitVirtualBaseInitializers = true;
  if (isa<CXXConstructorDecl>(D) &&
      CGM.getTarget().getCXXABI().hasConstructorVariants())
    EmitVirtualBaseInitializers = GD.getCtorType() != Ctor_Base;

  RegionCounterMap.reset(new llvm::DenseMap<const Stmt *, CounterPair>);
  if (CoverageCallContinuations)
    CallContinuationCounters.reset(new CallContinuationCounterMap);
  else
    CallContinuationCounters.reset();
  if (CoverageCallContinuations)
    VLATypeEvaluations.reset(new VLATypeEvaluationMap);
  else
    VLATypeEvaluations.reset();
  RegionMCDCState.reset(new MCDC::State);
  MapRegionCounters Walker(
      HashVersion, ProfileVersion, *RegionCounterMap,
      CallContinuationCounters.get(), VLATypeEvaluations.get(),
      *RegionMCDCState, MCDCMaxConditions, CoverageCallContinuations,
      IsSwiftAsyncFunction, EmitVirtualBaseInitializers,
      CGM.getTarget().getCXXABI().areArgsDestroyedLeftToRightInCallee(),
      CGM.getContext(), CGM.getDiags(), CGM);
  Walker.collectCallContinuations(D);
  if (const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(D))
    Walker.TraverseDecl(const_cast<FunctionDecl *>(FD));
  else if (const ObjCMethodDecl *MD = dyn_cast_or_null<ObjCMethodDecl>(D))
    Walker.TraverseDecl(const_cast<ObjCMethodDecl *>(MD));
  else if (const BlockDecl *BD = dyn_cast_or_null<BlockDecl>(D))
    Walker.TraverseDecl(const_cast<BlockDecl *>(BD));
  else if (const CapturedDecl *CD = dyn_cast_or_null<CapturedDecl>(D))
    Walker.TraverseDecl(const_cast<CapturedDecl *>(CD));
  // RecursiveASTVisitor intentionally skips in-class member initializers when
  // implicit-code traversal is disabled. Coverage emits those initializers,
  // so include their ordinary counters and structural continuation hash too.
  if (CoverageCallContinuations) {
    if (const auto *Ctor = dyn_cast_or_null<CXXConstructorDecl>(D)) {
      for (const CXXCtorInitializer *Initializer : Ctor->inits())
        if (!Initializer->isWritten() &&
            Initializer->isInClassMemberInitializer() &&
            (!Initializer->isBaseInitializer() || EmitVirtualBaseInitializers ||
             !Initializer->isBaseVirtual()))
          Walker.TraverseStmt(Initializer->getInit());
      if (Ctor->isDefaulted())
        Walker.hashCallContinuationMarkers(Ctor->getBody());
    }
  }
  Walker.hashCallContinuationLayout();
  Walker.assignCallContinuationCounters();
  assert(Walker.NextCounter > 0 && "no entry counter mapped for decl");
  NumRegionCounters = Walker.NextCounter;
  if (CoverageCallContinuations)
    Walker.Hash.combine(PGOHash::CallContinuationCounters);
  FunctionHash = Walker.Hash.finalize();
  if (HashVersion >= PGO_HASH_V4)
    FunctionHash &= llvm::NamedInstrProfRecord::FUNC_HASH_MASK;
}

bool CodeGenPGO::skipRegionMappingForDecl(const Decl *D) {
  if (!D->getBody())
    return true;

  // Skip host-only functions in the CUDA device compilation and device-only
  // functions in the host compilation. Just roughly filter them out based on
  // the function attributes. If there are effectively host-only or device-only
  // ones, their coverage mapping may still be generated.
  if (CGM.getLangOpts().CUDA &&
      ((CGM.getLangOpts().CUDAIsDevice && !D->hasAttr<CUDADeviceAttr>() &&
        !D->hasAttr<CUDAGlobalAttr>()) ||
       (!CGM.getLangOpts().CUDAIsDevice &&
        (D->hasAttr<CUDAGlobalAttr>() ||
         (!D->hasAttr<CUDAHostAttr>() && D->hasAttr<CUDADeviceAttr>())))))
    return true;

  // Don't map the functions in system headers.
  const auto &SM = CGM.getContext().getSourceManager();
  auto Loc = D->getBody()->getBeginLoc();
  return !llvm::coverage::SystemHeadersCoverage && SM.isInSystemHeader(Loc);
}

void CodeGenPGO::emitCounterRegionMapping(GlobalDecl GD) {
  const Decl *D = GD.getDecl();
  if (skipRegionMappingForDecl(D))
    return;

  std::string CoverageMapping;
  llvm::raw_string_ostream OS(CoverageMapping);
  RegionMCDCState->BranchByStmt.clear();
  CoverageMappingGen MappingGen(
      *CGM.getCoverageMapping(), CGM.getContext().getSourceManager(),
      CGM.getLangOpts(), RegionCounterMap.get(), CallContinuationCounters.get(),
      VLATypeEvaluations.get(), RegionMCDCState.get(), NumRegionCounters,
      !isa<CXXConstructorDecl>(D) ||
          !CGM.getTarget().getCXXABI().hasConstructorVariants() ||
          GD.getCtorType() != Ctor_Base);
  MappingGen.emitCounterMapping(D, OS);

  if (CoverageMapping.empty())
    return;

  // Scan max(FalseCnt) and update NumRegionCounters.
  unsigned MaxNumCounters = NumRegionCounters;
  for (const auto &[_, V] : *RegionCounterMap) {
    assert((!V.Executed.hasValue() || MaxNumCounters > V.Executed) &&
           "TrueCnt should not be reassigned");
    if (V.Skipped.hasValue())
      MaxNumCounters = std::max(MaxNumCounters, V.Skipped + 1);
  }
  if (CallContinuationCounters)
    for (const auto &[_, V] : *CallContinuationCounters)
      MaxNumCounters = std::max(MaxNumCounters, V + 1);
  NumRegionCounters = MaxNumCounters;

  CGM.getCoverageMapping()->addFunctionMappingRecord(
      FuncNameVar, FuncName, FunctionHash, CoverageMapping);
}

void CodeGenPGO::emitEmptyCounterMapping(
    const Decl *D, StringRef Name, llvm::GlobalValue::LinkageTypes Linkage) {
  if (skipRegionMappingForDecl(D))
    return;

  std::string CoverageMapping;
  llvm::raw_string_ostream OS(CoverageMapping);
  CoverageMappingGen MappingGen(*CGM.getCoverageMapping(),
                                CGM.getContext().getSourceManager(),
                                CGM.getLangOpts());
  MappingGen.emitEmptyMapping(D, OS);

  if (CoverageMapping.empty())
    return;

  setFuncName(Name, Linkage);
  CGM.getCoverageMapping()->addFunctionMappingRecord(
      FuncNameVar, FuncName, FunctionHash, CoverageMapping, false);
}

void CodeGenPGO::computeRegionCounts(const Decl *D) {
  StmtCountMap.reset(new llvm::DenseMap<const Stmt *, uint64_t>);
  ComputeRegionCounts Walker(*StmtCountMap, *this);
  if (const FunctionDecl *FD = dyn_cast_or_null<FunctionDecl>(D))
    Walker.VisitFunctionDecl(FD);
  else if (const ObjCMethodDecl *MD = dyn_cast_or_null<ObjCMethodDecl>(D))
    Walker.VisitObjCMethodDecl(MD);
  else if (const BlockDecl *BD = dyn_cast_or_null<BlockDecl>(D))
    Walker.VisitBlockDecl(BD);
  else if (const CapturedDecl *CD = dyn_cast_or_null<CapturedDecl>(D))
    Walker.VisitCapturedDecl(const_cast<CapturedDecl *>(CD));
}

void CodeGenPGO::applyFunctionAttributes(
    llvm::IndexedInstrProfReader *PGOReader, llvm::Function *Fn) {
  if (!haveRegionCounts())
    return;

  uint64_t FunctionCount = getRegionCount(nullptr);
  Fn->setEntryCount(FunctionCount);
}

bool CodeGenPGO::hasSkipCounter(const Stmt *S) const {
  if (!RegionCounterMap)
    return false;

  auto I = RegionCounterMap->find(S);
  if (I == RegionCounterMap->end())
    return false;

  return I->second.Skipped.hasValue();
}

void CodeGenPGO::emitCounterSetOrIncrement(CGBuilderTy &Builder, const Stmt *S,
                                           bool UseSkipPath, bool UseBoth,
                                           llvm::Value *StepV) {
  if (!RegionCounterMap)
    return;

  // Allocate S in the Map regardless of emission.
  const auto &TheCounterPair = (*RegionCounterMap)[S];

  if (!Builder.GetInsertBlock())
    return;

  const CounterPair::ValueOpt &Counter =
      (UseSkipPath ? TheCounterPair.Skipped : TheCounterPair.Executed);
  if (!Counter.hasValue())
    return;

  // Make sure that pointer to global is passed in with zero addrspace
  // This is relevant during GPU profiling
  auto *NormalizedFuncNameVarPtr =
      llvm::ConstantExpr::getPointerBitCastOrAddrSpaceCast(
          FuncNameVar, llvm::PointerType::get(CGM.getLLVMContext(), 0));

  llvm::Value *Args[] = {
      NormalizedFuncNameVarPtr, Builder.getInt64(FunctionHash),
      Builder.getInt32(NumRegionCounters), Builder.getInt32(Counter), StepV};

  if (llvm::EnableSingleByteCoverage) {
    assert(!StepV && "StepV is not supported in single byte counter mode");
    Builder.CreateCall(CGM.getIntrinsic(llvm::Intrinsic::instrprof_cover),
                       ArrayRef(Args, 4));
  } else if (!StepV)
    Builder.CreateCall(CGM.getIntrinsic(llvm::Intrinsic::instrprof_increment),
                       ArrayRef(Args, 4));
  else
    Builder.CreateCall(
        CGM.getIntrinsic(llvm::Intrinsic::instrprof_increment_step), Args);
}

void CodeGenPGO::emitCallContinuationCounter(CGBuilderTy &Builder,
                                             CallContinuationOwner Owner,
                                             CallContinuationKind Kind) {
  if (!CallContinuationCounters)
    return;

  auto I = CallContinuationCounters->find({Owner, Kind});
  if (I == CallContinuationCounters->end())
    return;

  unsigned Counter = I->second;

  if (!Builder.GetInsertBlock())
    return;

  auto *NormalizedFuncNameVarPtr =
      llvm::ConstantExpr::getPointerBitCastOrAddrSpaceCast(
          FuncNameVar, llvm::PointerType::get(CGM.getLLVMContext(), 0));

  llvm::Value *Args[] = {
      NormalizedFuncNameVarPtr, Builder.getInt64(FunctionHash),
      Builder.getInt32(NumRegionCounters), Builder.getInt32(Counter)};

  if (llvm::EnableSingleByteCoverage)
    Builder.CreateCall(CGM.getIntrinsic(llvm::Intrinsic::instrprof_cover),
                       ArrayRef(Args, 4));
  else
    Builder.CreateCall(CGM.getIntrinsic(llvm::Intrinsic::instrprof_increment),
                       ArrayRef(Args, 4));
}

bool CodeGenPGO::canEmitMCDCCoverage(const CGBuilderTy &Builder) {
  return (CGM.getCodeGenOpts().hasProfileClangInstr() &&
          CGM.getCodeGenOpts().MCDCCoverage && Builder.GetInsertBlock());
}

void CodeGenPGO::emitMCDCParameters(CGBuilderTy &Builder) {
  if (!canEmitMCDCCoverage(Builder) || !RegionMCDCState)
    return;

  auto *I8PtrTy = llvm::PointerType::getUnqual(CGM.getLLVMContext());

  // Emit intrinsic representing MCDC bitmap parameters at function entry.
  // This is used by the instrumentation pass, but it isn't actually lowered to
  // anything.
  llvm::Value *Args[3] = {llvm::ConstantExpr::getBitCast(FuncNameVar, I8PtrTy),
                          Builder.getInt64(FunctionHash),
                          Builder.getInt32(RegionMCDCState->BitmapBits)};
  Builder.CreateCall(
      CGM.getIntrinsic(llvm::Intrinsic::instrprof_mcdc_parameters), Args);
}

/// Fill mcdc.addr order by ID.
std::vector<Address *>
CodeGenPGO::getMCDCCondBitmapAddrArray(CGBuilderTy &Builder) {
  std::vector<Address *> Result;

  if (!canEmitMCDCCoverage(Builder) || !RegionMCDCState)
    return Result;

  SmallVector<std::pair<unsigned, Address *>> SortedPair;
  for (auto &[_, V] : RegionMCDCState->DecisionByStmt)
    if (V.isValid())
      SortedPair.emplace_back(V.ID, &V.MCDCCondBitmapAddr);

  llvm::sort(SortedPair);

  for (auto &[_, MCDCCondBitmapAddr] : SortedPair)
    Result.push_back(MCDCCondBitmapAddr);

  return Result;
}

void CodeGenPGO::emitMCDCTestVectorBitmapUpdate(CGBuilderTy &Builder,
                                                const Expr *S,
                                                CodeGenFunction &CGF) {
  if (!canEmitMCDCCoverage(Builder) || !RegionMCDCState)
    return;

  S = S->IgnoreParens();

  auto DecisionStateIter = RegionMCDCState->DecisionByStmt.find(S);
  if (DecisionStateIter == RegionMCDCState->DecisionByStmt.end())
    return;

  auto &MCDCCondBitmapAddr = DecisionStateIter->second.MCDCCondBitmapAddr;
  if (!MCDCCondBitmapAddr.isValid())
    return;

  // Don't create tvbitmap_update if the record is allocated but excluded.
  // Or `bitmap |= (1 << 0)` would be wrongly executed to the next bitmap.
  if (DecisionStateIter->second.Indices.size() == 0)
    return;

  // Extract the offset of the global bitmap associated with this expression.
  unsigned MCDCTestVectorBitmapOffset = DecisionStateIter->second.BitmapIdx;
  auto *I8PtrTy = llvm::PointerType::getUnqual(CGM.getLLVMContext());

  // Emit intrinsic responsible for updating the global bitmap corresponding to
  // a boolean expression. The index being set is based on the value loaded
  // from a pointer to a dedicated temporary value on the stack that is itself
  // updated via emitMCDCCondBitmapReset() and emitMCDCCondBitmapUpdate(). The
  // index represents an executed test vector.
  llvm::Value *Args[4] = {llvm::ConstantExpr::getBitCast(FuncNameVar, I8PtrTy),
                          Builder.getInt64(FunctionHash),
                          Builder.getInt32(MCDCTestVectorBitmapOffset),
                          MCDCCondBitmapAddr.emitRawPointer(CGF)};
  Builder.CreateCall(
      CGM.getIntrinsic(llvm::Intrinsic::instrprof_mcdc_tvbitmap_update), Args);
}

void CodeGenPGO::emitMCDCCondBitmapReset(CGBuilderTy &Builder, const Expr *S) {
  if (!canEmitMCDCCoverage(Builder) || !RegionMCDCState)
    return;

  auto I = RegionMCDCState->DecisionByStmt.find(S->IgnoreParens());
  if (I == RegionMCDCState->DecisionByStmt.end())
    return;

  auto &MCDCCondBitmapAddr = I->second.MCDCCondBitmapAddr;
  if (!MCDCCondBitmapAddr.isValid())
    return;

  // Emit intrinsic that resets a dedicated temporary value on the stack to 0.
  Builder.CreateStore(Builder.getInt32(0), MCDCCondBitmapAddr);
}

void CodeGenPGO::emitMCDCCondBitmapUpdate(CGBuilderTy &Builder, const Expr *S,
                                          llvm::Value *Val,
                                          CodeGenFunction &CGF) {
  if (!canEmitMCDCCoverage(Builder) || !RegionMCDCState)
    return;

  // Even though, for simplicity, parentheses and unary logical-NOT operators
  // are considered part of their underlying condition for both MC/DC and
  // branch coverage, the condition IDs themselves are assigned and tracked
  // using the underlying condition itself.  This is done solely for
  // consistency since parentheses and logical-NOTs are ignored when checking
  // whether the condition is actually an instrumentable condition. This can
  // also make debugging a bit easier.
  S = CodeGenFunction::stripCond(S);

  auto BranchStateIter = RegionMCDCState->BranchByStmt.find(S);
  if (BranchStateIter == RegionMCDCState->BranchByStmt.end())
    return;

  // Extract the ID of the condition we are setting in the bitmap.
  const auto &Branch = BranchStateIter->second;
  assert(Branch.ID >= 0 && "Condition has no ID!");
  assert(Branch.DecisionStmt);

  // Cancel the emission if the Decision is erased after the allocation.
  const auto DecisionIter =
      RegionMCDCState->DecisionByStmt.find(Branch.DecisionStmt);
  if (DecisionIter == RegionMCDCState->DecisionByStmt.end())
    return;

  auto &MCDCCondBitmapAddr = DecisionIter->second.MCDCCondBitmapAddr;
  if (!MCDCCondBitmapAddr.isValid())
    return;

  const auto &TVIdxs = DecisionIter->second.Indices[Branch.ID];

  auto *CurTV = Builder.CreateLoad(MCDCCondBitmapAddr,
                                   "mcdc." + Twine(Branch.ID + 1) + ".cur");
  auto *NewTV = Builder.CreateAdd(CurTV, Builder.getInt32(TVIdxs[true]));
  NewTV = Builder.CreateSelect(
      Val, NewTV, Builder.CreateAdd(CurTV, Builder.getInt32(TVIdxs[false])));
  Builder.CreateStore(NewTV, MCDCCondBitmapAddr);
}

void CodeGenPGO::setValueProfilingFlag(llvm::Module &M) {
  if (CGM.getCodeGenOpts().hasProfileClangInstr())
    M.addModuleFlag(llvm::Module::Warning, "EnableValueProfiling",
                    uint32_t(EnableValueProfiling));
}

void CodeGenPGO::setProfileVersion(llvm::Module &M) {
  if (CGM.getCodeGenOpts().hasProfileClangInstr() &&
      llvm::EnableSingleByteCoverage) {
    const StringRef VarName(INSTR_PROF_QUOTE(INSTR_PROF_RAW_VERSION_VAR));
    llvm::Type *IntTy64 = llvm::Type::getInt64Ty(M.getContext());
    uint64_t ProfileVersion =
        (INSTR_PROF_RAW_VERSION | VARIANT_MASK_BYTE_COVERAGE);

    auto IRLevelVersionVariable = new llvm::GlobalVariable(
        M, IntTy64, true, llvm::GlobalValue::WeakAnyLinkage,
        llvm::Constant::getIntegerValue(IntTy64,
                                        llvm::APInt(64, ProfileVersion)),
        VarName);

    IRLevelVersionVariable->setVisibility(llvm::GlobalValue::HiddenVisibility);
    llvm::Triple TT(M.getTargetTriple());
    if (TT.isGPU())
      IRLevelVersionVariable->setVisibility(
          llvm::GlobalValue::ProtectedVisibility);
    if (TT.supportsCOMDAT()) {
      IRLevelVersionVariable->setLinkage(llvm::GlobalValue::ExternalLinkage);
      IRLevelVersionVariable->setComdat(M.getOrInsertComdat(VarName));
    }
    IRLevelVersionVariable->setDSOLocal(true);
  }
}

// This method either inserts a call to the profile run-time during
// instrumentation or puts profile data into metadata for PGO use.
void CodeGenPGO::valueProfile(CGBuilderTy &Builder, uint32_t ValueKind,
                              llvm::Instruction *ValueSite,
                              llvm::Value *ValuePtr) {

  if (!EnableValueProfiling)
    return;

  if (!ValuePtr || !ValueSite || !Builder.GetInsertBlock())
    return;

  if (isa<llvm::Constant>(ValuePtr))
    return;

  bool InstrumentValueSites = CGM.getCodeGenOpts().hasProfileClangInstr();
  if (InstrumentValueSites && RegionCounterMap) {
    auto BuilderInsertPoint = Builder.saveIP();
    Builder.SetInsertPoint(ValueSite);
    llvm::Value *Args[5] = {
        FuncNameVar, Builder.getInt64(FunctionHash),
        Builder.CreatePtrToInt(ValuePtr, Builder.getInt64Ty()),
        Builder.getInt32(ValueKind),
        Builder.getInt32(NumValueSites[ValueKind]++)};
    Builder.CreateCall(
        CGM.getIntrinsic(llvm::Intrinsic::instrprof_value_profile), Args);
    Builder.restoreIP(BuilderInsertPoint);
    return;
  }

  llvm::IndexedInstrProfReader *PGOReader = CGM.getPGOReader();
  if (PGOReader && haveRegionCounts()) {
    // We record the top most called three functions at each call site.
    // Profile metadata contains "VP" string identifying this metadata
    // as value profiling data, then a uint32_t value for the value profiling
    // kind, a uint64_t value for the total number of times the call is
    // executed, followed by the function hash and execution count (uint64_t)
    // pairs for each function.
    if (NumValueSites[ValueKind] >= ProfRecord->getNumValueSites(ValueKind))
      return;

    llvm::annotateValueSite(CGM.getModule(), *ValueSite, *ProfRecord,
                            (llvm::InstrProfValueKind)ValueKind,
                            NumValueSites[ValueKind]);

    NumValueSites[ValueKind]++;
  }
}

void CodeGenPGO::loadRegionCounts(llvm::IndexedInstrProfReader *PGOReader,
                                  bool IsInMainFile) {
  CGM.getPGOStats().addVisited(IsInMainFile);
  RegionCounts.clear();
  auto RecordExpected = PGOReader->getInstrProfRecord(FuncName, FunctionHash);
  if (auto E = RecordExpected.takeError()) {
    auto IPE = std::get<0>(llvm::InstrProfError::take(std::move(E)));
    if (IPE == llvm::instrprof_error::unknown_function)
      CGM.getPGOStats().addMissing(IsInMainFile);
    else if (IPE == llvm::instrprof_error::hash_mismatch)
      CGM.getPGOStats().addMismatched(IsInMainFile);
    else if (IPE == llvm::instrprof_error::malformed)
      // TODO: Consider a more specific warning for this case.
      CGM.getPGOStats().addMismatched(IsInMainFile);
    return;
  }
  ProfRecord =
      std::make_unique<llvm::InstrProfRecord>(std::move(RecordExpected.get()));
  RegionCounts = ProfRecord->Counts;
}

/// Calculate what to divide by to scale weights.
///
/// Given the maximum weight, calculate a divisor that will scale all the
/// weights to strictly less than UINT32_MAX.
static uint64_t calculateWeightScale(uint64_t MaxWeight) {
  return MaxWeight < UINT32_MAX ? 1 : MaxWeight / UINT32_MAX + 1;
}

/// Scale an individual branch weight (and add 1).
///
/// Scale a 64-bit weight down to 32-bits using \c Scale.
///
/// According to Laplace's Rule of Succession, it is better to compute the
/// weight based on the count plus 1, so universally add 1 to the value.
///
/// \pre \c Scale was calculated by \a calculateWeightScale() with a weight no
/// greater than \c Weight.
static uint32_t scaleBranchWeight(uint64_t Weight, uint64_t Scale) {
  assert(Scale && "scale by 0?");
  uint64_t Scaled = Weight / Scale + 1;
  assert(Scaled <= UINT32_MAX && "overflow 32-bits");
  return Scaled;
}

llvm::MDNode *CodeGenFunction::createProfileWeights(uint64_t TrueCount,
                                                    uint64_t FalseCount) const {
  // Check for empty weights.
  if (!TrueCount && !FalseCount)
    return nullptr;

  // Calculate how to scale down to 32-bits.
  uint64_t Scale = calculateWeightScale(std::max(TrueCount, FalseCount));

  llvm::MDBuilder MDHelper(CGM.getLLVMContext());
  return MDHelper.createBranchWeights(scaleBranchWeight(TrueCount, Scale),
                                      scaleBranchWeight(FalseCount, Scale));
}

llvm::MDNode *
CodeGenFunction::createProfileWeights(ArrayRef<uint64_t> Weights) const {
  // We need at least two elements to create meaningful weights.
  if (Weights.size() < 2)
    return nullptr;

  // Check for empty weights.
  uint64_t MaxWeight = *llvm::max_element(Weights);
  if (MaxWeight == 0)
    return nullptr;

  // Calculate how to scale down to 32-bits.
  uint64_t Scale = calculateWeightScale(MaxWeight);

  SmallVector<uint32_t, 16> ScaledWeights;
  ScaledWeights.reserve(Weights.size());
  for (uint64_t W : Weights)
    ScaledWeights.push_back(scaleBranchWeight(W, Scale));

  llvm::MDBuilder MDHelper(CGM.getLLVMContext());
  return MDHelper.createBranchWeights(ScaledWeights);
}

llvm::MDNode *
CodeGenFunction::createProfileWeightsForLoop(const Stmt *Cond,
                                             uint64_t LoopCount) const {
  if (!PGO->haveRegionCounts())
    return nullptr;
  std::optional<uint64_t> CondCount = PGO->getStmtCount(Cond);
  if (!CondCount || *CondCount == 0)
    return nullptr;
  return createProfileWeights(LoopCount,
                              std::max(*CondCount, LoopCount) - LoopCount);
}

void CodeGenFunction::incrementProfileCounter(CounterForIncrement ExecSkip,
                                              const Stmt *S, bool UseBoth,
                                              llvm::Value *StepV) {
  if (CGM.getCodeGenOpts().hasProfileClangInstr() &&
      !CurFn->hasFnAttribute(llvm::Attribute::NoProfile) &&
      !CurFn->hasFnAttribute(llvm::Attribute::SkipProfile)) {
    auto AL = ApplyDebugLocation::CreateArtificial(*this);
    PGO->emitCounterSetOrIncrement(Builder, S, (ExecSkip == UseSkipPath),
                                   UseBoth, StepV);
  }
  PGO->setCurrentStmt(S);
}

void CodeGenFunction::incrementCallContinuationProfileCounter(
    const Stmt *S, CallContinuationKind Kind) {
  CodeGenFunction *ProfileOwner = this;
  while (ProfileOwner->IsOutlinedSEHHelper && ProfileOwner->ParentCGF)
    ProfileOwner = ProfileOwner->ParentCGF;

  if (CGM.getCodeGenOpts().hasProfileClangInstr() &&
      !ProfileOwner->CurFn->hasFnAttribute(llvm::Attribute::NoProfile) &&
      !ProfileOwner->CurFn->hasFnAttribute(llvm::Attribute::SkipProfile)) {
    auto AL = ApplyDebugLocation::CreateArtificial(*this);
    ProfileOwner->PGO->emitCallContinuationCounter(Builder, S, Kind);
  }
}

void CodeGenFunction::incrementCallContinuationProfileCounter(
    const Decl *D, CallContinuationKind Kind) {
  CodeGenFunction *ProfileOwner = this;
  while (ProfileOwner->IsOutlinedSEHHelper && ProfileOwner->ParentCGF)
    ProfileOwner = ProfileOwner->ParentCGF;

  if (CGM.getCodeGenOpts().hasProfileClangInstr() &&
      !ProfileOwner->CurFn->hasFnAttribute(llvm::Attribute::NoProfile) &&
      !ProfileOwner->CurFn->hasFnAttribute(llvm::Attribute::SkipProfile)) {
    auto AL = ApplyDebugLocation::CreateArtificial(*this);
    ProfileOwner->PGO->emitCallContinuationCounter(Builder, D, Kind);
  }
}

bool CodeGenFunction::hasSkipCounter(const Stmt *S) const {
  return PGO->hasSkipCounter(S);
}
void CodeGenFunction::markStmtAsUsed(bool Skipped, const Stmt *S) {
  PGO->markStmtAsUsed(Skipped, S);
}
void CodeGenFunction::markStmtMaybeUsed(const Stmt *S) {
  PGO->markStmtMaybeUsed(S);
}

void CodeGenFunction::maybeCreateMCDCCondBitmap() {
  if (isMCDCCoverageEnabled()) {
    PGO->emitMCDCParameters(Builder);

    // Set up MCDCCondBitmapAddr for each Decision.
    // Note: This doesn't initialize Addrs in invalidated Decisions.
    for (auto *MCDCCondBitmapAddr : PGO->getMCDCCondBitmapAddrArray(Builder))
      *MCDCCondBitmapAddr =
          CreateIRTempWithoutCast(getContext().UnsignedIntTy, "mcdc.addr");
  }
}
bool CodeGenFunction::isMCDCDecisionExpr(const Expr *E) const {
  return PGO->isMCDCDecisionExpr(E);
}
bool CodeGenFunction::isMCDCBranchExpr(const Expr *E) const {
  return PGO->isMCDCBranchExpr(E);
}
void CodeGenFunction::maybeResetMCDCCondBitmap(const Expr *E) {
  if (isMCDCCoverageEnabled() && isBinaryLogicalOp(E)) {
    PGO->emitMCDCCondBitmapReset(Builder, E);
    PGO->setCurrentStmt(E);
  }
}
void CodeGenFunction::maybeUpdateMCDCTestVectorBitmap(const Expr *E) {
  if (isMCDCCoverageEnabled() && isBinaryLogicalOp(E)) {
    PGO->emitMCDCTestVectorBitmapUpdate(Builder, E, *this);
    PGO->setCurrentStmt(E);
  }
}

void CodeGenFunction::maybeUpdateMCDCCondBitmap(const Expr *E,
                                                llvm::Value *Val) {
  if (isMCDCCoverageEnabled()) {
    PGO->emitMCDCCondBitmapUpdate(Builder, E, Val, *this);
    PGO->setCurrentStmt(E);
  }
}

uint64_t CodeGenFunction::getProfileCount(const Stmt *S) {
  return PGO->getStmtCount(S).value_or(0);
}

/// Set the profiler's current count.
void CodeGenFunction::setCurrentProfileCount(uint64_t Count) {
  PGO->setCurrentRegionCount(Count);
}

/// Get the profiler's current count. This is generally the count for the most
/// recently incremented counter.
uint64_t CodeGenFunction::getCurrentProfileCount() {
  return PGO->getCurrentRegionCount();
}

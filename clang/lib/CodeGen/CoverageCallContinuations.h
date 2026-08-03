//===--- CoverageCallContinuations.h - Coverage continuations -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_CODEGEN_COVERAGECALLCONTINUATIONS_H
#define LLVM_CLANG_LIB_CODEGEN_COVERAGECALLCONTINUATIONS_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include <cstdint>
#include <utility>

namespace clang {

class ASTContext;
class AtomicExpr;
class BinaryOperator;
class BlockExpr;
class CallExpr;
class CXXConstructExpr;
class CXXRewrittenBinaryOperator;
class Decl;
class Expr;
class LangOptions;
class ObjCMessageExpr;
class PseudoObjectExpr;
class QualType;
class Stmt;
class UnaryExprOrTypeTraitExpr;

namespace CodeGen {

class CodeGenModule;

/// A normal-completion boundary whose execution cannot always be derived from
/// the source-level region counters.  Keep this order stable: it participates
/// in the instrumentation hash when call-continuation coverage is enabled.
enum class CallContinuationKind : uint8_t {
  Call,
  Construct,
  DefaultArgument,
  DefaultInitializer,
  ArrayInitialization,
  VLAEvaluation,
  Declaration,
  FullExpression,
  NewExpression,
  NewInitializer,
  DeleteExpression,
  DynamicCast,
  TypeidExpression,
  RewrittenOperator,
  ObjCMessage,
  PseudoObject,
  ConstructorPrologue,
  CoroutineSuspend,
  CoroutineBody,
  CompoundFallthrough,
  IfExit,
  LoopBody,
  LoopContinue,
  LoopBackedge,
  LoopExit,
  Assignment,
  BlockLiteral,
  AtomicOperation,
  TLSAccess,
  ComplexOperation,
  OverflowOperation,
  Last = OverflowOperation,
};

using CallContinuationOwner = llvm::PointerUnion<const Stmt *, const Decl *>;
using CallContinuationKey =
    std::pair<CallContinuationOwner, CallContinuationKind>;
using CallContinuationCounterMap =
    llvm::DenseMap<CallContinuationKey, unsigned>;

enum class VLATypeEvaluationKind : uint8_t {
  ArrayBound,
  TypeOfExpression,
};

struct VLATypeEvaluation {
  VLATypeEvaluationKind Kind;
  const Expr *Expression;
};

using VLATypeEvaluationPlan = llvm::SmallVector<const Expr *, 2>;
using VLATypeEvaluationMap =
    llvm::DenseMap<CallContinuationOwner, VLATypeEvaluationPlan>;

/// Visit the run-time expressions in a variably-modified type in exactly the
/// order used by CodeGenFunction::EmitVariablyModifiedType. The callback still
/// sees an array bound which may already be cached; callers decide whether the
/// expression is evaluated at their particular emission site.
void visitVLATypeEvaluations(
    QualType Type, ASTContext &Context,
    llvm::function_ref<void(const VLATypeEvaluation &)> Visit);

/// Visit call operands in the same order used by IR generation. The default
/// argument order is ABI-dependent; language-mandated operator/list ordering
/// takes precedence.
void visitCallContinuationCallChildren(
    const CallExpr *E, bool ReverseDefaultArgs,
    llvm::function_ref<void(const Stmt *)> Visit);
/// Visit atomic builtin operands in the exact order used by EmitAtomicExpr.
void visitCallContinuationAtomicExprChildren(
    const AtomicExpr *E, llvm::function_ref<void(const Stmt *)> Visit);
void visitCallContinuationConstructChildren(
    const CXXConstructExpr *E, bool ReverseDefaultArgs,
    llvm::function_ref<void(const Stmt *)> Visit);
void visitCallContinuationRewrittenOperatorChildren(
    const CXXRewrittenBinaryOperator *E, bool ReverseDefaultArgs,
    llvm::function_ref<void(const Stmt *)> Visit);
void visitCallContinuationObjCMessageChildren(
    const ObjCMessageExpr *E, bool ReverseDefaultArgs,
    llvm::function_ref<void(const Stmt *)> Visit);
/// Visit a pseudo-object's semantic expressions in CodeGen evaluation order.
/// Direct non-unique opaque values bind and evaluate their source here. Direct
/// unique opaque values are deferred until an actual unique opaque-value use.
void visitCallContinuationPseudoObjectSemantics(
    const PseudoObjectExpr *E, llvm::function_ref<void(const Expr *)> Visit);
bool callContinuationAssignmentEvaluatesRHSFirst(const BinaryOperator *E,
                                                 const ASTContext &Context);
bool callContinuationAssignmentNeedsCounter(const BinaryOperator *E);
bool callContinuationComplexOperationNeedsCounter(const BinaryOperator *E);
bool callContinuationOverflowOperationNeedsCounter(const Expr *E,
                                                   const LangOptions &Opts);
bool callContinuationBlockLiteralNeedsCounter(const BlockExpr *E);
bool callContinuationBlockLiteralHasCleanup(const BlockExpr *E);
bool callContinuationTLSAccessNeedsCounter(const Expr *E, CodeGenModule &CGM);
bool callContinuationEvaluatesVLAExtent(const UnaryExprOrTypeTraitExpr *E,
                                        const ASTContext &Context);

} // namespace CodeGen
} // namespace clang

#endif

//===- FoldUtils.cpp ---- Fold Utilities ----------------------------------===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This file defines various operation fold utilities. These utilities are
// intended to be used by passes to unify and simply their logic.
//
//===----------------------------------------------------------------------===//

#include "mlir/Transforms/FoldUtils.h"

#include "mlir/IR/Builders.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/Operation.h"
#include "mlir/StandardOps/Ops.h"

using namespace mlir;

//===----------------------------------------------------------------------===//
// OperationFolder
//===----------------------------------------------------------------------===//

LogicalResult
OperationFolder::tryToFold(Operation *op,
                           std::function<void(Operation *)> preReplaceAction) {
  assert(op->getFunction() == function &&
         "cannot constant fold op from another function");

  // The constant op also implements the constant fold hook; it can be folded
  // into the value it contains. We need to consider constants before the
  // constant folding logic to avoid re-creating the same constant later.
  // TODO: Extend to support dialect-specific constant ops.
  if (auto constant = dyn_cast<ConstantOp>(op)) {
    // If this constant is dead, update bookkeeping and signal the caller.
    if (constant.use_empty()) {
      notifyRemoval(op);
      op->erase();
      return success();
    }
    // Otherwise, try to see if we can de-duplicate it.
    return tryToUnify(op);
  }

  // Try to fold the operation.
  SmallVector<Value *, 8> results;
  if (failed(tryToFold(op, results)))
    return failure();

  // Constant folding succeeded. We will start replacing this op's uses and
  // eventually erase this op. Invoke the callback provided by the caller to
  // perform any pre-replacement action.
  if (preReplaceAction)
    preReplaceAction(op);

  // Check to see if the operation was just updated in place.
  if (results.empty())
    return success();

  // Otherwise, replace all of the result values and erase the operation.
  for (unsigned i = 0, e = results.size(); i != e; ++i)
    op->getResult(i)->replaceAllUsesWith(results[i]);
  op->erase();
  return success();
}

/// Tries to perform folding on the given `op`. If successful, populates
/// `results` with the results of the folding.
LogicalResult OperationFolder::tryToFold(Operation *op,
                                         SmallVectorImpl<Value *> &results) {
  assert(op->getFunction() == function &&
         "cannot constant fold op from another function");

  SmallVector<Attribute, 8> operandConstants;
  SmallVector<OpFoldResult, 8> foldResults;

  // Check to see if any operands to the operation is constant and whether
  // the operation knows how to constant fold itself.
  operandConstants.assign(op->getNumOperands(), Attribute());
  for (unsigned i = 0, e = op->getNumOperands(); i != e; ++i)
    matchPattern(op->getOperand(i), m_Constant(&operandConstants[i]));

  // If this is a commutative binary operation with a constant on the left
  // side move it to the right side.
  if (operandConstants.size() == 2 && operandConstants[0] &&
      !operandConstants[1] && op->isCommutative()) {
    std::swap(op->getOpOperand(0), op->getOpOperand(1));
    std::swap(operandConstants[0], operandConstants[1]);
  }

  // Attempt to constant fold the operation.
  if (failed(op->fold(operandConstants, foldResults)))
    return failure();

  // Check to see if the operation was just updated in place.
  if (foldResults.empty())
    return success();
  assert(foldResults.size() == op->getNumResults());

  // Create the result constants and replace the results.
  OpBuilder builder(op);
  for (unsigned i = 0, e = op->getNumResults(); i != e; ++i) {
    assert(!foldResults[i].isNull() && "expected valid OpFoldResult");

    // Check if the result was an SSA value.
    if (auto *repl = foldResults[i].dyn_cast<Value *>()) {
      results.emplace_back(repl);
      continue;
    }

    // If we already have a canonicalized version of this constant, just reuse
    // it. Otherwise create a new one.
    Attribute attrRepl = foldResults[i].get<Attribute>();
    auto *res = op->getResult(i);
    auto &constInst =
        uniquedConstants[std::make_pair(attrRepl, res->getType())];
    if (!constInst) {
      // TODO: Extend to support dialect-specific constant ops.
      auto newOp =
          builder.create<ConstantOp>(op->getLoc(), res->getType(), attrRepl);
      // Register to the constant map and also move up to entry block to
      // guarantee dominance.
      constInst = newOp.getOperation();
      moveConstantToEntryBlock(constInst);
    }
    results.push_back(constInst->getResult(0));
  }

  return success();
}

void OperationFolder::notifyRemoval(Operation *op) {
  assert(op->getFunction() == function &&
         "cannot remove constant from another function");

  Attribute constValue;
  if (!matchPattern(op, m_Constant(&constValue)))
    return;

  // This constant is dead. keep uniquedConstants up to date.
  auto it = uniquedConstants.find({constValue, op->getResult(0)->getType()});
  if (it != uniquedConstants.end() && it->second == op)
    uniquedConstants.erase(it);
}

LogicalResult OperationFolder::tryToUnify(Operation *op) {
  Attribute constValue;
  matchPattern(op, m_Constant(&constValue));
  assert(constValue);

  // Check to see if we already have a constant with this type and value:
  auto &constInst =
      uniquedConstants[std::make_pair(constValue, op->getResult(0)->getType())];
  if (constInst) {
    // If this constant is already our uniqued one, then leave it alone.
    if (constInst == op)
      return failure();

    // Otherwise replace this redundant constant with the uniqued one.  We know
    // this is safe because we move constants to the top of the function when
    // they are uniqued, so we know they dominate all uses.
    op->getResult(0)->replaceAllUsesWith(constInst->getResult(0));
    op->erase();
    return success();
  }

  // If we have no entry, then we should unique this constant as the
  // canonical version.  To ensure safe dominance, move the operation to the
  // entry block of the function.
  constInst = op;
  moveConstantToEntryBlock(op);
  return failure();
}

void OperationFolder::moveConstantToEntryBlock(Operation *op) {
  // Insert at the very top of the entry block.
  auto &entryBB = function->front();
  op->moveBefore(&entryBB, entryBB.begin());
}

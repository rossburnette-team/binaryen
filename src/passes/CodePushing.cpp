/*
 * Copyright 2016 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Pushes code "forward" as much as possible, potentially into
// a location behind a condition, where it might not always execute.
//

#include <ir/effects.h>
#include <pass.h>
#include <wasm-builder.h>
#include <wasm.h>

namespace wasm {

//
// Analyzers some useful local properties: # of sets and gets, and SFA.
//
// Single First Assignment (SFA) form: the local has a single local.set, is
// not a parameter, and has no local.gets before the local.set in postorder.
// This is a much weaker property than SSA, obviously, but together with
// our implicit dominance properties in the structured AST is quite useful.
//
struct LocalAnalyzer : public PostWalker<LocalAnalyzer> {
  std::vector<bool> sfa;
  std::vector<Index> numSets;
  std::vector<Index> numGets;

  void analyze(Function* func) {
    auto num = func->getNumLocals();
    numSets.clear();
    numSets.resize(num);
    numGets.clear();
    numGets.resize(num);
    sfa.clear();
    sfa.resize(num);
    std::fill(sfa.begin() + func->getNumParams(), sfa.end(), true);
    walk(func->body);
    for (Index i = 0; i < num; i++) {
      if (numSets[i] == 0) {
        sfa[i] = false;
      }
    }
  }

  bool isSFA(Index i) { return sfa[i]; }

  Index getNumGets(Index i) { return numGets[i]; }

  void visitLocalGet(LocalGet* curr) {
    if (numSets[curr->index] == 0) {
      sfa[curr->index] = false;
    }
    numGets[curr->index]++;
  }

  void visitLocalSet(LocalSet* curr) {
    numSets[curr->index]++;
    if (numSets[curr->index] > 1) {
      sfa[curr->index] = false;
    }
  }
};

// Implements core optimization logic. Used and then discarded entirely
// for each block.
class Pusher {
  ExpressionList& list;
  LocalAnalyzer& analyzer;
  std::vector<Index>& numGetsSoFar;
  PassOptions& passOptions;
  Module& module;

public:
  Pusher(Block* block,
         LocalAnalyzer& analyzer,
         std::vector<Index>& numGetsSoFar,
         PassOptions& passOptions,
         Module& module)
    : list(block->list), analyzer(analyzer), numGetsSoFar(numGetsSoFar),
      passOptions(passOptions), module(module) {
    // Find an optimization segment: from the first pushable thing, to the first
    // point past which we want to push. We then push in that range before
    // continuing forward.
    // we never need to push past a final element, as we couldn't be used after
    // it.
    Index relevant = list.size() - 1;
    const Index nothing = -1;
    Index i = 0;
    Index firstPushable = nothing;
    while (i < relevant) {
      if (firstPushable == nothing && isPushable(list[i])) {
        firstPushable = i;
        i++;
        continue;
      }
      if (firstPushable != nothing && isPushPoint(list[i])) {
        // optimize this segment, and proceed from where it tells us
        i = optimizeSegment(firstPushable, i);
        firstPushable = nothing;
        continue;
      }
      i++;
    }
  }

private:
  LocalSet* isPushable(Expression* curr) {
    auto* set = curr->dynCast<LocalSet>();
    if (!set) {
      return nullptr;
    }
    auto index = set->index;
    // to be pushable, this must be SFA and the right # of gets,
    // but also have no side effects, as it may not execute if pushed.
    if (analyzer.isSFA(index) &&
        numGetsSoFar[index] == analyzer.getNumGets(index) &&
        !EffectAnalyzer(passOptions, module, set->value).hasSideEffects()) {
      return set;
    }
    return nullptr;
  }

  // Push past conditional control flow.
  // TODO: push into ifs as well
  bool isPushPoint(Expression* curr) {
    // look through drops
    if (auto* drop = curr->dynCast<Drop>()) {
      curr = drop->value;
    }
    if (curr->is<If>()) {
      return true;
    }
    if (auto* br = curr->dynCast<Break>()) {
      return !!br->condition;
    }
    return false;
  }

  Index optimizeSegment(Index firstPushable, Index pushPoint) {
    // The interesting part. Starting at firstPushable, try to push
    // code past pushPoint. We start at the end since we are pushing
    // forward, that way we can push later things out of the way
    // of earlier ones. Once we know all we can push, we push it all
    // in one pass, keeping the order of the pushables intact.
    assert(firstPushable != Index(-1) && pushPoint != Index(-1) &&
           firstPushable < pushPoint);
    // everything that matters if you want to be pushed past the pushPoint
    EffectAnalyzer cumulativeEffects(passOptions, module);
    cumulativeEffects.walk(list[pushPoint]);
    // It is ok to ignore branching out of the block here, that is the crucial
    // point of this optimization. That is, we are in a situation like this:
    //
    // {
    //   x = value;
    //   if (..) break;
    //   foo(x);
    // }
    //
    // If the branch is taken, then that's fine, it will jump out of this block
    // and reach some outer scope, and in that case we never need x at all
    // (since we've proven before that x is not used outside of this block, see
    // numGetsSoFar which we use for that). Similarly, control flow could
    // transfer away via a return or an exception and that would be ok as well.
    cumulativeEffects.ignoreControlFlowTransfers();
    std::vector<LocalSet*> toPush;
    Index i = pushPoint - 1;
    while (1) {
      auto* pushable = isPushable(list[i]);
      if (pushable) {
        auto iter = pushableEffects.find(pushable);
        if (iter == pushableEffects.end()) {
          iter =
            pushableEffects
              .emplace(std::piecewise_construct,
                       std::forward_as_tuple(pushable),
                       std::forward_as_tuple(passOptions, module, pushable))
              .first;
        }
        auto& effects = iter->second;
        if (cumulativeEffects.invalidates(effects)) {
          // we can't push this, so further pushables must pass it
          cumulativeEffects.mergeIn(effects);
        } else {
          // we can push this, great!
          toPush.push_back(pushable);
        }
        if (i == firstPushable) {
          // no point in looking further
          break;
        }
      } else {
        // something that can't be pushed, so it might block further pushing
        cumulativeEffects.walk(list[i]);
      }
      assert(i > 0);
      i--;
    }
    if (toPush.size() == 0) {
      // nothing to do, can only continue after the push point
      return pushPoint + 1;
    }
    // we have work to do!
    Index total = toPush.size();
    Index last = total - 1;
    Index skip = 0;
    for (Index i = firstPushable; i <= pushPoint; i++) {
      // we see the first elements at the end of toPush
      if (skip < total && list[i] == toPush[last - skip]) {
        // this is one of our elements to push, skip it
        skip++;
      } else {
        if (skip) {
          list[i - skip] = list[i];
        }
      }
    }
    assert(skip == total);
    // write out the skipped elements
    for (Index i = 0; i < total; i++) {
      list[pushPoint - i] = toPush[i];
    }
    // proceed right after the push point, we may push the pushed elements again
    return pushPoint - total + 1;
  }

  // Pushables may need to be scanned more than once, so cache their effects.
  std::unordered_map<LocalSet*, EffectAnalyzer> pushableEffects;
};

struct CodePushing : public WalkerPass<PostWalker<CodePushing>> {
  bool isFunctionParallel() override { return true; }

  // This pass moves code forward in blocks, but a local.set would not be moved
  // after a local.get with the same index (effects prevent breaking things that
  // way), so validation will be preserved.
  bool requiresNonNullableLocalFixups() override { return false; }

  std::unique_ptr<Pass> create() override {
    return std::make_unique<CodePushing>();
  }

  LocalAnalyzer analyzer;

  // gets seen so far in the main traversal
  std::vector<Index> numGetsSoFar;

  void doWalkFunction(Function* func) {
    // pre-scan to find which vars are sfa, and also count their gets&sets
    analyzer.analyze(func);
    // prepare to walk
    numGetsSoFar.clear();
    numGetsSoFar.resize(func->getNumLocals());
    // walk and optimize
    walk(func->body);
  }

  void visitLocalGet(LocalGet* curr) { numGetsSoFar[curr->index]++; }

  void visitBlock(Block* curr) {
    // Pushing code only makes sense if we are size 3 or above: we need
    // one element to push, an element to push it past, and an element to use
    // what we pushed.
    if (curr->list.size() < 3) {
      return;
    }
    // At this point in the postorder traversal we have gone through all our
    // children. Therefore any variable whose gets seen so far is equal to the
    // total gets must have no further users after this block. And therefore
    // when we see an SFA variable defined here, we know it isn't used before it
    // either, and has just this one assign. So we can push it forward while we
    // don't hit a non-control-flow ordering invalidation issue, since if this
    // isn't a loop, it's fine (we're not used outside), and if it is, we hit
    // the assign before any use (as we can't push it past a use).
    Pusher pusher(curr, analyzer, numGetsSoFar, getPassOptions(), *getModule());
  }
};

Pass* createCodePushingPass() { return new CodePushing(); }

} // namespace wasm

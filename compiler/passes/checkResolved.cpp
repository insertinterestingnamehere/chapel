/*
 * Copyright 2020-2025 Hewlett Packard Enterprise Development LP
 * Copyright 2004-2019 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
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

// checkResolved.cpp

#include "passes.h"

#include "astutil.h"
#include "CatchStmt.h"
#include "fcf-support.h"
#include "DecoratedClassType.h"
#include "driver.h"
#include "expr.h"
#include "iterator.h"
#include "stmt.h"
#include "stlUtil.h"
#include "stringutil.h"
#include "type.h"
#include "TryStmt.h"
#include "wellknown.h"

#include "global-ast-vecs.h"

#include <set>

// We use RefVector to store a list of symbols that are aliases for the return
// symbol being sought in isDefinedAllPaths.
// We assume that if the ref is being used then it is valid (resolution should
// ensure this).
typedef std::set<Symbol*> RefSet;

static void checkConstLoops();
static int isDefinedAllPaths(Expr* expr, Symbol* ret, RefSet& refs);
static void checkReturnPaths(FnSymbol* fn);
static void checkCalls();
static void checkExternProcs();
static void checkExportedProcs();
static void checkTheseWithArguments();

static void
checkConstLoops() {
  if (fWarnConstLoops == true) {
    forv_Vec(BlockStmt, block, gBlockStmts) {
      block->checkConstLoops();
    }
  }
}

static void checkForClassAssignOps(FnSymbol* fn) {
  if (fn->getModule()->modTag == MOD_USER) {
    if (strcmp(fn->name, "=") == 0 &&
        fn->formals.head) {
      ArgSymbol* formal = toArgSymbol(toDefExpr(fn->formals.head)->sym);
      Type* formalType = formal->type->getValType();
      if (isOwnedOrSharedOrBorrowed(formalType) ||
          isUnmanagedClass(formalType)) {
        USR_FATAL_CONT(fn, "Can't overload assignments for class types");
      }
    }
  }
}

// This function is checking for any AggregateType (record/class/union) which
// contains a non-default initializable/non-copyable field
// (sync/atomic), but fails to explicitly define either `init` or
// `init=`. This includes having a field which is a container for that type
// like an array or tuple.
static void checkSyncAtomicDefaultInit() {
  for_alive_in_Vec(AggregateType, at, gAggregateTypes) {
    bool hasCompilerGeneratedInit = false;
    bool hasCompilerGeneratedCopyInit = false;
    for (auto fn : at->methods) {
      if (fn && fn->isDefaultInit()) {
        hasCompilerGeneratedInit = true;
        break;
      }
      if (fn && fn->isDefaultCopyInit()) {
        hasCompilerGeneratedCopyInit = true;
        break;
      }
    }

    if (hasCompilerGeneratedInit || hasCompilerGeneratedCopyInit) {
      for_fields(field, at) {
        bool isSync = isOrContainsSyncType(field->type);
        bool isAtomic = isOrContainsAtomicType(field->type);
        if (isSync || isAtomic) {
          USR_WARN(at,
                  "compiler generated default initializers for %s with '%s' "
                  "fields are deprecated, please supply an 'init%s' method",
                  at->isClass() ? "classes" : (at->isRecord() ? "records" : "unions"),
                  isSync ? "sync" :  "atomic", hasCompilerGeneratedCopyInit ? "=" : "");
        }
      }
    }
  }

}


// This function is checking for any function which returns a non-default
// copyable type (sync/atomic). This has exclusions for functions that
// don't copy like array aliases or explicit "no copy" functions. This
// includes functions which return a type which is a container for that type
// like an array or tuple.
static void checkSyncAtomicReturnByCopy() {

  const char* astrCompilerCopySync = astr("chpl__compilerGeneratedCopySync");

  //checks for return by anything by ref
  for_alive_in_Vec(FnSymbol, fn, gFnSymbols) {

    // skip functions which support deprecation
    if (fn->name == astrCompilerCopySync) continue;
    if (fn->hasFlag(FLAG_DEPRECATED)) continue;

    bool isSync = isOrContainsSyncType(fn->retType, false);
    bool isAtomic = isOrContainsAtomicType(fn->retType, false);
    bool isRef = fn->returnsRefOrConstRef() || fn->retType->isRef();

    bool isInitAutoCopy = fn->hasEitherFlag(FLAG_INIT_COPY_FN, FLAG_AUTO_COPY_FN);
    bool isNoCopy = fn->hasEitherFlag(FLAG_NO_COPY, FLAG_NO_COPY_RETURN) || fn->hasFlag(FLAG_NO_COPY_RETURNS_OWNED);
    bool isCoerce = fn->hasFlag(FLAG_COERCE_FN);
    bool isDefaultOf = fn->name == astr_defaultOf;
    bool isAliasing = fn->hasFlag(FLAG_RETURNS_ALIASING_ARRAY);
    bool isDefaultActualFn = fn->hasFlag(FLAG_DEFAULT_ACTUAL_FUNCTION);
    bool optOut = isInitAutoCopy || isNoCopy ||
                  isCoerce || isDefaultOf || isAliasing || isDefaultActualFn;

    bool shouldWarn = !optOut && !isRef && (isSync || isAtomic);

    if (shouldWarn) {
      USR_WARN(fn,
               "returning a%s by %s is deprecated",
               isSync ? " sync" : "n atomic",
               retTagDescrString(fn->retTag));
    }
  }
}

void
checkResolved() {
  forv_Vec(FnSymbol, fn, gFnSymbols) {
    checkForClassAssignOps(fn);
    checkReturnPaths(fn);
    if (fn->retType->symbol->hasFlag(FLAG_ITERATOR_RECORD) &&
        !fn->isIterator()) {
      IteratorInfo* ii = toAggregateType(fn->retType)->iteratorInfo;
      if (ii && ii->iterator &&
          ii->iterator->defPoint->parentSymbol == fn &&
          fn->hasFlag(FLAG_COMPILER_GENERATED) == false) {
        // This error isn't really possible in regular code anymore,
        // since you have to have FLAG_FN_RETURNS_ITERATOR / that pragma
        // to generate it. (Otherwise the iterator expression is turned
        // into an array in the process of being returned).
        // If we remove FLAG_FN_RETURNS_ITERATOR, we should remove this error.
        USR_FATAL_CONT(fn, "functions cannot return nested iterators or loop expressions");
      }
    }

    // MPF note: the below error will be thrown if += is promoted
    // (e.g. over an array). But, that is probably an error anyway,
    // because currently there are array += scalar overloads and if
    // one of these is promoted, we get too many additions.
    if (fn->hasFlag(FLAG_ASSIGNOP) && fn->retType != dtVoid)
      USR_FATAL(fn, "The return value of an assignment operator must be 'void'.");
  }

  forv_Vec(TypeSymbol, type, gTypeSymbols) {
    if (EnumType* et = toEnumType(type->type)) {
      std::set<std::string> enumVals;
      for_enums(def, et) {
        if (def->init) {
          SymExpr* sym = toSymExpr(def->init);
          if (!sym || (!sym->symbol()->hasFlag(FLAG_PARAM) &&
                       !toVarSymbol(sym->symbol())->immediate)) {
            USR_FATAL_CONT(def, "enumerator '%s' is not an integer param value",
                           def->sym->name);
          } else if (fWarnUnstable) {
            Immediate* imm = toVarSymbol(sym->symbol())->immediate;
            std::string enumVal = imm->to_string();
            if (enumVals.count(enumVal) != 0) {
              USR_WARN(sym, "it has been suggested that support for enums "
                       "with duplicate integer values should be deprecated, "
                       "so this enum could be considered unstable; if you "
                       "value such enums, please let the Chapel team know.");
              break;
            }
            enumVals.insert(enumVal);
          }
        }
      }
    }
  }
  // check for no record deletes, no invalid PRIM_ADDR_OF
  checkCalls();
  checkConstLoops();
  checkExternProcs();
  checkExportedProcs();

  checkSyncAtomicDefaultInit();
  checkSyncAtomicReturnByCopy();

  checkTheseWithArguments();
}


// This routine returns '0' if we can find a path through the given
// expression that does not return (assign to 'ret'), halt, throw,
// etc.  I.e., if there is a path that would constitute an error for a
// function that was meant to return something and is not.  It returns
// non-zero if all paths are covered, and the result _may_ indicate
// something about the smallest number of definitions of 'ret' along
// any path through the expression (though the behavior of throws,
// halts, etc. may influence that number...).  In the only use-case in
// our compiler, we are currently only relying on zero / non-zero
// behavior and care should be taken before reading too much into the
// non-zero value.
static int
isDefinedAllPaths(Expr* expr, Symbol* ret, RefSet& refs)
{
  if (!expr)
    return 0;

  if (isDefExpr(expr))
    return 0;

  if (isSymExpr(expr))
    return 0;

  if (ret == gNone)
    return 1;

  if (CallExpr* call = toCallExpr(expr))
  {
    if (call->isResolved() &&
        call->resolvedFunction()->hasFlag(FLAG_FUNCTION_TERMINATES_PROGRAM))
      return 1;

    if (call->isPrimitive(PRIM_RT_ERROR))
      return 1;

    if (call->isPrimitive(PRIM_THROW)) {
      return 1;
    }

    if (call->isPrimitive(PRIM_MOVE) ||
        call->isPrimitive(PRIM_ASSIGN))
    {
      SymExpr* lhs = toSymExpr(call->get(1));

      if (lhs->symbol() == ret)
        return 1;

      if (refs.find(lhs->symbol()) != refs.end())
        return 1;

      if (CallExpr* rhs = toCallExpr(call->get(2)))
        if (rhs->isPrimitive(PRIM_ADDR_OF))
        {
          // We expect only a SymExpr as the operand of 'addr of'.
          SymExpr* se = toSymExpr(rhs->get(1));
          if (se->symbol() == ret)
            // lhs <- ('addr of' ret)
            refs.insert(lhs->symbol());
        }
    }

    if (call->isResolved())
    {
      for_alist(e, call->argList)
      {
        if (SymExpr* se = toSymExpr(e))
        {
          ArgSymbol* arg = actual_to_formal(se);
          // If ret is passed as an out or inout argument, that's a definition.
          if (se->symbol() == ret &&
              (arg->intent == INTENT_OUT ||
               arg->intent == INTENT_INOUT ||
               arg->intent == INTENT_REF))
            return 1;
        }
      }
    }

    return 0;
  }

  if (CondStmt* cond = toCondStmt(expr)) {
    return std::min(isDefinedAllPaths(cond->thenStmt, ret, refs),
                    isDefinedAllPaths(cond->elseStmt, ret, refs));
  }

  if (isGotoStmt(expr))
    return 0;

  // bodies of Defer statements will be moved, so treat them as
  // not defining anything
  if (isDeferStmt(expr))
    return 0;

  if (TryStmt* tryStmt = toTryStmt(expr))
  {
    int result = INT_MAX;

    // This indicates whether or not we find a catch-all case, in
    // which case nothing can escape us unless the individual clauses
    // let it; if this is a try! statement, it doesn't need a
    // catch-all case, so set it to true
    //
    bool foundCatchall = tryStmt->tryBang();
    for_alist(c, tryStmt->_catches) {
      result = std::min(result, isDefinedAllPaths(c, ret, refs));
      if (toCatchStmt(c)->isCatchall()) {
        foundCatchall = true;
      }
    }

    result = std::min(result, isDefinedAllPaths(tryStmt->body(), ret, refs));

    // even if the try and all catches are air-tight, if there's no
    // catch-all, we can escape via an uncaught error, and will need
    // for our parent statement to contain returns as well...
    if (result == 1 && !foundCatchall) {
      result = 0;
    }
    return result;
  }

  if (CatchStmt* catchStmt = toCatchStmt(expr)) {
    return isDefinedAllPaths(catchStmt->bodyWithoutTest(), ret, refs);
  }

  if (BlockStmt* block = toBlockStmt(expr))
  {
    // NOAKES 2014/11/25 Transitional.  Ensure we don't call blockInfoGet()
    if (block->isWhileDoStmt() ||
        block->isForLoop()     ||
        block->isCForLoop()    ||
        block->isParamForLoop())
    {
      return 0;
    }
    else if (block->isDoWhileStmt()        ||
             block->blockInfoGet() == NULL ||
             block->blockInfoGet()->isPrimitive(PRIM_BLOCK_LOCAL)) {
      int result = 0;

      for_alist(e, block->body)
        result += isDefinedAllPaths(e, ret, refs);

      return result;
    }
    else
    {
      return 0;
    }
  }

  if (isForallStmt(expr))
    return 0;

  if (isExternBlockStmt(expr))
    return 0;

  INT_FATAL("isDefinedAllPaths: Unhandled case.");
  return 0;
}

// This function helps when checking that we aren't returning
// a local variable by reference. In particular, it checks for two
// cases:
//   * returning a ref-intent argument by ref
//   * returning a const-ref-intent argument by const ref
//
static bool
returnsRefArgumentByRef(CallExpr* returnedCall, FnSymbol* fn)
{
  INT_ASSERT(returnedCall->isPrimitive(PRIM_ADDR_OF));
  if (SymExpr* rhs = toSymExpr(returnedCall->get(1))) {
    if (ArgSymbol* formal = toArgSymbol(rhs->symbol())) {
      IntentTag intent = formal->intent;
      if (fn->retTag == RET_CONST_REF && (intent & INTENT_FLAG_REF))
        return true;
      else if(fn->retTag == RET_REF &&
              (intent == INTENT_REF || intent == INTENT_REF_MAYBE_CONST))
        return true;
    }
  }

  return false;
}

// This function finds the original Symbol that a local array
// refers to (through aliasing or slicing).
// It returns the number of Exprs added to sources.
static int findOriginalArrays(FnSymbol*        fn,
                              Symbol*          sym,
                              std::set<Expr*>& sources) {
  int ret = 0;

  for_SymbolSymExprs(se, sym) {
    Expr* stmt = se->getStmtExpr();

    if (CallExpr* call = toCallExpr(stmt)) {
      if (call->isPrimitive(PRIM_MOVE)   == true  ||
          call->isPrimitive(PRIM_ASSIGN) == true) {
        Expr* lhs = call->get(1);

        if (se == lhs) {
          Expr* rhs = call->get(2);

          // Handle the following cases:
          //   rhs is a call_tmp -> recurse on the call_tmp
          //   rhs is a call to a function returning an aliasing array ->
          //      recurse into the source array argument
          if (SymExpr* rhsSe = toSymExpr(rhs)) {
            VarSymbol* rhsSym = toVarSymbol(rhsSe->symbol());
            // is RHS a local variable (user or temp)?
            // if so, find the definitions for that, and further
            // traverse if they are aliases.
            if (rhsSym                          != NULL &&
                rhsSym->defPoint->getFunction() ==   fn &&
                rhsSym->hasFlag(FLAG_TEMP)      == true) {
              ret += findOriginalArrays(fn, rhsSym, sources);
            }

          } else if (CallExpr* rhsCall = toCallExpr(rhs)) {
            FnSymbol* calledFn = rhsCall->resolvedFunction();
            SymExpr*  aliased  = NULL; // the array that is sliced or aliased

            if (calledFn && calledFn->hasFlag(FLAG_RETURNS_ALIASING_ARRAY)) {
              aliased = toSymExpr(rhsCall->get(1));
            }

            if (aliased) {
              int got = 0;

              if (aliased->symbol()->defPoint->getFunction() == fn) {
                // further traverse if aliased was a local variable.
                got = findOriginalArrays(fn, aliased->symbol(), sources);
              }

              if (got == 0) {
                // If we didn't find another local source array, add
                // aliased as the source.
                got = 1;
                sources.insert(aliased);
              }

              ret += got;
            }
          }
        }
      }
    }
  }

  return ret;
}

static void
checkBadLocalReturn(FnSymbol* fn, Symbol* retVar) {

  for_SymbolSymExprs(se, retVar) {
    // se is a use or def of 'ret'
    Expr* stmt = se->getStmtExpr();
    if (CallExpr* call = toCallExpr(stmt)) {
      if (call->isPrimitive(PRIM_MOVE) || call->isPrimitive(PRIM_ASSIGN)) {
        Expr* lhs = call->get(1);
        Expr* rhs = call->get(2);
        CallExpr* rhsCall = toCallExpr(rhs);
        // is the se on the LHS of the PRIM_MOVE/PRIM_ASSIGN
        // i.e. is it a definition?
        if (se == lhs) {
          // Are we returning a variable by ref?
          if (rhsCall && rhsCall->isPrimitive(PRIM_ADDR_OF)) {
            // What variable are we returning with PRIM_ADDR_OF?
            SymExpr* ret = toSymExpr(rhsCall->get(1));
            // Check: Are we returning a local variable by ref?
            if (ret->symbol()->defPoint->getFunction() == fn &&
                !returnsRefArgumentByRef(rhsCall, fn)) {
              USR_FATAL_CONT(ret, "illegal expression to return by ref");
            } else {
              // Check: Are we returning a constant by ref?
              if (fn->retTag == RET_REF &&
                  (ret->symbol()->isConstant() ||
                   ret->symbol()->isParameter())) {
                USR_FATAL_CONT(rhs, "function cannot return constant by ref");
              }
            }
          }
        }
      }
    }
  }

  // if it's an array, collect the definitions for the
  // returned array. We need to go in to chains of
  // array slicing or aliasing as well as call_tmp copies.

  std::set<Expr*> sources;
  if (retVar->typeInfo()->symbol->hasFlag(FLAG_ARRAY)) {
    findOriginalArrays(fn, retVar, sources);

    for_set(Expr, source, sources) {
      // Check: Are we returning a slice or alias of a local variable
      // by value? (The above code should have found the case returning
      // such things by ref).
      SymExpr* rhsSe = toSymExpr(source);
      if (rhsSe &&
          isVarSymbol(rhsSe->symbol()) &&
          rhsSe->symbol()->defPoint->getFunction() == fn &&
          !rhsSe->isRef()) {
        USR_FATAL_CONT(rhsSe, "illegal return of array aliasing a local array");
      }
    }
  }
}

static void
checkReturnPaths(FnSymbol* fn) {
  // Check to see if the function returns a value.
  //
  // FLAG_THUNK_BUILDER is analogous to isIterator (both produce a record
  // that contains outer variables and other information), so they are
  // next to each other in the if statement. Similarly, FLAG_THUNK_INVOKE
  // is analogous to FLAG_AUTO_II (both mark compiler-generated methods
  // on the thunk record / iterator record that are filled in late in
  // compilation)
  if (fn->isIterator() ||
      fn->hasFlag(FLAG_THUNK_BUILDER) ||
      !strcmp(fn->name, "=") || // TODO: Remove this to enforce new signature.
      !strcmp(fn->name, "chpl__buildArrayRuntimeType") ||
      fn->retTag == RET_TYPE ||
      fn->hasFlag(FLAG_EXTERN) ||
      fn->hasFlag(FLAG_INIT_TUPLE) ||
      fn->hasFlag(FLAG_AUTO_II) ||
      fn->hasFlag(FLAG_THUNK_INVOKE))
    return; // No.

  if (fn->retType == dtVoid) {
    if (fn->returnsRefOrConstRef()) {
      USR_FATAL_CONT(fn, "function declared 'ref' but does not return anything");
    }
    return; // Doesn't return a value, no need to look for returns.
  }

  // Check to see if the returned value is initialized.
  Symbol* ret = fn->getReturnSymbol();
  VarSymbol* var = toVarSymbol(ret);
  if (var)
  {
    // If it has an immediate initializer, it is initialized.
    if (var->immediate)
      return;
  }

  if (isEnumSymbol(ret))
    return;

  RefSet refs;
  int result = isDefinedAllPaths(fn->body, ret, refs);

  //
  // Issue a warning if there is a path that has zero definitions.
  //
  if (result == 0)
    USR_FATAL_CONT(fn->body, "control reaches end of function that returns a value");

  //
  // Issue an error if returning a local variable by ref.
  // Issue an error if returning a slice of a local variable.
  //
  if (!fn->hasFlag(FLAG_WRAPPER)) {
    // Add returned aliases to refs.
    checkBadLocalReturn(fn, ret);
    for_set(Symbol, alias, refs) {
      checkBadLocalReturn(fn, alias);
    }
  }
}

static void checkIteratorContextPrimitives(CallExpr* call) {
  if (call->isPrimitive(PRIM_INNERMOST_CONTEXT) ||
      call->isPrimitive(PRIM_OUTER_CONTEXT)     ||
      call->isPrimitive(PRIM_HOIST_TO_CONTEXT)  )
    USR_FATAL_CONT(call,
      "use of this feature requires compiling with --iterator-contexts");
}

static void
checkBadAddrOf(CallExpr* call)
{
  if (call->isPrimitive(PRIM_ADDR_OF)) {
    // This test is turned off if we are in a wrapper function.
    FnSymbol* fn = call->getFunction();
    if (!fn->hasFlag(FLAG_WRAPPER)) {
      SymExpr* lhs = NULL;

      if (CallExpr* move = toCallExpr(call->parentExpr))
        if (move->isPrimitive(PRIM_MOVE))
          lhs = toSymExpr(move->get(1));

      //
      // check that the operand of 'addr of' is a legal lvalue.
      if (SymExpr* rhs = toSymExpr(call->get(1))) {

        bool lhsRef   = lhs && lhs->symbol()->hasFlag(FLAG_REF_VAR);
        bool lhsConst = lhs && lhs->symbol()->hasFlag(FLAG_CONST);

        bool rhsType = rhs->symbol()->hasFlag(FLAG_TYPE_VARIABLE);
        bool rhsParam = rhs->symbol()->isParameter();

        bool rhsExprTemp = rhs->symbol()->hasFlag(FLAG_EXPR_TEMP) &&
                           !rhs->symbol()->type->symbol->hasFlag(FLAG_ARRAY);

        // Also detect runtime type variables
        if (rhs->symbol()->type->symbol->hasFlag(FLAG_RUNTIME_TYPE_VALUE))
          rhsType = true;

        if (lhsRef && rhsType) {
          USR_FATAL_CONT(call, "Cannot set a reference to a type variable.");
        } else if (lhsRef && rhsParam) {
          USR_FATAL_CONT(call, "Cannot set a reference to a param variable.");
        } else if (lhsRef && !lhsConst) {
          if (rhsExprTemp || rhs->symbol()->isConstant()) {
            USR_FATAL_CONT(call, "Cannot set a non-const reference to a const variable.");
          }
        }
      }
    }
  }
}


static void
checkCalls()
{
  forv_Vec(CallExpr, call, gCallExprs) {
    checkBadAddrOf(call);
    if (! fIteratorContexts)
      checkIteratorContextPrimitives(call);
  }
}

// This function checks that the passed type is an acceptable
// argument/return type for an extern/export function.
//
// Note that some export functions use wrappers (e.g. for string arguments)
// that will have already been processed by this point in compilation.
// In that event, this code serves to check that a case has not been
// missed in the wrapper generation.
static void externExportTypeError(FnSymbol* fn, Type* t) {
  INT_ASSERT(fn->hasFlag(FLAG_EXTERN) || fn->hasFlag(FLAG_EXPORT));

  bool isExtern = fn->hasFlag(FLAG_EXTERN);

  if (!fn->hasFlag(FLAG_INSTANTIATED_GENERIC)) {
    if (t == dtString) {
      if (isExtern)
        USR_FATAL_CONT(fn, "extern procedures should not take arguments of "
                           "type string, use c_ptrConst(c_char) instead");
      else
        USR_FATAL_CONT(fn, "export procedures should not take arguments of "
                           "type string, use c_ptrConst(c_char) instead");
    } else {
      if (isExtern)
        USR_FATAL_CONT(fn, "extern procedure argument types should be "
                           "extern types - '%s' is not",
                           toString(t));
      else
        USR_FATAL_CONT(fn, "export procedure argument types should be "
                           "exportable types - '%s' is not",
                           toString(t));
    }
  } else {
    // This is a generic instantiation of an extern proc that is using
    // string, so we want to report the call sites causing this
    if (t == dtString) {
      if (isExtern)
        USR_FATAL_CONT(fn, "extern procedure has arguments of type string");
      else
        USR_FATAL_CONT(fn, "export procedure has arguments of type string");
    } else {
      if (isExtern)
        USR_FATAL_CONT(fn, "extern procedure argument types should be "
                           "extern types - '%s' is not",
                           toString(t));
      else
        USR_FATAL_CONT(fn, "export procedure argument types should be "
                           "exportable types - '%s' is not",
                           toString(t));
    }

    forv_Vec(CallExpr, call, *fn->calledBy) {
      USR_PRINT(call, "when instantiated from here");
    }

    if (t == dtString)
      USR_PRINT(fn, "use c_ptrConst(c_char) instead");
  }
}

static bool isErroneousExternExportArgIntent(ArgSymbol* formal) {

  Type* valType = formal->getValType();

  // workaround for issue #15917
  if (valType == dtExternalArray || valType == dtOpaqueArray)
    return false;

  if (valType->symbol->hasFlag(FLAG_FUNCTION_CLASS)) return false;

  return isRecord(valType) &&
         (formal->originalIntent == INTENT_BLANK ||
          formal->originalIntent == INTENT_CONST ||
          formal->originalIntent == INTENT_INOUT ||
          formal->originalIntent == INTENT_OUT);
}



static void externExportIntentError(FnSymbol* fn, ArgSymbol* arg) {
  INT_ASSERT(fn->hasFlag(FLAG_EXTERN) || fn->hasFlag(FLAG_EXPORT));

  bool isExtern = fn->hasFlag(FLAG_EXTERN);

  IntentTag intent = arg->originalIntent;

  if (intent == INTENT_BLANK || intent == INTENT_CONST) {
    USR_FATAL_CONT(arg, "a concrete intent is required for the "
                        "%s function formal '%s' "
                        "which has record type '%s'",
                        isExtern ? "extern" : "exported",
                        arg->name,
                        toString(arg->getValType()));
  } else if (intent == INTENT_INOUT || intent == INTENT_OUT) {
    USR_FATAL_CONT(arg, "%s is not yet supported for %s functions - "
                        "consider changing formal '%s' to use 'ref' intent",
                        intentDescrString(intent),
                        isExtern ? "extern" : "exported",
                        arg->name);
  }
}

static void checkExternProcs() {
  const char* sizeof_ = astr("sizeof");
  const char* alignof_ = astr("alignof");
  const char* offsetof_ = astr("offsetof");
  const char* c_pointer_return = astr("c_pointer_return");

  forv_Vec(FnSymbol, fn, gFnSymbols) {
    if (!fn->hasFlag(FLAG_EXTERN))
      continue;

    // Don't worry about passing Chapel types to sizeof etc.
    if (fn->cname == sizeof_ ||
        fn->cname == alignof_ ||
        fn->cname == offsetof_ ||
        fn->cname == c_pointer_return)
      continue;

    for_formals(formal, fn) {
      if (!isExternType(formal->type)) {
        externExportTypeError(fn, formal->type);
      } else if (isErroneousExternExportArgIntent(formal)) {
        externExportIntentError(fn, formal);
      }
    }

    if (!isExternType(fn->retType)) {
      externExportTypeError(fn, fn->retType);
    }

    if (fn->retType->symbol->hasFlag(FLAG_C_ARRAY)) {
      USR_FATAL_CONT(fn, "extern procedures should not return c_array");
    }
  }
}

static void checkExportedProcs() {
  forv_Vec(FnSymbol, fn, gFnSymbols) {
    if (!fn->hasFlag(FLAG_EXPORT))
      continue;

    for_formals(formal, fn) {
      if (!isExportableType(formal->type)) {
        externExportTypeError(fn, formal->type);
      } else if (isErroneousExternExportArgIntent(formal)) {
        externExportIntentError(fn, formal);
      }
    }

    if (!isExportableType(fn->retType)) {
      externExportTypeError(fn, fn->retType);
    }

    if (fn->retType->symbol->hasFlag(FLAG_C_ARRAY)) {
      USR_FATAL_CONT(fn, "exported procedures should not return c_array");
    }
  }
}

static bool isTheseIterator(FnSymbol* fn) {
  return fn->isIterator() && fn->isMethod() && fn->name == astrThese;
}
static bool hasIterTag(FnSymbol* fn, Symbol* iterKind) {
  if (Symbol* tag = fn->getSubstitutionWithName(astr("tag"))) {
    return tag->type == iterKind->type && tag->name == iterKind->name;
  }
  return false;
}
static bool isParallelTheseIterator(FnSymbol* fn) {
  return isTheseIterator(fn) &&
          (hasIterTag(fn, gStandaloneTag) ||
           hasIterTag(fn, gLeaderTag) ||
           hasIterTag(fn, gFollowerTag));
}
static bool isStandaloneTheseIterator(FnSymbol* fn) {
  return isTheseIterator(fn) && hasIterTag(fn, gStandaloneTag);
}
static bool isLeaderTheseIterator(FnSymbol* fn) {
  return isTheseIterator(fn) && hasIterTag(fn, gLeaderTag);
}
static bool isFollowerTheseIterator(FnSymbol* fn) {
  return isTheseIterator(fn) && hasIterTag(fn, gFollowerTag);
}
static bool isSerialTheseIterator(FnSymbol* fn) {
  return isTheseIterator(fn) && !isParallelTheseIterator(fn);
}

static void checkTheseWithArguments() {
  // only do these checks if `--warn-unstable`
  if (!fWarnUnstable) return;

  // keep track of if we have run these checks,
  // because checkResolved is called multiple times with `--verify`
  // and these should only run once
  static bool hasPerformedChecks = false;
  if (hasPerformedChecks) return;

  for_alive_in_Vec(FnSymbol, fn, gFnSymbols) {
    if (shouldWarnUnstableFor(fn)) {
      if (isSerialTheseIterator(fn) && fn->numFormals() > 1) {
        USR_WARN(fn,
                 "defining a serial 'these' iterator that takes arguments "
                 "is unstable and may change in the future");
      } else if (isStandaloneTheseIterator(fn) && fn->numFormals() > 1) {
        USR_WARN(fn,
                 "defining a parallel 'these' standalone iterator that takes "
                 "extra arguments is unstable and may change in the future");
      } else if (isLeaderTheseIterator(fn) && fn->numFormals() > 1) {
        USR_WARN(fn,
                 "defining a parallel 'these' leader iterator that takes "
                 "extra arguments is unstable and may change in the future");
      } else if (isFollowerTheseIterator(fn) && fn->numFormals() > 2) {
        USR_WARN(fn,
                 "defining a parallel 'these' follower iterator that takes "
                 "extra arguments is unstable and may change in the future");
      }
    }
  }
  hasPerformedChecks = true;
}

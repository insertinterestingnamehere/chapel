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

#include "passes.h"


#include "astutil.h"
#include "baseAST.h"
#include "CForLoop.h"
#include "driver.h"
#include "expr.h"
#include "exprAnalysis.h"
#include "LoopStmt.h"
#include "optimizations.h"
#include "stlUtil.h"
#include "stmt.h"
#include "symbol.h"
#include "type.h"
#include "WhileStmt.h"

#include "global-ast-vecs.h"

static void undoReturnByRef(FnSymbol* fn);
static void collapseTrivialMoves();

//helper datastructures/types
typedef std::pair<Expr*, Type*> DefCastPair;
typedef std::map<SymExpr*, DefCastPair> UseDefCastMap;

//prototypes
bool primMoveGeneratesCommCall(CallExpr* ce);
inline bool unsafeExprInBetween(Expr* e1, Expr* e2, Expr* exprToMove,
    SafeExprAnalysis& analysisData);
inline bool requiresCast(Type* t);
inline bool isArithmeticPrimitive(CallExpr *ce);
inline bool isFloatComparisonPrimitive(CallExpr *ce);
bool isDenormalizable(Symbol* sym,
    SymExpr** useOut, Expr** defOut,
    Type** castTo, SafeExprAnalysis& analysisData);
void findCandidatesInFunc(FnSymbol *fn, UseDefCastMap& candidates,
    SafeExprAnalysis& analysisData);
void findCandidatesInFuncOnlySym(FnSymbol* fn, std::set<Symbol*> symVec,
    UseDefCastMap& udcMap, SafeExprAnalysis& analysisData);
void denormalize(void);
void denormalize(Expr* def, SymExpr* use, Type* castTo);
void denormalizeOrDeferCandidates(UseDefCastMap& candidates,
    std::set<Symbol*>& deferredSyms);

int maxDenormalizesPerFunction = 1000;

/*
 * This function tries to remove temporary variables in function `fn`
 *
 * A local variable is removed if:
 *
 * - It is def'd and use'd once
 *
 * - Its def is a PRIM_MOVE or PRIM_ASSIGN, with no possible
 *   communication
 * - RHS and LHS are of same type and non-extern
 *
 * - Its use is a suitable primitive
 * - Its use is not repeated(condition/increment statement of a loop)
 *
 * Denormalization uses helpers in util/exprAnalysis.cpp to decide if
 * it's safe to move function calls and primitives.
 */
void denormalize(void) {

  UseDefCastMap candidates;
  std::set<Symbol*> deferredSyms;
  SafeExprAnalysis analysisData;

  if(fDenormalize) {
    forv_Vec(FnSymbol, fn, gFnSymbols) {
      // remove unused epilogue labels
      removeUnnecessaryGotos(fn, true);
      if (!fReturnByRef && fn->hasFlag(FLAG_FN_RETARG))
        undoReturnByRef(fn);

      bool isFirstRound = true;
      do {
        candidates.clear();
        deferredSyms.clear();

        // if we are analyzing locals in a function for the first time,
        // we look at all the symbols in the function. otherwise we only
        // look at the ones deferred in the previous passes on the same
        // function
        if(isFirstRound) {
          findCandidatesInFunc(fn, candidates, analysisData);
        }
        else {
          findCandidatesInFuncOnlySym(fn, deferredSyms, candidates,
              analysisData);
        }

        denormalizeOrDeferCandidates(candidates, deferredSyms);

        isFirstRound = false;
      } while(deferredSyms.size() > 0);
    }

    collapseTrivialMoves();
  }
}

/*
 * deferring denormalizing over some temporaries and the way it's done
 * is as follows:
 *
 * Consider following snippet:
 *
 *   var t1, t2, t3;
 *   t1 = f();
 *   t2 = t1;
 *   t3 = t2;
 *
 *   and corresponding pseudo-AST
 *
 *   (1 move (2 SymExpr t1), (3 CallExpr f))
 *   (4 move (5 SymExpr t2), (6 SymExpr t1))
 *   (7 move (8 SymExpr t3), (9 SymExpr t2))
 *
 *   In first run we will have a map as follows (use,def expr keypair)
 *
 *   use               def
 *   --------------------------------
 *   (6 SymExpr t1)    (3 CallExpr f)
 *   (9 SymExpr t2)    (6 SymExpr t1)
 *
 *   Than in denormalizeOrDeferCandidates, assume we denormalize (t1,f)
 *   pair first, which will replace node 6 with 3 and remove 1
 *   altogether. At that point when we try to denormalize the second
 *   pair(t2,t1) in our list, node id 6 will still be in the map but
 *   it's parent pointer will be NULL, since it's removed by
 *   6->replace(3) operation in the previous iteration.
 *
 *   [I might be leaking compiler memory in the logic I just described]
 *
 *   At that point, we still know that t2 is a good candidate for
 *   removal, but its def has changed, and we need to reanalyze for
 *   safety. So we add t2 to deferredSyms for future iterations. We can
 *   get rid of deferredSyms, but that would mean reiterating all
 *   symbols in the function. And I am almost certain that we can never
 *   generate new candidates once we start denormalizing. So, we are
 *   generating candidates once, and iterating over and over until we
 *   don't defer anything(thus do{}while(deferredSyms.count()>0)) in
 *   denormalize(void)
 */
void denormalizeOrDeferCandidates(UseDefCastMap& candidates,
    std::set<Symbol*>& deferredSyms) {

  for(UseDefCastMap::iterator it = candidates.begin() ;
      it != candidates.end() ; ++it) {
    // unpack the bundle
    DefCastPair defCastPair = it->second;
    SymExpr* use = it->first;
    Expr* def = defCastPair.first;
    Type* castTo = defCastPair.second;

    if(def->parentExpr == NULL) {
      deferredSyms.insert(use->symbol());
      continue;
    }
    denormalize(def, use, castTo);
  }
}

void findCandidatesInFuncOnlySym(FnSymbol* fn, std::set<Symbol*> symVec,
    UseDefCastMap& udcMap, SafeExprAnalysis& analysisData) {

  bool cachedGlobalManip = analysisData.isRegisteredGlobalManip(fn);
  bool cachedExternManip = analysisData.isRegisteredExternManip(fn);

  // Limit the denormalization candidates to avoid
  // too much recursion in code generation
  int limit = maxDenormalizesPerFunction;
  int found = 0;

  for_set(Symbol, sym, symVec) {

    SymExpr *use = NULL;
    Expr *def = NULL;
    Type* castTo = NULL;

    //if we don't already have it cached,
    //check for global symbols in function body
    if(!cachedGlobalManip) {
      // I am not sure if we can check for const and param here.
      // Touching a global const and param sounds safe to me, but I am
      // not certain especially with nonprimitive types. Playing safe
      // for now. Engin
      if(sym && !sym->isImmediate() && isGlobal(sym)){
        analysisData.registerGlobalManip(fn, true);
        cachedGlobalManip = true;
      }
    }

    if(!cachedExternManip) {
      if(sym && sym->hasFlag(FLAG_EXTERN)){
        analysisData.registerExternManip(fn, true);
        cachedExternManip = true;
      }

    }

    if(isDenormalizable(sym, &use, &def, &castTo, analysisData)) {

      // Initially I used to defer denormalizing actuals and have
      // special treatment while denormalizing actuals of a function
      // call. Main reason behind that was C standard not specifying
      // evaluation order of actuals and I wanted to keep the call order
      // in the IR. However, if we have strong enough safety checks on
      // `Expr`s that we are moving, than that shouldn't necessarily
      // matter.
      //
      // Possible alternative for an easy-to-implement actual
      // denormalization is to denormalize an actual only if it's the
      // last/only one.

      //denormalize if the def is safe to move and there is no unsafe
      //function between use and def
      if(analysisData.exprHasNoSideEffects(def, NULL)) {
        if(!unsafeExprInBetween(def, use, def, analysisData)) {
          DefCastPair defCastPair(def, castTo);
          udcMap.insert(std::pair<SymExpr*, DefCastPair>
              (use, defCastPair));
          found++;
        }
      }
    }

    // Stop if we've found too many.
    if (found >= limit)
      break;

  } // end loop for symbol
  if(!cachedGlobalManip) {
    analysisData.registerGlobalManip(fn, false);
  }
  if(!cachedExternManip) {
    analysisData.registerExternManip(fn, false);
  }
}

void findCandidatesInFunc(FnSymbol *fn, UseDefCastMap& udcMap,
    SafeExprAnalysis& analysisData) {

  std::set<Symbol*> symSet;

  collectSymbolSet(fn, symSet);

  findCandidatesInFuncOnlySym(fn, symSet, udcMap, analysisData);
}

static bool isBadMove(CallExpr* ce) {
  bool ret = false;

  if (ce->isPrimitive(PRIM_MOVE)) {
    Expr* lhs = ce->get(1);
    Expr* rhs = ce->get(2);
    if (lhs->typeInfo() != rhs->typeInfo()) {
      // Possible for shorthand/convenience moves where codegen will transform
      // it into something else. For example:
      //   (move myWideClass myNarrowClass)
      // will create a wide temporary for 'myNarrowClass' and assign it to
      // 'myWideClass'.
      ret = true;
    } else if ((lhs->isWideRef() && rhs->isRef()) ||
               (lhs->isRef() && rhs->isWideRef())) {
      // another wide-temporary convenience pattern
      ret = true;
    } else if (isDerefMove(ce)) {
      ret = true;
    }
  }

  return ret;
}

static ArgSymbol* formalForActual(FnSymbol* fn, Expr* firstActual,
                                  Expr* actualToMatch)
{
  Expr* formalDef = fn->formals.head;
  Expr* actual    = firstActual;

  while (formalDef != NULL && actual != NULL)
  {
    if (actual == actualToMatch)
      return toArgSymbol(toDefExpr(formalDef)->sym);

    formalDef = formalDef->next;
    actual = actual->next;
  }

  INT_ASSERT(false); // should have matched the actual
  return NULL;
}

// Return true when 'se' is passed by reference
// and we wouldn't know it just by looking at it
// i.e. se itself is not a ref.
// TODO are there any primitives where this can occur?
static bool isValPassedByRef(CallExpr* ce, SymExpr* se)
{
  if (se->symbol()->isRef())
    // It is a reference, not a value.
    return false;

  // Alas we cannot use for_formals_actuals because of
  // the extra line/file actuals. Doing it "manually" here.

  Expr* actual = ce->argList.head;
  FnSymbol* fn = ce->resolvedFunction();

  if (fn == NULL) {
    if (ce->isPrimitive(PRIM_VIRTUAL_METHOD_CALL)) {
      fn = toFnSymbol(toSymExpr(actual)->symbol());
      if (se == actual->next)
        // The class ID - not passed by ref.
        return false;
      actual = actual->next->next;
    } else {
      // Do not handle other primitives.
      return false;
    }
  }

  ArgSymbol* arg = formalForActual(fn, actual, se);
  return arg->isRef();
}

bool isDenormalizable(Symbol* sym,
    SymExpr** useOut, Expr** defOut, Type** castTo,
    SafeExprAnalysis& analysisData) {

  if(sym && !(toFnSymbol(sym) || toArgSymbol(sym) || toTypeSymbol(sym))) {
    if(sym->name != astrThis) { //avoid issue with --baseline
      SymExpr *use = NULL;
      Expr *usePar = NULL;
      Expr *def = NULL;
      Expr *defPar = NULL;

      SymExpr* singleDef = sym->getSingleDef();
      SymExpr* singleUse = sym->getSingleUse();

      if(singleDef != NULL && singleUse != NULL) { // check def-use counts
        SymExpr* se = singleDef;
        defPar = se->parentExpr;

        //defPar has to be a move without any coercion
        CallExpr* ce = toCallExpr(defPar);
        if(ce) {
          if(ce->isPrimitive(PRIM_MOVE) || ce->isPrimitive(PRIM_ASSIGN)) {
            Type* lhsType = ce->get(1)->typeInfo();
            Type* rhsType = ce->get(2)->typeInfo();
            if(lhsType == rhsType) {
              // records semantics required next if
              // More: it seems records are passed by value. denormalizing
              // record temporaries caused semantics to change. So I use a
              // wide brush to disable record denormalization. In earlier
              // passes of denormalization implementation this was only
              // checking if the temporary to be removed is an actual to a
              // function.
              if(! isRecord(lhsType)) {
                // calls to communication functions are generated during
                // codegen. ie at this time they are still PRIM_MOVEs.
                // Generated communication calls return their result in a
                // pointer argument, therefore not suitable for
                // denormalization. See function definition for more
                // comments
                if(! primMoveGeneratesCommCall(ce)) {
                  if(! (lhsType->symbol->hasFlag(FLAG_EXTERN))){
                    if(!lhsType->symbol->hasFlag(FLAG_ATOMIC_TYPE)){
                      //at this point we now that def is fine
                      def = ce->get(2);

                      //now check if we need to cast it when we move it
                      if(CallExpr* defCe = toCallExpr(def)) {
                        if(isArithmeticPrimitive(defCe)) {
                          if(requiresCast(lhsType)) {
                            *castTo = lhsType;
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }

        if(def) {
          *defOut = def;
          // we have def now find where the value is used
          SymExpr* se = singleUse;
          usePar = se->parentExpr;
          if(CallExpr* ce = toCallExpr(usePar)) {
            if( !(ce->isPrimitive(PRIM_ADDR_OF) ||
                  ce->isPrimitive(PRIM_SET_REFERENCE) ||
                  // TODO: PRIM_SET_REFERENCE?
                  //
                  // TODO: BHARSH: I added PRIM_RETURN here after seeing a case
                  // where we did something like this:
                  //   (return (get_member_value this myField))
                  // FnSymbol expects to return one symbol, so it's easier to
                  // just not denormalize the returned symbol.
                  //
                  // PRIM_ARRAY_SHIFT_BASE_POINTER sets its first argument, so
                  // we should not denormalize if 'se' is the first actual in
                  // case of AST like this:
                  //   var ret = (cast ddata(...) nil)
                  //   shift_base_pointer(ret, ...)
                  // turning into this:
                  //   shift_base_pointer((cast ddata nil), ...)
                  //
                  // TODO: Have PRIM_ARRAY_SHIFT_BASE_POINTER return instead of
                  // setting its first arg, then we can rely on PRIM_MOVE logic.
                  //
                  ce->isPrimitive(PRIM_ARRAY_GET) ||
                  ce->isPrimitive(PRIM_GET_MEMBER) ||
                  ce->isPrimitive(PRIM_DEREF) ||
                  ce->isPrimitive(PRIM_GET_MEMBER_VALUE) ||
                  ce->isPrimitive(PRIM_RETURN) ||
                  (ce->isPrimitive(PRIM_ARRAY_SHIFT_BASE_POINTER) && ce->get(1) == se) ||

                  // We want to pass symbols to kernel launches for now. This
                  // simplifies its codegen.
                  ce->isPrimitive(PRIM_GPU_KERNEL_LAUNCH) ||

                  isBadMove(ce) ||
                  isValPassedByRef(ce, se) ||
                  isFloatComparisonPrimitive(ce))) {
              use = se;
            }
          } else if (isConditionalInCondStmt(se)) {
            use = se;
          }
        }
        if(use) {
          *useOut = use;
        }

        if(!use || !def) {
          return false;
        }

        //here I have my valid use and def so far we checked specific
        //cases for them individually, now check if there is anything
        //wrong with them as a pair

        //this issue feels too specific, maybe there is
        //safer/better/more general way of doing this check
        //
        //for reference test that caused this was:
        //test/library/standard/FileSystem/bharshbarg/filer
        //
        //The issue seemed to be yielding string from an iterator
        if(CallExpr* useParentCe = toCallExpr(usePar)) {
          if(useParentCe->isPrimitive(PRIM_FTABLE_CALL)) {
            if(argMustUseCPtr(def->typeInfo())){
              return false;
            }
          }
        }

        // we have to protect repeatedly evaluated statements of loops
        // from expensive and/or unsafe CallExprs
        if(CallExpr* defCe = toCallExpr(def)){
          //nonessential primitives are safe
          if(! analysisData.isNonEssentialPrimitive(defCe)) {
            if(LoopStmt* enclLoop = LoopStmt::findEnclosingLoop(use)) {
              if(CForLoop* enclCForLoop = toCForLoop(enclLoop)) {
                if(enclCForLoop->testBlockGet()->contains(ce) ||
                    enclCForLoop->incrBlockGet()->contains(ce)) {
                  return false;
                }
              }
              else if(enclLoop->isWhileStmt() ||
                  enclLoop->isDoWhileStmt() ||
                  enclLoop->isWhileDoStmt()) {
                if(toWhileStmt(enclLoop)->condExprGet()->contains(ce)) {
                  return false;
                }
              }
            }
          }
        }
        return true;
      }
    }
  }
  return false;
}

void denormalize(Expr* def, SymExpr* use, Type* castTo) {
  Expr* defPar = def->parentExpr;

  //remove variable declaration
  use->symbol()->defPoint->remove();

  //remove def
  Expr* replExpr = def->remove();

  //replace use with def
  if(castTo != NULL) {
    SET_LINENO(def);
    Expr* castExpr = new CallExpr(PRIM_CAST, castTo->symbol, replExpr);
    use->replace(castExpr);
  }
  else {
    use->replace(replExpr);
  }
  //remove defPar
  defPar->remove();
}

inline bool requiresCast(Type* t) {
  if(is_int_type(t) || is_uint_type(t) || is_real_type(t)) {
    return true;
  }
  return false;
}

inline bool isFloatComparisonPrimitive(CallExpr *ce) {
  if(ce->isPrimitive()) {
    switch(ce->primitive->tag) {
      case PRIM_EQUAL:
      case PRIM_NOTEQUAL:
      case PRIM_LESSOREQUAL:
      case PRIM_GREATEROREQUAL:
      case PRIM_LESS:
      case PRIM_GREATER:
        if(is_real_type(ce->get(1)->typeInfo()) ||
           is_real_type(ce->get(2)->typeInfo())) {
          return true;
        }
        break;
      default:
        return false;
        break;
    }
  }
  return false;
}

inline bool isArithmeticPrimitive(CallExpr *ce) {
  if(ce->isPrimitive()) {
    switch(ce->primitive->tag) {
      case PRIM_ADD:
      case PRIM_SUBTRACT:
      case PRIM_MULT:
      case PRIM_DIV:
      case PRIM_MOD:
      case PRIM_LSH:
      case PRIM_RSH:
      case PRIM_UNARY_NOT:
        return true;
        break;
      default:
        return false;
        break;
    }
  }
  return false;
}

// If RHS of a move contain access to anything wide, it can generate
// communication. chpl_gen_comm_get "returns" the data in the first
// argument, and the function itself is generated during codegen(see
// expr.cpp codegenAssign). Although the value assigned is actually
// temporary in the AST, it cannot be denormalized due to that.
//
// Such temporaries that are passed as address are not denormalized in
// other functions due to PRIM_ADDROF, since chpl_gen_comm_get is
// generated at codegen time, such information is not readily available
// at AST.
bool primMoveGeneratesCommCall(CallExpr* ce) {
  INT_ASSERT(ce);
  INT_ASSERT(ce->isPrimitive(PRIM_MOVE) || ce->isPrimitive(PRIM_ASSIGN));

  Expr* lhs = ce->get(1);
  Expr* rhs = ce->get(2);
  Type* lhsType = lhs->typeInfo();
  Type* rhsType = rhs->typeInfo();

  if(lhsType->symbol->hasEitherFlag(FLAG_WIDE_REF, FLAG_WIDE_CLASS) || lhs->isWideRef())
    return true; // direct put
  if(rhsType->symbol->hasEitherFlag(FLAG_WIDE_REF, FLAG_WIDE_CLASS) || rhs->isWideRef())
    return true; // direct get

  //now it is still possible that rhs primitive has a nonwide symbol yet
  //the primitive itself generates communication
  if(CallExpr* rhsCe = toCallExpr(rhs)) {
    if(rhsCe->isPrimitive()) {
      switch(rhsCe->primitive->tag) {
        case PRIM_SET_MEMBER:
        case PRIM_GET_MEMBER:
        case PRIM_GET_MEMBER_VALUE:
        case PRIM_SET_SVEC_MEMBER:
        case PRIM_GET_SVEC_MEMBER:
        case PRIM_GET_SVEC_MEMBER_VALUE:
          if(rhsCe->get(1)->typeInfo()->symbol->hasEitherFlag(FLAG_WIDE_REF,
                FLAG_WIDE_CLASS) || rhsCe->get(1)->isWideRef()) {
            return true;
          }
          break;
        default:
          return false;
      }
    }
  }
  else {
    // var to var move
    // I don't see how this might create communication
  }
  return false;
}


inline bool unsafeExprInBetween(Expr* e1, Expr* e2, Expr* exprToMove,
    SafeExprAnalysis& analysisData){
  int counter = 0;
  for(Expr* e = e1; e != e2 ; e = getNextExpr(e)) {
    if(! analysisData.exprHasNoSideEffects(e, exprToMove)) {
      return true;
    }

    // implementation of this function is suboptimal as it's
    // asymptotically O(N**2). This if is a stopgap measure to prevent
    // it running for too long.  So, currently we give up when there is
    // more than 100 Exprs between e1 and e2.
    //
    // If this still causes any performance issues, a better way of
    // implementing this is:
    // (1) get tally of safe(0)/unsafe(1) Expr in the function
    //
    //  (0)  Expr1
    //  (1)  Expr2
    //  (0)  Expr3
    //  (1)  Expr4
    //  (0)  Expr5
    //
    // (2) run scan on the data generated
    //
    //  (0)  Expr1
    //  (1)  Expr2
    //  (1)  Expr3
    //  (2)  Expr4
    //  (2)  Expr5
    //
    // (3) there is an unsafe Expr between Expr_i and Expr_j if their "distance"
    // are different.
    if(++counter >= 100) {
      return true;
    }
  }
  return false;
}

/////////////////////////////////////////////////////////////////////////////
// undoReturnByRef()
//
// Ultimately we may want to avoid introducing return by ref.
// For now, simply undo it as a denormalization step.

///////////
//
// transformRetTempDef() will modify 'fn' to replace
//   =(refArg, something)
//   return(void)
// with:
//   return(something)

struct ReturnByRefDef {
  FnSymbol*  fn;           // the function that returns by ref
  ArgSymbol* refArg;       // the function's formal used for indirect return
  bool       skipLnFnArgs; // if true, refArg is fn's 3rd-last formal
};

static bool isGoodRefArg(FnSymbol* fn, ReturnByRefDef& info,
                         bool skippedLnFnArgs, Expr* argDef) {
  ArgSymbol* refArg = toArgSymbol(toDefExpr(argDef)->sym);
  if (!refArg->hasFlag(FLAG_RETARG))
    return false; // give up; maybe look harder?
  INT_ASSERT(!strcmp(refArg->name, "_retArg"));
  INT_ASSERT(fn->retType = dtVoid);

  info = { fn, refArg, skippedLnFnArgs };
  return true;
}

static bool acceptableDef(FnSymbol* fn, ReturnByRefDef& info) {
  // isGoodRefArg() fills 'info'
  if (! isGoodRefArg(fn, info, false, fn->formals.tail)             &&
      ! (fn->numFormals() >= 3 &&
         isGoodRefArg(fn, info, true, fn->formals.tail->prev->prev)) )
    return false;

  if (info.refArg->type->symbol->hasFlag(FLAG_STAR_TUPLE))
    return false; // codegen requires star tuples to be passed by ref

  for_SymbolSymExprs(refSE, info.refArg) {
    if (CallExpr* call = toCallExpr(refSE->parentExpr)) {
      if (call->isPrimitive(PRIM_NO_ALIAS_SET))
        continue; // any of these are ok
      if (! call->isPrimitive(PRIM_ASSIGN))
        return false; // need more work to handle, ex., PRIM_SET_MEMBER
      // transformRetTempDef() will assert that we have only one PRIM_ASSIGN
    }
  }

  return true;
}

static void transformRetTempDef(ReturnByRefDef& info) {
  SymExpr* refUse = nullptr;
  for_SymbolSymExprs(refSE, info.refArg) {
    if (CallExpr* call = toCallExpr(refSE->parentExpr))
      if (call->isPrimitive(PRIM_NO_ALIAS_SET)) {
        if (call->numActuals() == 1) call->remove();
        else refSE->remove();
        continue;
      }
    INT_ASSERT(refUse == nullptr); // expect only a single SE
    refUse = refSE;
  }

  CallExpr* assignCall = toCallExpr(refUse->parentExpr);
  Expr* refValueExpr = assignCall->get(2)->remove();
  INT_ASSERT(isSymExpr(refValueExpr)); // ensure it can be used in 'return'
  // Todo: do we need to add FLAG_RVV to refValueExpr->symbol()?
  // At this point temps with FLAG_RVV may occur anywhere due to inlining.
  assignCall->remove();

  CallExpr* returnCall = toCallExpr(info.fn->body->body.tail);
  INT_ASSERT(returnCall->isPrimitive(PRIM_RETURN));
  INT_ASSERT(toSymExpr(returnCall->get(1))->symbol() == gVoid);
  returnCall->get(1)->replace(refValueExpr);

  info.refArg->defPoint->remove();
  info.fn->retType = info.refArg->type;
  info.fn->removeFlag(FLAG_FN_RETARG);
}

///////////
//
// transformRetTempUse() will replace:
//   call fn(args, ret_tmp)
// with:
//   move(ret_tmp, call fn(args))
//
// collapseTrivialMoves() will reduce to the original 'call_temp = fn(...)'

struct ReturnByRefUse {
  SymExpr* fnSE;    //  'fn'
  SymExpr* tempSE;  //  'ret_tmp'
};

// Return true if the desired pattern is present.
static bool acceptableUse(ReturnByRefDef& defInfo, SymExpr* fnUse,
                          ReturnByRefUse& useInfo) {
  CallExpr* call = toCallExpr(fnUse->parentExpr);
  if (call == nullptr || call->resolvedFunction() != defInfo.fn)
    return false;

  INT_ASSERT(call == call->getStmtExpr());
  SymExpr* tempSE = toSymExpr(defInfo.skipLnFnArgs ?
                      call->argList.tail->prev->prev : call->argList.tail);
  // the temp is usually called "ret_tmp", however not necessarily
  INT_ASSERT(tempSE->symbol()->type == defInfo.refArg->type); // fyi

  useInfo = { fnUse, tempSE };
  return true;
}

static void transformRetTempUse(ReturnByRefUse& info) {
  Expr*   fnCall  = info.fnSE->parentExpr;
  Symbol* retTemp = info.tempSE->symbol();
  INT_ASSERT(! retTemp->type->symbol->hasEitherFlag(FLAG_REF, FLAG_WIDE_REF));

  // replace:
  //   call fn(args, ret_tmp)
  // with:
  //   move(ret_tmp, call fn(args))  // if ret_tmp is a ref, make it assign
  //
  SET_LINENO(fnCall);

  Expr* anchor = fnCall->prev;
  BlockStmt* encl = anchor ? nullptr : toBlockStmt(fnCall->parentExpr);
  Expr* move   = new CallExpr(retTemp->isRef() ? PRIM_ASSIGN : PRIM_MOVE,
                              info.tempSE->remove(), fnCall->remove());
  if (anchor) anchor->insertAfter(move);
  else        encl->insertAtHead(move);
}

///////////
// putting it all together

static void undoReturnByRef(FnSymbol* fn) {
  if (fn->hasFlag(FLAG_VIRTUAL)) return;  // skip these for now

  ReturnByRefDef defInfo;
  if (! acceptableDef(fn, defInfo))
    return;

  // Make changes only if we can handle all uses of 'fn'.
  // While checking, store some findings for later use.
  std::vector<ReturnByRefUse> infos;
  for_SymbolSymExprs(use, fn) {
    ReturnByRefUse useInfo;
    if (acceptableUse(defInfo, use, useInfo))
      infos.push_back(useInfo);
    else
      return;
  }

  for (ReturnByRefUse& info: infos)
    transformRetTempUse(info);

  transformRetTempDef(defInfo);
}

/////////////////////////////////////////////////////////////////////////////
// collapseTrivialMoves() converts:
//   move(source, expr)  // move1
//   move(dest, source)  // move2
// provided:
//   'move1' and 'move2' are adjacent or have only DefExprs in between
//   'source' has no other references
// to:
//   move(dest, expr)

// In some cases codegen adds an appropriate dereference, widening, etc.
// for a symbol-to-symbol move and not for a call-to-symbol move.
// So collapse only moves for which such additions are not needed.
static bool okSymbol(Symbol* sym) {
  return sym->qual == QUAL_VAL || sym->qual == QUAL_CONST_VAL;
}

static bool canCollapseMoveBetween(SymExpr* dest, SymExpr* source) {
  Symbol *destSym = dest->symbol(), *sourceSym = source->symbol();
  return
    destSym->type == sourceSym->type        &&
    ! sourceSym->hasFlag(FLAG_CONFIG)       &&
    ! sourceSym->hasFlag(FLAG_EXPORT)       &&
    ! sourceSym->hasFlag(FLAG_EXTERN)       &&
    okSymbol(destSym) && okSymbol(sourceSym);
}

static bool closeEnough(Expr* move1, Expr* move2) {
  Expr* curr = move1;
  for (int dist = 0; dist < 5; dist++) {  // heuristically allow <=5 defs
    curr = curr->next;
    if (curr == move2) return true;
    if (!curr || ! isDefExpr(curr)) return false;
  }
  return false;
}

// Returns 'move1', or NULL if the desired pattern is not present.
// 'sourceSE' is the SymExpr for 'source' in 'move2'.
static CallExpr* singleMoveTo(CallExpr* move2, SymExpr* sourceSE) {
  Symbol* source = sourceSE->symbol();
  SymExpr* otherSE = source->firstSymExpr();
  // Specialize for exactly two references to 'source'.
  if (otherSE == sourceSE) {
    // need source -> sourceSE -> otherSE -> NULL
    otherSE = otherSE->symbolSymExprsNext;
    if (otherSE == nullptr || otherSE->symbolSymExprsNext != nullptr)
      return nullptr; // 1 or >2 references
  }
  else {
    // need source -> otherSE -> sourceSE -> NULL
    if (otherSE->symbolSymExprsNext != sourceSE ||
        sourceSE->symbolSymExprsNext != nullptr)
      return nullptr; // >2 references
  }

  CallExpr* move1 = toCallExpr(otherSE->parentExpr);
  if (move1 != nullptr                                    &&
      otherSE == move1->get(1)                            &&
      move1->isPrimitive(PRIM_MOVE)                       &&
      (move2 == move1->next || closeEnough(move1, move2)) )
    return move1;
  else
    return nullptr;
}

static void collapseTrivialMoves() {
  // Empirically, running collapseTrivialMoves() the second time
  // would not result in any additional removals. So, do it just once.
  //
  // This work could be done on a per-function basis using collectCallExprs().
  // Simply traversing 'gCallExprs' avoids the overhead of collectCallExprs().
  for_alive_in_Vec(CallExpr, move2, gCallExprs) {
   if (move2->isPrimitive(PRIM_MOVE))
    if (SymExpr* dest = toSymExpr(move2->get(1)))
     while (true) {
      if (SymExpr* source = toSymExpr(move2->get(2)))
       if (canCollapseMoveBetween(dest, source))
        if (CallExpr* move1 = singleMoveTo(move2, source)) {
          move1->remove();
          source->replace(move1->get(2)->remove());
          source->symbol()->defPoint->remove();
          continue; // will check for another move1 to reduce with move2
        }
      break; // no further opportunities with move2
     } // while
  } // for
}

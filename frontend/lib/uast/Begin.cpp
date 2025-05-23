/*
 * Copyright 2021-2025 Hewlett Packard Enterprise Development LP
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

#include "chpl/uast/Begin.h"

#include "chpl/uast/Builder.h"

namespace chpl {
namespace uast {


std::string Begin::dumpChildLabelInner(int i) const {
  if (withClauseChildNum_ >= 0 && i == withClauseChildNum_) {
    return "with";
  }

  return "";
}

owned<Begin> Begin::build(Builder* builder,
                          Location loc,
                          owned<WithClause> withClause,
                          BlockStyle blockStyle,
                          AstList stmts) {
  AstList lst;
  int8_t withClauseChildNum = NO_CHILD;

  if (withClause.get() != nullptr) {
    withClauseChildNum = lst.size();
    lst.push_back(std::move(withClause));
  }

  const int bodyChildNum = lst.size();
  const int numBodyStmts = stmts.size();

  for (auto& stmt : stmts) {
    lst.push_back(std::move(stmt));
  }

  Begin* ret = new Begin(std::move(lst), withClauseChildNum, blockStyle,
                         bodyChildNum,
                         numBodyStmts);
  builder->noteLocation(ret, loc);
  return toOwned(ret);
}


} // namespace uast
} // namespace chpl

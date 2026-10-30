/*
 * Copyright (c) 2025 OceanBase.
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

#pragma once

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/container/ob_se_array.h"

struct ParseNode;

namespace oceanbase
{
namespace sql
{

struct ObResolverParams;

// One column attribute clause of the form
//   <col> <type> AI_COMPLETE('model', dep1, dep2) (max_retry=3)
// captured while the column definition is being resolved.
//
// Only the parse node and the column name are recorded here: both live in memory that
// outlives the whole resolve() call, so staging costs no allocation and cannot fail
// half way. The heavy lifting (resolving the dependent columns, validating the result
// type and generating the hidden columns) happens in one pass at the end of the
// statement, when the table schema holds every column and the full picture is known.
struct ObAiColumnDefineItem
{
  ObAiColumnDefineItem() : column_name_(), attr_node_(NULL) {}
  common::ObString column_name_;
  ParseNode *attr_node_;
  TO_STRING_KV(K_(column_name), KP_(attr_node));
};

// Staging area of the AI column clauses of one DDL statement.
//
// It is attached to ObResolverParams and is strictly single use: a statement stages a
// definition while its column is being resolved and consumes it at the end. The guard
// (ObAiColumnDefineGuard) clears it on every exit path and asserts it stayed empty,
// because a leftover would otherwise be picked up by the next statement of the same
// session and attached to a table it does not belong to.
class ObAiColumnDefineCtx
{
public:
  static const int64_t MAX_AI_COLUMN_PER_STMT = 64;
public:
  ObAiColumnDefineCtx() : table_id_(common::OB_INVALID_ID), items_() {}
  ~ObAiColumnDefineCtx() = default;

  void reset()
  {
    table_id_ = common::OB_INVALID_ID;
    items_.reset();
  }
  bool empty() const { return 0 == items_.count(); }
  int64_t count() const { return items_.count(); }
  const ObAiColumnDefineItem &at(const int64_t idx) const { return items_.at(idx); }

  // Re-staging the same column in one statement is a user error, report it instead of
  // silently keeping the last definition.
  int add(const common::ObString &column_name, ParseNode *attr_node);
  // Consume the staged definitions: called by the DDL resolver before it reads them.
  int consume(common::ObIArray<ObAiColumnDefineItem> &items);

  void set_table_id(const uint64_t table_id) { table_id_ = table_id; }
  uint64_t get_table_id() const { return table_id_; }
  // A staged definition may only be applied to the table it was staged for. The check
  // is explicit so that a mismatch is an error and not a silent no-op.
  int check_table_id(const uint64_t table_id) const;

  TO_STRING_KV(K_(table_id), K_(items));
private:
  uint64_t table_id_;
  common::ObSEArray<ObAiColumnDefineItem, 8> items_;
};

// RAII owner of the staging area inside ObResolverParams.
class ObAiColumnDefineGuard
{
public:
  // `allocator` may be NULL: it only has to be usable when a definition is actually staged.
  ObAiColumnDefineGuard(common::ObIAllocator *allocator, ObResolverParams &params);
  ~ObAiColumnDefineGuard();

  int get_ret() const { return ret_; }
  ObAiColumnDefineCtx *get_ctx() const { return ctx_; }
  // Regression protection: every staged definition must have been consumed. Report it
  // instead of letting the statement succeed with a half applied AI column.
  int check_all_consumed(const int resolve_ret) const;

private:
  ObResolverParams &params_;
  ObAiColumnDefineCtx *ctx_;
  bool owns_ctx_;
  int ret_;
};

} // namespace sql
} // namespace oceanbase

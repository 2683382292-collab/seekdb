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
#include "share/schema/ob_ai_column_schema.h"
#include "sql/resolver/ddl/ob_ai_column_define_ctx.h"

struct ParseNode;

namespace oceanbase
{
namespace common
{
class ObIAllocator;
template <typename T, int64_t N> class ObSEArray;
}
namespace share
{
namespace schema
{
class ObTableSchema;
class AlterTableSchema;
class ObColumnSchemaV2;
class ObAiColumnSchema;
}
}
namespace sql
{

struct ObResolverParams;

// Resolves the AI column clauses of a DDL statement.
//
// The work is split in two steps because the dependent columns of an AI column can be
// declared after it and, for CREATE TABLE, are not in the table schema yet while the
// column definition itself is being resolved:
//   1. stage_definition() records the raw clause and does only syntax level checks,
//   2. resolve_create_definitions() / resolve_alter_definitions() run once, when every
//      column is materialized, and do the semantic work: dependency resolution, result
//      type and vector dimension validation, type driven prompt hint, hidden column
//      generation and persistence of the definition in __all_column.ai_column_info.
class ObAiColumnResolver
{
public:
  // Called from ObDDLResolver::resolve_normal_column_attribute() for T_AI_COLUMN_ATTR.
  static int stage_definition(ObResolverParams &params,
                              const common::ObString &column_name,
                              ParseNode *attr_node);

  // Every column of the table is already in table_schema. Defined hard: an AI column
  // whose clause cannot be fully resolved fails the DDL, it is never silently ignored.
  static int resolve_create_definitions(common::ObIAllocator &allocator,
                                        const ObResolverParams &params,
                                        share::schema::ObTableSchema &table_schema,
                                        uint64_t &next_column_id);

  // base_table_schema is the table before the statement, alter_table_schema carries the
  // added/modified columns.
  static int resolve_alter_definitions(common::ObIAllocator &allocator,
                                       const ObResolverParams &params,
                                       const share::schema::ObTableSchema &base_table_schema,
                                       share::schema::AlterTableSchema &alter_table_schema);

  // Parse one AI clause against its already typed result column. Exposed so that the
  // DML resolvers can recognise an AI column without paying for a full table walk.
  static int resolve_definition(common::ObIAllocator &allocator,
                                const share::schema::ObColumnSchemaV2 &result_column,
                                ParseNode *attr_node,
                                share::schema::ObAiColumnSchema &ai_column);

  // Names of the three hidden columns of an AI column, used by DDL to reject a user
  // column that would collide with them.
  static int check_hidden_column_conflict(common::ObIAllocator &allocator,
                                          const share::schema::ObAiColumnSchema &ai_column,
                                          const share::schema::ObTableSchema &table_schema);
};

} // namespace sql
} // namespace oceanbase

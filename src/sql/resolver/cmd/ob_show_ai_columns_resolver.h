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

#include "sql/resolver/dml/ob_select_resolver.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObTableSchema;
class ObColumnSchemaV2;
class ObAiColumnSchema;
} // namespace schema
} // namespace share
namespace sql
{
class ObSqlString;

// SHOW AI COLUMNS FROM <table>.
//
// The definition of an ai column lives in the table schema (__all_column.ai_column_info),
// so the rows are built from the schema the statement sees when it is resolved and are
// handed over to the normal select pipeline. Reading the schema instead of a new inner
// table keeps SHOW consistent with what the DDL resolver validated and with what the
// background service persists: there is only one copy of the truth.
class ObShowAiColumnsResolver : public ObSelectResolver
{
public:
  explicit ObShowAiColumnsResolver(ObResolverParams &params);
  virtual ~ObShowAiColumnsResolver();
  virtual int resolve(const ParseNode &parse_tree);
private:
  int resolve_table(const ParseNode *relation_node,
                    const share::schema::ObTableSchema *&table_schema,
                    common::ObString &database_name);
  int build_select_sql(const share::schema::ObTableSchema &table_schema,
                       ObSqlString &select_sql);
  int append_column_row(const share::schema::ObTableSchema &table_schema,
                        const share::schema::ObColumnSchemaV2 &column,
                        const share::schema::ObAiColumnSchema &ai_column,
                        const int64_t row_idx,
                        ObSqlString &select_sql);
  int resolve_select_sql(const common::ObString &select_sql);
private:
  DISALLOW_COPY_AND_ASSIGN(ObShowAiColumnsResolver);
};

} // namespace sql
} // namespace oceanbase

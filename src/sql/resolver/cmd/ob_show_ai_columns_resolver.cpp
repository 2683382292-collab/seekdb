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

#define USING_LOG_PREFIX SQL_RESV
#include "sql/resolver/cmd/ob_show_ai_columns_resolver.h"
#include "lib/string/ob_sql_string.h"
#include "common/object/ob_obj_type.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_ai_column_schema.h"
#include "sql/parser/ob_parser.h"
#include "sql/resolver/ob_schema_checker.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace sql
{

namespace
{

// The rows are produced by a generated select, so every value read from the schema has to
// be quoted as a MySQL string literal. Only the quotes and the escape character itself are
// special inside such a literal.
int append_string_literal(common::ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
    LOG_WARN("fail to append string literal", KR(ret));
  }
  for (int32_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value[i];
    if ('\'' == c) {
      ret = sql.append("''");
    } else if ('\\' == c) {
      ret = sql.append("\\\\");
    } else {
      ret = sql.append(&c, 1);
    }
    if (OB_FAIL(ret)) {
      LOG_WARN("fail to append string literal", KR(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append("'"))) {
    LOG_WARN("fail to append string literal", KR(ret));
  }
  return ret;
}

} // anonymous namespace

ObShowAiColumnsResolver::ObShowAiColumnsResolver(ObResolverParams &params)
    : ObSelectResolver(params)
{
  params_.is_from_show_resolver_ = true;
}

ObShowAiColumnsResolver::~ObShowAiColumnsResolver()
{
}

int ObShowAiColumnsResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  const ObTableSchema *table_schema = NULL;
  ObString database_name;
  ObSqlString select_sql;
  if (OB_ISNULL(session_info_) || OB_ISNULL(params_.allocator_) || OB_ISNULL(schema_checker_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("resolver is not initialized", KR(ret));
  } else if (OB_UNLIKELY(T_SHOW_AI_COLUMNS != parse_tree.type_ || 1 != parse_tree.num_child_
                         || OB_ISNULL(parse_tree.children_)
                         || OB_ISNULL(parse_tree.children_[0]))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid show ai columns node", KR(ret), K(parse_tree.type_));
  } else if (OB_FAIL(resolve_table(parse_tree.children_[0], table_schema, database_name))) {
    LOG_WARN("fail to resolve the table of show ai columns", KR(ret));
  } else if (OB_FAIL(build_select_sql(*table_schema, select_sql))) {
    LOG_WARN("fail to build the select of show ai columns", KR(ret), KPC(table_schema));
  } else if (OB_FAIL(resolve_select_sql(select_sql.string()))) {
    LOG_WARN("fail to resolve the select of show ai columns", KR(ret));
  } else {
    ObSelectStmt *select_stmt = static_cast<ObSelectStmt *>(get_basic_stmt());
    if (OB_ISNULL(select_stmt)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("select stmt of show ai columns is null", KR(ret));
    } else {
      // A SHOW does not change the FOUND_ROWS of the session.
      select_stmt->set_select_type(NOT_AFFECT_FOUND_ROWS);
    }
  }
  return ret;
}

int ObShowAiColumnsResolver::resolve_table(const ParseNode *relation_node,
                                           const ObTableSchema *&table_schema,
                                           ObString &database_name)
{
  int ret = OB_SUCCESS;
  uint64_t database_id = OB_INVALID_ID;
  ObString table_name;
  table_schema = NULL;
  if (OB_FAIL(resolve_table_relation_factor_normal(relation_node,
                                                   database_id,
                                                   table_name,
                                                   database_name))) {
    LOG_WARN("fail to resolve table name", KR(ret));
  } else if (OB_FAIL(schema_checker_->get_table_schema(database_id,
                                                       table_name,
                                                       false /*is_index_table*/,
                                                       false /*cte_table_first*/,
                                                       false /*with_hidden_flag*/,
                                                       table_schema))) {
    LOG_WARN("fail to get table schema", KR(ret), K(database_id), K(table_name));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table of show ai columns does not exist", KR(ret), K(table_name));
  } else {
    ObSessionPrivInfo session_priv;
    bool allow_show = true;
    if (OB_FAIL(session_info_->get_session_priv_info(session_priv))) {
      LOG_WARN("fail to get session priv", KR(ret));
    } else if (OB_FAIL(schema_checker_->check_table_show(session_priv,
                                                         session_info_->get_enable_role_array(),
                                                         database_name,
                                                         table_schema->get_table_name_str(),
                                                         allow_show))) {
      LOG_WARN("fail to check table access for show", KR(ret));
    } else if (!allow_show) {
      ret = OB_ERR_NO_TABLE_PRIVILEGE;
      LOG_WARN("no privilege to show the ai columns of the table", KR(ret),
               K(database_name), K(table_name));
      LOG_USER_ERROR(OB_ERR_NO_TABLE_PRIVILEGE, (int)strlen("SELECT"), "SELECT",
                     session_priv.user_name_.length(), session_priv.user_name_.ptr(),
                     session_priv.host_name_.length(), session_priv.host_name_.ptr(),
                     table_schema->get_table_name_str().length(),
                     table_schema->get_table_name_str().ptr());
    }
  }
  return ret;
}

int ObShowAiColumnsResolver::build_select_sql(const ObTableSchema &table_schema,
                                              common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  int64_t row_cnt = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < table_schema.get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema_by_idx(i);
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column schema is null", KR(ret), K(i));
    } else if (!column->is_ai_column()) {
      // only the result column of an ai column carries a definition
    } else if (column->get_ai_column_info().empty()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column has no definition", KR(ret), KPC(column));
    } else {
      ObAiColumnSchema ai_column;
      ai_column.set_allocator(params_.allocator_);
      if (OB_FAIL(ai_column.deserialize(column->get_ai_column_info()))) {
        LOG_WARN("fail to deserialize ai column definition", KR(ret), KPC(column));
      } else if (OB_FAIL(append_column_row(table_schema, *column, ai_column, row_cnt, sql))) {
        LOG_WARN("fail to append ai column row", KR(ret), KPC(column));
      } else {
        ++row_cnt;
      }
    }
  }
  if (OB_SUCC(ret) && 0 == row_cnt) {
    // Without any row the types of the result columns cannot be deduced, so one typed but
    // empty branch is emitted. An empty string and zero give the same column types as a
    // real row would.
    if (OB_FAIL(sql.append("select '' as `column_name`, '' as `func_type`, "
                           "'' as `model_name`, '' as `expr`, '' as `depend_columns`, "
                           "'' as `result_type`, 0 as `vector_dimension`, "
                           "'' as `prompt_hint`, 0 as `max_retry`, 0 as `timeout_sec`, "
                           "'' as `sync_mode` from dual where 1 = 0"))) {
      LOG_WARN("fail to append the empty branch", KR(ret));
    }
  }
  return ret;
}

int ObShowAiColumnsResolver::append_column_row(const ObTableSchema &table_schema,
                                               const ObColumnSchemaV2 &column,
                                               const ObAiColumnSchema &ai_column,
                                               const int64_t row_idx,
                                               common::ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  common::ObSqlString depend_columns;
  const ObIArray<uint64_t> &depend_column_ids = ai_column.get_depend_column_ids();
  for (int64_t i = 0; OB_SUCC(ret) && i < depend_column_ids.count(); ++i) {
    const ObColumnSchemaV2 *depend_column =
        table_schema.get_column_schema(depend_column_ids.at(i));
    if (OB_ISNULL(depend_column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("depend column of ai column does not exist", KR(ret),
               K(depend_column_ids.at(i)));
    } else if (OB_FAIL(depend_columns.append(0 == i ? "" : ","))) {
      LOG_WARN("fail to append depend column separator", KR(ret));
    } else if (OB_FAIL(depend_columns.append(depend_column->get_column_name_str()))) {
      LOG_WARN("fail to append depend column name", KR(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (0 < row_idx && OB_FAIL(sql.append(" union all "))) {
    LOG_WARN("fail to append union all", KR(ret));
  } else if (OB_FAIL(sql.append("select "))) {
    LOG_WARN("fail to append select", KR(ret));
  } else if (OB_FAIL(append_string_literal(sql, column.get_column_name_str()))) {
  } else if (OB_FAIL(sql.append(" as `column_name`, "))) {
  } else if (OB_FAIL(append_string_literal(sql,
               ObString::make_string(ai_column.is_embedding() ? "DENSE_EMBEDDING" : "COMPLETION")))) {
  } else if (OB_FAIL(sql.append(" as `func_type`, "))) {
  } else if (OB_FAIL(append_string_literal(sql, ai_column.get_model_name()))) {
  } else if (OB_FAIL(sql.append(" as `model_name`, "))) {
  } else if (OB_FAIL(append_string_literal(sql, ai_column.get_expr_str()))) {
  } else if (OB_FAIL(sql.append(" as `expr`, "))) {
  } else if (OB_FAIL(append_string_literal(sql, depend_columns.string()))) {
  } else if (OB_FAIL(sql.append(" as `depend_columns`, "))) {
  } else if (OB_FAIL(append_string_literal(sql,
               ObString::make_string(ob_sql_type_str(column.get_data_type()))))) {
  } else if (OB_FAIL(sql.append(" as `result_type`, "))) {
  } else if (OB_FAIL(sql.append_fmt("%ld", ai_column.get_vector_dimension()))) {
    LOG_WARN("fail to append vector dimension", KR(ret));
  } else if (OB_FAIL(sql.append(" as `vector_dimension`, "))) {
  } else if (OB_FAIL(append_string_literal(sql, ai_column.get_prompt_hint()))) {
  } else if (OB_FAIL(sql.append(" as `prompt_hint`, "))) {
  } else if (OB_FAIL(sql.append_fmt("%ld", ai_column.get_max_retry()))) {
    LOG_WARN("fail to append max retry", KR(ret));
  } else if (OB_FAIL(sql.append(" as `max_retry`, "))) {
  } else if (OB_FAIL(sql.append_fmt("%ld", ai_column.get_timeout_sec()))) {
    LOG_WARN("fail to append timeout", KR(ret));
  } else if (OB_FAIL(sql.append(" as `timeout_sec`, "))) {
  } else if (OB_FAIL(append_string_literal(sql,
               ObString::make_string(ai_column.is_sync_mode_async() ? "async" : "sync")))) {
  } else if (OB_FAIL(sql.append(" as `sync_mode` from dual"))) {
    LOG_WARN("fail to append the branch tail", KR(ret));
  }
  return ret;
}

int ObShowAiColumnsResolver::resolve_select_sql(const ObString &select_sql)
{
  int ret = OB_SUCCESS;
  ParseResult select_result;
  ObParser parser(*params_.allocator_, session_info_->get_sql_mode());
  if (OB_FAIL(parser.parse(select_sql, select_result))) {
    LOG_WARN("fail to parse the generated select sql", KR(ret), K(select_sql));
  } else if (OB_ISNULL(select_result.result_tree_)
             || OB_UNLIKELY(1 != select_result.result_tree_->num_child_)
             || OB_ISNULL(select_result.result_tree_->children_)
             || OB_ISNULL(select_result.result_tree_->children_[0])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected parse result of the generated select sql", KR(ret), K(select_sql));
  } else if (OB_FAIL(ObSelectResolver::resolve(*select_result.result_tree_->children_[0]))) {
    LOG_WARN("fail to resolve the generated select sql", KR(ret), K(select_sql));
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase

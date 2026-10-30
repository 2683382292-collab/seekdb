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

#include <new>
#include "sql/resolver/ddl/ob_ai_column_resolver.h"
#include "sql/resolver/ddl/ob_ai_column_define_ctx.h"
#include "sql/resolver/ob_resolver_define.h"
#include "sql/parser/parse_node.h"
#include "query/parser/ob_item_type.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_schema_service.h"
#include "common/object/ob_obj_type.h"
#include "lib/string/ob_string_buffer.h"

using namespace oceanbase::common;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace sql
{

namespace
{

bool is_string_literal(const ParseNode &node)
{
  return T_VARCHAR == node.type_ || T_CHAR == node.type_ || T_NCHAR == node.type_;
}

int get_literal_string(const ParseNode &node, ObString &str)
{
  int ret = OB_SUCCESS;
  str.reset();
  if (is_string_literal(node)) {
    str.assign_ptr(node.str_value_, static_cast<int32_t>(node.str_len_));
  } else {
    ret = OB_ERR_COLUMN_SPEC;
    LOG_WARN("ai column expects a string literal", KR(ret), K(node.type_));
  }
  return ret;
}

int get_literal_int(const ParseNode &node, int64_t &value)
{
  int ret = OB_SUCCESS;
  value = 0;
  if (T_INT == node.type_) {
    value = node.value_;
  } else if (is_string_literal(node)) {
    // '3' is accepted as well so that an option can be written uniformly.
    const int32_t len = static_cast<int32_t>(node.str_len_);
    if (len <= 0) {
      ret = OB_ERR_COLUMN_SPEC;
      LOG_WARN("ai column option is empty", KR(ret));
    }
    for (int32_t i = 0; OB_SUCC(ret) && i < len; ++i) {
      const char c = node.str_value_[i];
      if (c < '0' || c > '9') {
        ret = OB_ERR_COLUMN_SPEC;
        LOG_WARN("ai column option is not a non negative integer", KR(ret));
      } else {
        value = value * 10 + (c - '0');
      }
    }
  } else {
    ret = OB_ERR_COLUMN_SPEC;
    LOG_WARN("ai column expects an integer", KR(ret), K(node.type_));
  }
  return ret;
}

// Dimension of a VECTOR(n) result column. ObDDLResolver::resolve_collection_column()
// writes "VECTOR(n)" into extended_type_info, which is also where the vector index and
// the write back path read it from, so the metadata, the column type and the index can
// never disagree.
int get_vector_dimension(const ObColumnSchemaV2 &column, int64_t &dimension)
{
  int ret = OB_SUCCESS;
  dimension = 0;
  const ObIArray<ObString> &type_info = column.get_extended_type_info();
  if (type_info.count() <= 0 || type_info.at(0).empty()) {
    ret = OB_ERR_COLUMN_SPEC;
    LOG_WARN("vector column has no dimension", KR(ret), K(column));
  } else {
    const ObString &info = type_info.at(0);
    int32_t open = -1;
    int32_t close = -1;
    for (int32_t i = 0; OB_SUCC(ret) && i < info.length(); ++i) {
      if ('(' == info[i] && open < 0) {
        open = i;
      } else if (')' == info[i] && open >= 0) {
        close = i;
        break;
      } else { /* keep scanning */ }
    }
    if (OB_SUCC(ret)) {
      if (open < 0 || close <= open + 1) {
        ret = OB_ERR_COLUMN_SPEC;
        LOG_WARN("unexpected vector type info", KR(ret), K(info));
      } else {
        int64_t value = 0;
        for (int32_t i = open + 1; OB_SUCC(ret) && i < close; ++i) {
          const char c = info[i];
          if (c < '0' || c > '9') {
            ret = OB_ERR_COLUMN_SPEC;
            LOG_WARN("unexpected character in vector dimension", KR(ret), K(info));
          } else {
            value = value * 10 + (c - '0');
          }
        }
        if (OB_SUCC(ret)) {
          dimension = value;
        }
      }
    }
  }
  return ret;
}

// The prompt hint is derived from the declared result type, so the model is told which
// shape of answer the write back path accepts. Without it a completion tends to answer
// with prose and the result type check would reject every attempt.
const char *type_prompt_hint(const ObObjType type)
{
  const char *hint = "Reply with plain text only, no explanation.";
  if (ob_is_json(type)) {
    hint = "Reply with a single JSON value only, no explanation, no code fence.";
  } else if (ob_is_integer_type(type) || ob_is_number_or_decimal_int_tc(type)) {
    hint = "Reply with a single number only, no unit, no thousands separator, no explanation.";
  } else if (ob_is_date_tc(type) || ob_is_datetime_tc(type)) {
    hint = "Reply with a single date literal in 'YYYY-MM-DD HH:MM:SS' format only.";
  } else { /* string types keep the text hint */ }
  return hint;
}

// One of the three hidden bookkeeping columns:
//   type     INT / BIGINT
//   default  0, so a plain INSERT starts the row in PENDING state
//   flags    the AI_*_COLUMN_FLAG of its role
int build_hidden_column(const ObString &column_name,
                        const ObObjType type,
                        const int64_t role_flag,
                        ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  ObObj default_value;
  default_value.set_int(0);
  column.reset();
  column.set_data_type(type);
  column.set_nullable(false);
  column.set_is_hidden(true);
  column.set_charset_type(CHARSET_BINARY);
  column.set_collation_type(CS_TYPE_BINARY);
  column.add_column_flag(role_flag);
  if (OB_FAIL(column.set_column_name(column_name))) {
    LOG_WARN("fail to set hidden column name", KR(ret), K(column_name));
  } else if (OB_FAIL(column.set_orig_default_value(default_value))) {
    LOG_WARN("fail to set hidden column default value", KR(ret));
  } else if (OB_FAIL(column.set_cur_default_value(default_value, false))) {
    LOG_WARN("fail to set hidden column default value", KR(ret));
  }
  return ret;
}

struct HiddenColumnSpec
{
  const char *prefix_;
  ObObjType type_;
  int64_t role_flag_;
};

const HiddenColumnSpec HIDDEN_COLUMN_SPECS[] = {
  { "__ai_status_", ObInt32Type, AI_STATUS_COLUMN_FLAG },
  { "__ai_lease_", ObInt64Type, AI_LEASE_COLUMN_FLAG },
  { "__ai_retry_", ObInt32Type, AI_RETRY_COLUMN_FLAG },
};

bool is_staged_column(const ObIArray<ObString> &staged_names, const ObString &name)
{
  bool found = false;
  for (int64_t i = 0; !found && i < staged_names.count(); ++i) {
    found = staged_names.at(i).case_compare_equal(name);
  }
  return found;
}

// Unlike a table schema, the alter schema keeps no name index for the columns it adds
// (ObTableSchema::add_col_to_column_array() only appends to the array), so a lookup by
// name has to scan the array. This works for both kinds of schema.
const ObColumnSchemaV2 *find_column_by_name(const ObTableSchema &table_schema,
                                            const ObString &column_name)
{
  const ObColumnSchemaV2 *found = NULL;
  for (int64_t i = 0; NULL == found && i < table_schema.get_column_count(); ++i) {
    const ObColumnSchemaV2 *column = table_schema.get_column_schema_by_idx(i);
    if (NULL != column && column->get_column_name_str() == column_name) {
      found = column;
    }
  }
  return found;
}

// Resolve the dependent column references of one AI column to column ids. They are
// resolved here, in the second phase, because an AI column may depend on a column that
// is declared after it.
int resolve_depend_columns(const ParseNode *attr_node,
                           const ObString &ai_column_name,
                           const uint64_t ai_column_id,
                           const ObTableSchema &base_table_schema,
                           const ObTableSchema *extra_table_schema,
                           const ObIArray<ObString> &staged_names,
                           ObAiColumnSchema &ai_column)
{
  int ret = OB_SUCCESS;
  const ParseNode *arg_list = NULL;
  ai_column.get_depend_column_ids().reuse();
  if (OB_ISNULL(attr_node) || 3 != attr_node->num_child_ || OB_ISNULL(attr_node->children_)
      || OB_ISNULL(arg_list = attr_node->children_[1])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid ai column attribute node", KR(ret));
  }
  for (int64_t i = 1; OB_SUCC(ret) && i < arg_list->num_child_; ++i) {
    const ParseNode *arg = arg_list->children_[i];
    if (OB_ISNULL(arg) || T_IDENT != arg->type_) {
      ret = OB_ERR_COLUMN_SPEC;
      LOG_WARN("ai column depend expression must be a column reference", KR(ret),
               K(ai_column_name));
    } else {
      const ObString dep_name(static_cast<int32_t>(arg->str_len_), arg->str_value_);
      const ObColumnSchemaV2 *dep = find_column_by_name(base_table_schema, dep_name);
      if (NULL == dep && NULL != extra_table_schema) {
        dep = find_column_by_name(*extra_table_schema, dep_name);
      }
      if (OB_ISNULL(dep)) {
        ret = OB_ERR_COLUMN_SPEC;
        LOG_WARN("ai column depend column does not exist", KR(ret),
                 K(ai_column_name), K(dep_name));
      } else if (OB_INVALID_ID == dep->get_column_id()) {
        // An ADD COLUMN gets its column id only when the DDL is applied, so a dependency
        // on a column added by the same statement cannot be recorded in the metadata.
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("ai column can not depend on a column added by the same statement",
                 KR(ret), K(ai_column_name), K(dep_name));
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "ai column depends on a column of the same ddl");
      } else if (dep->is_hidden()) {
        ret = OB_ERR_COLUMN_SPEC;
        LOG_WARN("ai column can not depend on a hidden column", KR(ret),
                 K(ai_column_name), K(dep_name));
      } else if (dep->is_ai_column() || is_staged_column(staged_names, dep_name)) {
        // Chained AI columns would make the dependency change detection recursive and
        // the scheduling order non deterministic, they are not supported.
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("ai column can not depend on another ai column", KR(ret),
                 K(ai_column_name), K(dep_name));
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "chained ai column");
      } else if (dep->get_column_id() == ai_column_id) {
        ret = OB_ERR_COLUMN_SPEC;
        LOG_WARN("ai column can not depend on itself", KR(ret), K(ai_column_name));
      } else if (OB_FAIL(ai_column.add_depend_column_id(dep->get_column_id()))) {
        LOG_WARN("fail to add depend column id", KR(ret), K(dep_name));
      }
    }
  }
  return ret;
}

} // anonymous namespace

int ObAiColumnResolver::stage_definition(ObResolverParams &params,
                                         const ObString &column_name,
                                         ParseNode *attr_node)
{
  int ret = OB_SUCCESS;
  ObAiColumnDefineCtx *ctx = params.ai_column_define_ctx_;
  ObString staged_name;
  if (OB_ISNULL(ctx)) {
    // The guard is installed by the statement resolver, so this can only be reached by
    // a column definition that is resolved outside of create/alter table.
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column define ctx is not installed", KR(ret));
  } else if (OB_ISNULL(attr_node) || T_AI_COLUMN_ATTR != attr_node->type_
             || 3 != attr_node->num_child_ || OB_ISNULL(attr_node->children_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid ai column attribute node", KR(ret));
  } else if (OB_ISNULL(params.allocator_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("resolver allocator is null", KR(ret));
  } else if (OB_FAIL(ob_write_string(*params.allocator_, column_name, staged_name))) {
    // The column schema the name came from is a temporary object, so the name is copied
    // into the statement allocator before it is staged.
    LOG_WARN("fail to copy ai column name", KR(ret));
  } else if (OB_FAIL(ctx->add(staged_name, attr_node))) {
    LOG_WARN("fail to stage ai column definition", KR(ret), K(column_name));
  }
  return ret;
}

int ObAiColumnResolver::resolve_definition(ObIAllocator &allocator,
                                           const ObColumnSchemaV2 &result_column,
                                           ParseNode *attr_node,
                                           ObAiColumnSchema &ai_column)
{
  int ret = OB_SUCCESS;
  const ObString &column_name = result_column.get_column_name_str();
  ai_column.set_allocator(&allocator);
  ai_column.set_table_id(result_column.get_table_id());
  ai_column.set_column_id(result_column.get_column_id());
  ai_column.set_result_type(result_column.get_data_type());
  if (OB_FAIL(ai_column.set_column_name(column_name))) {
    LOG_WARN("fail to set ai column name", KR(ret));
  } else if (OB_UNLIKELY(result_column.is_rowkey_column() || !result_column.is_nullable())) {
    // The result is written asynchronously, so the column stays NULL until the first
    // generation succeeds: it can be neither a key column nor NOT NULL.
    ret = OB_ERR_COLUMN_SPEC;
    LOG_WARN("ai column must be a nullable non key column", KR(ret), K(column_name));
    LOG_USER_ERROR(OB_ERR_COLUMN_SPEC, column_name.length(), column_name.ptr());
  } else if (OB_ISNULL(attr_node) || T_AI_COLUMN_ATTR != attr_node->type_
             || 3 != attr_node->num_child_ || OB_ISNULL(attr_node->children_)
             || OB_ISNULL(attr_node->children_[0])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid ai column attribute node", KR(ret));
  } else {
    const ObString func_name(static_cast<int32_t>(attr_node->children_[0]->str_len_),
                             attr_node->children_[0]->str_value_);
    if (func_name.case_compare_equal("ai_complete")) {
      ai_column.set_func_type(share::EndpointType::COMPLETION);
    } else if (func_name.case_compare_equal("ai_embed")) {
      ai_column.set_func_type(share::EndpointType::DENSE_EMBEDDING);
    } else {
      ret = OB_ERR_COLUMN_SPEC;
      LOG_WARN("unknown ai column function", KR(ret), K(func_name));
    }
  }

  // Arguments: the first one names the model, the remaining ones are the expressions the
  // result depends on. They are restricted to plain column references in this version so
  // that a dependency change can be detected with a NULL safe comparison of those
  // columns, which keeps the DML hook cheap and exact.
  ParseNode *arg_list = NULL;
  ObString model_name;
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(arg_list = attr_node->children_[1])
             || T_EXPR_LIST != arg_list->type_ || arg_list->num_child_ < 2
             || OB_ISNULL(arg_list->children_)) {
    ret = OB_ERR_COLUMN_SPEC;
    LOG_WARN("ai column requires a model name and at least one depend column",
             KR(ret), K(column_name));
  } else if (OB_FAIL(get_literal_string(*arg_list->children_[0], model_name))) {
    LOG_WARN("ai column model name must be a string literal", KR(ret), K(column_name));
  } else if (OB_FAIL(ai_column.set_model_name(model_name))) {
    LOG_WARN("fail to set ai column model name", KR(ret));
  }

  // Build the expression the background service runs:
  //   ai_complete('text_model', body)   /   ai_embed('embed_model', body)
  // Storing it in the definition means the service never has to rebuild the SQL.
  ObStringBuffer expr_buf(&allocator);
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(expr_buf.append(ai_column.is_embedding() ? "ai_embed(" : "ai_complete("))) {
    LOG_WARN("fail to format ai column expression", KR(ret));
  } else if (OB_FAIL(expr_buf.append("'"))) {
  } else if (OB_FAIL(expr_buf.append(model_name))) {
  } else if (OB_FAIL(expr_buf.append("'"))) {
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 1; OB_SUCC(ret) && i < arg_list->num_child_; ++i) {
      ParseNode *arg = arg_list->children_[i];
      if (OB_ISNULL(arg)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column argument is null", KR(ret), K(i));
      } else if (T_IDENT != arg->type_) {
        // A computed expression would have to be re-evaluated by the service and its
        // dependencies could not be tracked per column, so it is rejected explicitly
        // instead of being silently ignored.
        ret = OB_ERR_COLUMN_SPEC;
        LOG_WARN("ai column depend expression must be a column reference", KR(ret),
                 K(arg->type_), K(column_name));
      } else if (OB_FAIL(expr_buf.append(", "))) {
      } else if (OB_FAIL(expr_buf.append(ObString(static_cast<int32_t>(arg->str_len_),
                                                  arg->str_value_)))) {
      } else { /* column ids are resolved in the second phase */ }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(expr_buf.append(")"))) {
  } else if (OB_FAIL(ai_column.set_expr_str(expr_buf.string()))) {
    LOG_WARN("fail to set ai column expression", KR(ret));
  }

  // Options. sync_mode is accepted only to produce a clear error message: a synchronous
  // AI column would block the DML on a remote model call.
  if (OB_SUCC(ret)) {
    const ParseNode *opt = attr_node->children_[2];
    while (OB_SUCC(ret) && NULL != opt) {
      const ParseNode *item = NULL;
      if (T_LINK_NODE == opt->type_) {
        item = opt->children_[1];
        opt = opt->children_[0];
      } else {
        item = opt;
        opt = NULL;
      }
      if (OB_ISNULL(item)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("ai column option node is null", KR(ret));
      } else if (T_AI_COLUMN_OPTION != item->type_ || 2 != item->num_child_
                 || OB_ISNULL(item->children_[0]) || OB_ISNULL(item->children_[1])) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid ai column option node", KR(ret));
      } else {
        const ObString option_name(static_cast<int32_t>(item->children_[0]->str_len_),
                                   item->children_[0]->str_value_);
        int64_t int_value = 0;
        ObString str_value;
        if (option_name.case_compare_equal("max_retry")) {
          if (OB_FAIL(get_literal_int(*item->children_[1], int_value))) {
          } else if (int_value < 0) {
            ret = OB_ERR_COLUMN_SPEC;
          } else {
            ai_column.set_max_retry(int_value);
          }
        } else if (option_name.case_compare_equal("timeout_sec")) {
          if (OB_FAIL(get_literal_int(*item->children_[1], int_value))) {
          } else if (int_value <= 0) {
            ret = OB_ERR_COLUMN_SPEC;
          } else {
            ai_column.set_timeout_sec(int_value);
          }
        } else if (option_name.case_compare_equal("sync_mode")) {
          if (OB_FAIL(get_literal_string(*item->children_[1], str_value))) {
          } else if (str_value.case_compare_equal("async")) {
            ai_column.set_sync_mode_async(true);
          } else {
            ret = OB_NOT_SUPPORTED;
            LOG_USER_ERROR(OB_NOT_SUPPORTED, "synchronous ai column");
          }
        } else if (option_name.case_compare_equal("prompt_hint")) {
          if (OB_FAIL(get_literal_string(*item->children_[1], str_value))) {
          } else if (OB_FAIL(ai_column.set_prompt_hint(str_value))) {
          }
        } else {
          ret = OB_ERR_COLUMN_SPEC;
          LOG_WARN("unknown ai column option", KR(ret), K(option_name));
        }
        if (OB_FAIL(ret)) {
          LOG_WARN("fail to resolve ai column option", KR(ret), K(option_name));
        }
      }
    }
  }

  // Result type, plus the vector dimension that lets the write back path reject a wrong
  // length embedding instead of storing it.
  if (OB_SUCC(ret)) {
    int64_t dimension = 0;
    if (ai_column.is_embedding()) {
      // A vector result column carries its dimension in extended_type_info, so the same
      // value is used for the metadata, for the write back check and for the vector index.
      if (OB_FAIL(get_vector_dimension(result_column, dimension))) {
        LOG_WARN("embedding ai column requires a vector result column", KR(ret), K(column_name));
        ret = OB_ERR_COLUMN_SPEC;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (FALSE_IT(ai_column.set_vector_dimension(dimension))) {
    } else if (OB_FAIL(ai_column.check_result_type(result_column.get_data_type(), dimension))) {
      LOG_WARN("ai column result type is not supported", KR(ret), K(column_name));
      // The error code of the check is an internal one, the client is told about the
      // definition instead of about an invalid argument.
      ret = OB_ERR_COLUMN_SPEC;
    }
    if (OB_FAIL(ret)) {
      // A missing dimension and a mismatched type get the same message: both mean that the
      // declaration of this very column can not hold the answer of the model.
      LOG_USER_ERROR(OB_ERR_COLUMN_SPEC, column_name.length(), column_name.ptr());
    } else if (!ai_column.is_embedding() && ai_column.get_prompt_hint().empty()) {
      // 根据结果类型补充提示词: the definition carries the hint that tells the model which
      // shape of answer the write back path accepts. An explicit prompt_hint wins.
      const char *hint = type_prompt_hint(result_column.get_data_type());
      if (OB_FAIL(ai_column.set_prompt_hint(
              ObString(static_cast<int32_t>(STRLEN(hint)), hint)))) {
        LOG_WARN("fail to set ai column prompt hint", KR(ret));
      }
    }
  }
  return ret;
}

int ObAiColumnResolver::check_hidden_column_conflict(ObIAllocator &allocator,
                                                     const ObAiColumnSchema &ai_column,
                                                     const ObTableSchema &table_schema)
{
  int ret = OB_SUCCESS;
  ObString hidden_name;
  for (int64_t i = 0; OB_SUCC(ret)
       && i < static_cast<int64_t>(ARRAYSIZEOF(HIDDEN_COLUMN_SPECS)); ++i) {
    if (OB_FAIL(ObAiColumnSchema::build_hidden_column_name(allocator,
                                                           HIDDEN_COLUMN_SPECS[i].prefix_,
                                                           ai_column.get_column_name(),
                                                           hidden_name))) {
      LOG_WARN("fail to build hidden column name", KR(ret));
    } else if (NULL != find_column_by_name(table_schema, hidden_name)) {
      ret = OB_ERR_COLUMN_SPEC;
      LOG_WARN("ai column hidden column name collides with an existing column",
               KR(ret), K(ai_column.get_column_name()), K(hidden_name));
    }
  }
  return ret;
}

int ObAiColumnResolver::resolve_create_definitions(ObIAllocator &allocator,
                                                   const ObResolverParams &params,
                                                   ObTableSchema &table_schema,
                                                   uint64_t &next_column_id)
{
  int ret = OB_SUCCESS;
  ObAiColumnDefineCtx *ctx = params.ai_column_define_ctx_;
  ObSEArray<ObAiColumnDefineItem, 8> items;
  ObSEArray<ObString, 8> staged_names;
  if (OB_ISNULL(ctx) || ctx->empty()) {
    // No AI column in this statement: the common case, nothing to do.
  } else if (OB_FAIL(ctx->check_table_id(table_schema.get_table_id()))) {
    LOG_WARN("ai column definition is staged for another table", KR(ret),
             K(table_schema.get_table_id()), KPC(ctx));
  } else if (OB_FAIL(ctx->consume(items))) {
    LOG_WARN("fail to consume staged ai column definitions", KR(ret));
  }
  // The staging area is empty from here on, so a failure below cannot leak into the next
  // statement of the session.
  for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
    if (OB_FAIL(staged_names.push_back(items.at(i).column_name_))) {
      LOG_WARN("fail to record staged ai column name", KR(ret), K(i));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
    const ObAiColumnDefineItem &item = items.at(i);
    ObColumnSchemaV2 *result_column = table_schema.get_column_schema(item.column_name_);
    if (OB_ISNULL(result_column)) {
      ret = OB_ERR_COLUMN_SPEC;
      LOG_WARN("ai column does not exist", KR(ret), K(item.column_name_));
    } else if (result_column->is_ai_column()) {
      ret = OB_ERR_COLUMN_SPEC;
      LOG_WARN("column is declared as an ai column twice", KR(ret), K(item.column_name_));
    } else {
      result_column->set_is_ai_column(true);
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
    const ObAiColumnDefineItem &item = items.at(i);
    ObColumnSchemaV2 *result_column = table_schema.get_column_schema(item.column_name_);
    ObAiColumnSchema ai_column(&allocator);
    ObString info;
    if (OB_ISNULL(result_column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column disappeared", KR(ret), K(item.column_name_));
    } else if (OB_FAIL(resolve_definition(allocator, *result_column, item.attr_node_, ai_column))) {
      LOG_WARN("fail to resolve ai column definition", KR(ret), K(item.column_name_));
    } else if (OB_FAIL(resolve_depend_columns(item.attr_node_, item.column_name_,
                                              result_column->get_column_id(), table_schema,
                                              NULL, staged_names, ai_column))) {
      LOG_WARN("fail to resolve ai column depend columns", KR(ret), K(item.column_name_));
    } else if (OB_FAIL(check_hidden_column_conflict(allocator, ai_column, table_schema))) {
      LOG_WARN("ai column hidden column conflict", KR(ret), K(item.column_name_));
    } else if (OB_FAIL(ai_column.serialize(allocator, info))) {
      LOG_WARN("fail to serialize ai column definition", KR(ret), K(item.column_name_));
    } else if (OB_FAIL(result_column->set_ai_column_info(info))) {
      LOG_WARN("fail to store ai column definition", KR(ret), K(item.column_name_));
    } else {
      // The hidden columns are appended at the end of the table, one id per column, so
      // that the column id space stays dense and stable.
      for (int64_t j = 0; OB_SUCC(ret)
           && j < static_cast<int64_t>(ARRAYSIZEOF(HIDDEN_COLUMN_SPECS)); ++j) {
        ObColumnSchemaV2 hidden_column;
        ObString hidden_name;
        if (OB_FAIL(ObAiColumnSchema::build_hidden_column_name(allocator,
                                                               HIDDEN_COLUMN_SPECS[j].prefix_,
                                                               item.column_name_,
                                                               hidden_name))) {
          LOG_WARN("fail to build hidden column name", KR(ret));
        } else if (OB_FAIL(build_hidden_column(hidden_name, HIDDEN_COLUMN_SPECS[j].type_,
                                               HIDDEN_COLUMN_SPECS[j].role_flag_,
                                               hidden_column))) {
          LOG_WARN("fail to build hidden column", KR(ret), K(hidden_name));
        } else if (FALSE_IT(hidden_column.set_column_id(++next_column_id))) {
        } else if (OB_FAIL(table_schema.add_column(hidden_column))) {
          LOG_WARN("fail to add hidden column", KR(ret), K(hidden_name));
        }
      }
    }
  }
  return ret;
}

int ObAiColumnResolver::resolve_alter_definitions(ObIAllocator &allocator,
                                                  const ObResolverParams &params,
                                                  const ObTableSchema &base_table_schema,
                                                  AlterTableSchema &alter_table_schema)
{
  int ret = OB_SUCCESS;
  ObAiColumnDefineCtx *ctx = params.ai_column_define_ctx_;
  ObSEArray<ObAiColumnDefineItem, 8> items;
  ObSEArray<ObString, 8> staged_names;
  if (OB_ISNULL(ctx) || ctx->empty()) {
    // No AI column in this statement.
  } else if (OB_FAIL(ctx->check_table_id(base_table_schema.get_table_id()))) {
    // A staged definition may only be applied to the table it was staged for.
    LOG_WARN("ai column definition is staged for another table", KR(ret),
             K(base_table_schema.get_table_id()), KPC(ctx));
  } else if (OB_FAIL(ctx->consume(items))) {
    LOG_WARN("fail to consume staged ai column definitions", KR(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
    const ObAiColumnDefineItem &item = items.at(i);
    if (OB_FAIL(staged_names.push_back(item.column_name_))) {
      LOG_WARN("fail to record staged ai column name", KR(ret), K(i));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
    const ObAiColumnDefineItem &item = items.at(i);
    AlterColumnSchema *alter_column = NULL;
    for (int64_t idx = 0; OB_SUCC(ret) && NULL == alter_column
         && idx < alter_table_schema.get_column_count(); ++idx) {
      ObColumnSchemaV2 *cur = alter_table_schema.get_column_schema_by_idx(idx);
      if (OB_ISNULL(cur)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("altered column is null", KR(ret), K(idx));
      } else if (cur->get_column_name_str() == item.column_name_) {
        alter_column = static_cast<AlterColumnSchema *>(cur);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(alter_column)) {
      ret = OB_ERR_COLUMN_SPEC;
      LOG_WARN("ai column was staged but the altered column is missing",
               KR(ret), K(item.column_name_));
    } else if (alter_column->is_ai_column()) {
      ret = OB_ERR_COLUMN_SPEC;
      LOG_WARN("column is declared as an ai column twice", KR(ret), K(item.column_name_));
    } else {
      alter_column->set_is_ai_column(true);
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
    const ObAiColumnDefineItem &item = items.at(i);
    AlterColumnSchema *alter_column = NULL;
    for (int64_t idx = 0; NULL == alter_column
         && idx < alter_table_schema.get_column_count(); ++idx) {
      ObColumnSchemaV2 *cur = alter_table_schema.get_column_schema_by_idx(idx);
      if (NULL != cur && cur->get_column_name_str() == item.column_name_) {
        alter_column = static_cast<AlterColumnSchema *>(cur);
      }
    }
    if (OB_ISNULL(alter_column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("altered ai column disappeared", KR(ret), K(item.column_name_));
    } else {
      ObAiColumnSchema ai_column(&allocator);
      ObString info;
      if (OB_FAIL(resolve_definition(allocator, *alter_column, item.attr_node_, ai_column))) {
        LOG_WARN("fail to resolve ai column definition", KR(ret), K(item.column_name_));
      } else if (OB_FAIL(resolve_depend_columns(item.attr_node_, item.column_name_,
                                                alter_column->get_column_id(), base_table_schema,
                                                &alter_table_schema, staged_names, ai_column))) {
        LOG_WARN("fail to resolve ai column depend columns", KR(ret), K(item.column_name_));
      } else if (OB_FAIL(ai_column.serialize(allocator, info))) {
        LOG_WARN("fail to serialize ai column definition", KR(ret), K(item.column_name_));
      } else if (OB_FAIL(alter_column->set_ai_column_info(info))) {
        LOG_WARN("fail to store ai column definition", KR(ret), K(item.column_name_));
      } else {
        // MODIFY / CHANGE reuse the hidden columns that the original ADD COLUMN created,
        // only a first time declaration has to generate them.
        ObString status_name;
        if (OB_FAIL(ObAiColumnSchema::build_hidden_column_name(allocator,
                                                               HIDDEN_COLUMN_SPECS[0].prefix_,
                                                               item.column_name_,
                                                               status_name))) {
          LOG_WARN("fail to build hidden column name", KR(ret));
        } else if (NULL != find_column_by_name(alter_table_schema, status_name)) {
          // already added by this statement
        } else if (NULL != find_column_by_name(base_table_schema, status_name)) {
          // already added by a previous statement
        } else {
          for (int64_t j = 0; OB_SUCC(ret)
               && j < static_cast<int64_t>(ARRAYSIZEOF(HIDDEN_COLUMN_SPECS)); ++j) {
            AlterColumnSchema hidden_column;
            ObString hidden_name;
            if (OB_FAIL(ObAiColumnSchema::build_hidden_column_name(allocator,
                                                                   HIDDEN_COLUMN_SPECS[j].prefix_,
                                                                   item.column_name_,
                                                                   hidden_name))) {
              LOG_WARN("fail to build hidden column name", KR(ret));
            } else if (OB_FAIL(build_hidden_column(hidden_name, HIDDEN_COLUMN_SPECS[j].type_,
                                                   HIDDEN_COLUMN_SPECS[j].role_flag_,
                                                   hidden_column))) {
              LOG_WARN("fail to build hidden column", KR(ret), K(hidden_name));
            } else if (FALSE_IT(hidden_column.alter_type_ = OB_DDL_ADD_COLUMN)) {
            } else if (OB_FAIL(alter_table_schema.add_alter_column(hidden_column, true))) {
              LOG_WARN("fail to add hidden column", KR(ret), K(hidden_name));
            }
          }
        }
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// ObAiColumnDefineCtx / ObAiColumnDefineGuard
////////////////////////////////////////////////////////////////////////////////

int ObAiColumnDefineCtx::add(const ObString &column_name, ParseNode *attr_node)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(attr_node) || column_name.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid ai column define item", KR(ret), K(column_name));
  } else if (items_.count() >= MAX_AI_COLUMN_PER_STMT) {
    ret = OB_ERR_TOO_MANY_COLUMNS;
    LOG_WARN("too many ai columns in one statement", KR(ret), K(column_name));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < items_.count(); ++i) {
      if (items_.at(i).column_name_.case_compare_equal(column_name)) {
        ret = OB_ERR_COLUMN_SPEC;
        LOG_WARN("the same column has more than one ai column clause",
                 KR(ret), K(column_name));
      }
    }
    if (OB_SUCC(ret)) {
      ObAiColumnDefineItem item;
      item.column_name_ = column_name;
      item.attr_node_ = attr_node;
      if (OB_FAIL(items_.push_back(item))) {
        LOG_WARN("fail to stage ai column definition", KR(ret), K(column_name));
      }
    }
  }
  return ret;
}

int ObAiColumnDefineCtx::consume(ObIArray<ObAiColumnDefineItem> &items)
{
  int ret = OB_SUCCESS;
  items.reuse();
  for (int64_t i = 0; OB_SUCC(ret) && i < items_.count(); ++i) {
    if (OB_FAIL(items.push_back(items_.at(i)))) {
      LOG_WARN("fail to copy ai column define item", KR(ret), K(i));
    }
  }
  // Cleared immediately, so that an error while applying the definitions cannot leave
  // them behind for the next statement of the session.
  items_.reuse();
  return ret;
}

int ObAiColumnDefineCtx::check_table_id(const uint64_t table_id) const
{
  int ret = OB_SUCCESS;
  // OB_INVALID_ID means the identifier is not known yet, which is the case for
  // CREATE TABLE: the table does not exist before the statement succeeds.
  if (OB_INVALID_ID != table_id_ && table_id_ != table_id) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

ObAiColumnDefineGuard::ObAiColumnDefineGuard(ObIAllocator *allocator, ObResolverParams &params)
  : params_(params), ctx_(NULL), owns_ctx_(false), ret_(OB_SUCCESS)
{
  if (NULL != params.ai_column_define_ctx_) {
    // An outer resolver already installed the staging area.
    ctx_ = params.ai_column_define_ctx_;
  } else if (OB_ISNULL(allocator)) {
    // Without an allocator the staging area cannot be created; staging a definition will
    // fail explicitly instead of crashing.
    ret_ = OB_INVALID_ARGUMENT;
  } else {
    void *buf = allocator->alloc(sizeof(ObAiColumnDefineCtx));
    if (OB_ISNULL(buf)) {
      ret_ = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("fail to allocate ai column define ctx", K(ret_));
    } else {
      ctx_ = new (buf) ObAiColumnDefineCtx();
      params_.ai_column_define_ctx_ = ctx_;
      owns_ctx_ = true;
    }
  }
}

ObAiColumnDefineGuard::~ObAiColumnDefineGuard()
{
  // Every exit path of the statement resolver goes through here, so a staged definition
  // never survives the statement it was written in.
  if (owns_ctx_ && NULL != ctx_) {
    ctx_->reset();
    params_.ai_column_define_ctx_ = NULL;
    ctx_->~ObAiColumnDefineCtx();
    ctx_ = NULL;
  }
}

int ObAiColumnDefineGuard::check_all_consumed(const int resolve_ret) const
{
  int ret = OB_SUCCESS;
  if (NULL != ctx_ && !ctx_->empty()) {
    if (OB_SUCCESS == resolve_ret) {
      // The statement succeeded but an AI column clause was never applied: report it
      // rather than silently creating a table without the requested column.
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column definition was staged but never consumed", KR(ret), KPC(ctx_));
    } else {
      LOG_WARN("ai column definition left behind by a failed resolve",
               K(resolve_ret), KPC(ctx_));
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase

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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/cmd/ob_ai_column_service.h"

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "lib/allocator/page_arena.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_parse.h"
#include "lib/lock/ob_spin_lock.h"
#include "lib/thread/thread_pool.h"
#include "lib/time/ob_time_utility.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "share/ob_server_struct.h"
#include "share/ob_errno.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_service.h"
#include "share/schema/ob_table_schema.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"
#include "sql/resolver/ddl/ob_alter_table_stmt.h"
#include "sql/session/ob_inner_sql_connection.h"
#include "sql/session/ob_sql_session_info.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "query/session/ob_inner_sql_connection_access.h"

namespace oceanbase
{
namespace sql
{
namespace
{
constexpr int NAME_LEN = 128;
constexpr int EXPR_LEN = 1500;
constexpr int DEP_LEN = 512;
constexpr int PROMPT_LEN = 256;
constexpr int REQ_LEN = 80;
constexpr int PRED_LEN = 512;
constexpr int SQL_LEN = 16384;
constexpr int RESULT_LEN = 4000;
constexpr int MAX_META = 32;
constexpr int MAX_STAGED = 8;
constexpr int MAX_RETRY_ROWS = 256;
constexpr int MAX_CACHE = 96;
constexpr int KIND_COMPLETE = 1;
constexpr int KIND_EMBED = 2;
constexpr const char *JSON_PROMPT = "Return exactly one valid JSON value";
constexpr const char *INT_PROMPT = "Return exactly one base-10 integer";

struct Meta
{
  uint64_t table_id;
  uint64_t column_id;
  uint64_t state_column_id;
  uint64_t request_column_id;
  uint64_t spec_column_id;
  char database_name[NAME_LEN];
  char table_name[NAME_LEN];
  char column_name[NAME_LEN];
  char state_column_name[NAME_LEN];
  char request_column_name[NAME_LEN];
  char spec_column_name[NAME_LEN];
  char model_name[NAME_LEN];
  char expr_sql[EXPR_LEN];
  char dep_columns[DEP_LEN];
  char type_prompt[PROMPT_LEN];
  char normalized_expr[EXPR_LEN];
  int64_t max_retries;
  int kind;
  int result_type;
  int64_t vector_dim;
  bool persisted;
  bool not_ready;
};

struct Staged
{
  char column_name[NAME_LEN];
  char model_name[NAME_LEN];
  char expr_sql[EXPR_LEN];
  char dep_columns[DEP_LEN];
  char type_prompt[PROMPT_LEN];
  int64_t max_retries;
  int kind;
  int result_type;
  int64_t vector_dim;
  bool used;
};

struct SqlBuf
{
  char data[SQL_LEN];
  int len;
  SqlBuf() : len(0) { data[0] = '\0'; }
  int append(const char *text, int n)
  {
    int ret = OB_SUCCESS;
    if (n < 0) {
      n = text == nullptr ? 0 : static_cast<int>(strlen(text));
    }
    if (text == nullptr || n < 0 || len + n >= SQL_LEN) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      if (n > 0) {
        memcpy(data + len, text, n);
      }
      len += n;
      data[len] = '\0';
    }
    return ret;
  }
  int fmt(const char *format, ...) __attribute__((format(printf, 2, 3)))
  {
    int ret = OB_SUCCESS;
    va_list args;
    va_start(args, format);
    int remain = SQL_LEN - len;
    int wrote = vsnprintf(data + len, remain, format, args);
    va_end(args);
    if (wrote < 0 || wrote >= remain) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      len += wrote;
    }
    return ret;
  }
};

static common::ObSpinLock g_lock;
static Meta g_metas[MAX_META];
static int g_meta_count = 0;
static thread_local Staged g_staged[MAX_STAGED];
static thread_local int g_staged_count = 0;
static thread_local int g_internal_depth = 0;
static int64_t g_request_seq = 0;

struct LockGuard
{
  explicit LockGuard(common::ObSpinLock &lock) : lock_(lock), locked_(OB_SUCCESS == lock_.lock()) {}
  ~LockGuard() { if (locked_) { lock_.unlock(); } }
  bool locked() const { return locked_; }
  common::ObSpinLock &lock_;
  bool locked_;
};

void copy_text(char *dst, int dst_len, const char *src, int src_len)
{
  if (dst == nullptr || dst_len <= 0) {
  } else {
    if (src == nullptr || src_len < 0) {
      src_len = 0;
    }
    if (src_len >= dst_len) {
      src_len = dst_len - 1;
    }
    if (src_len > 0 && src != nullptr) {
      memcpy(dst, src, src_len);
    }
    dst[src_len] = '\0';
  }
}

void copy_ob(char *dst, int dst_len, const common::ObString &src)
{
  copy_text(dst, dst_len, src.ptr(), src.length());
}

bool text_eq(const char *left, const common::ObString &right)
{
  int left_len = left == nullptr ? 0 : static_cast<int>(strlen(left));
  return left_len == right.length()
         && (left_len == 0 || 0 == strncasecmp(left, right.ptr(), left_len));
}

bool text_eq_raw(const char *left, const char *right, int right_len)
{
  int left_len = left == nullptr ? 0 : static_cast<int>(strlen(left));
  return left_len == right_len
         && (left_len == 0 || 0 == strncasecmp(left, right, left_len));
}

int quote_ident(SqlBuf &sql, const char *name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("`", 1))) {
  } else if (name != nullptr) {
    for (const char *p = name; OB_SUCC(ret) && *p != '\0'; ++p) {
      if (*p == '`' && OB_FAIL(sql.append("``", 2))) {
      } else if (*p != '`' && OB_FAIL(sql.append(p, 1))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("`", 1);
  }
  return ret;
}

int quote_literal(SqlBuf &sql, const char *text, int text_len)
{
  int ret = OB_SUCCESS;
  if (text == nullptr) {
    text_len = 0;
  }
  if (OB_FAIL(sql.append("'", 1))) {
  }
  for (int i = 0; OB_SUCC(ret) && i < text_len; ++i) {
    if (text[i] == '\\' && OB_FAIL(sql.append("\\\\", 2))) {
    } else if (text[i] == '\'' && OB_FAIL(sql.append("''", 2))) {
    } else if (text[i] != '\\' && text[i] != '\'' && OB_FAIL(sql.append(text + i, 1))) {
    }
  }
  if (OB_SUCC(ret)) {
    ret = sql.append("'", 1);
  }
  return ret;
}

void make_request_id(char *dst, int dst_len)
{
  int64_t seq = __sync_add_and_fetch(&g_request_seq, 1);
  snprintf(dst, dst_len, "r%ld_%ld",
           static_cast<long>(common::ObTimeUtility::current_time()),
           static_cast<long>(seq));
}

bool datum_same(const common::ObDatum &left, const common::ObDatum &right)
{
  bool same = false;
  if (left.is_null() && right.is_null()) {
    same = true;
  } else if (left.is_null() || right.is_null() || left.len_ != right.len_) {
    same = false;
  } else {
    same = left.len_ == 0 || 0 == memcmp(left.ptr_, right.ptr_, left.len_);
  }
  return same;
}

int find_idx(const ObDMLBaseCtDef &ctdef, uint64_t column_id)
{
  int idx = -1;
  for (int64_t i = 0; idx < 0 && i < ctdef.column_ids_.count(); ++i) {
    if (ctdef.column_ids_.at(i) == column_id) {
      idx = static_cast<int>(i);
    }
  }
  return idx;
}

Meta *find_meta_locked(uint64_t table_id, uint64_t column_id)
{
  Meta *found = nullptr;
  for (int i = 0; found == nullptr && i < g_meta_count; ++i) {
    if (g_metas[i].table_id == table_id && g_metas[i].column_id == column_id && column_id != 0) {
      found = &g_metas[i];
    }
  }
  return found;
}

Meta *find_meta_name_locked(uint64_t table_id, const char *column_name)
{
  Meta *found = nullptr;
  for (int i = 0; found == nullptr && i < g_meta_count; ++i) {
    if (g_metas[i].table_id == table_id
        && text_eq_raw(g_metas[i].column_name, column_name, static_cast<int>(strlen(column_name)))) {
      found = &g_metas[i];
    }
  }
  return found;
}

bool ai_name_locked(const char *name, int name_len)
{
  bool found = false;
  for (int i = 0; !found && i < g_staged_count; ++i) {
    found = text_eq_raw(g_staged[i].column_name, name, name_len);
  }
  for (int i = 0; !found && i < g_meta_count; ++i) {
    found = text_eq_raw(g_metas[i].column_name, name, name_len);
  }
  return found;
}

int append_dep(char *deps, int deps_len, const char *name, int name_len)
{
  int ret = OB_SUCCESS;
  int used = static_cast<int>(strlen(deps));
  int extra = name_len + (used > 0 ? 1 : 0);
  if (used + extra >= deps_len) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    if (used > 0) {
      deps[used++] = ',';
    }
    memcpy(deps + used, name, name_len);
    deps[used + name_len] = '\0';
  }
  return ret;
}

void collect_deps(ParseNode *node, char *deps, int deps_len, bool &chained, int &ret)
{
  if (OB_SUCCESS != ret || node == nullptr || chained) {
  } else if (T_COLUMN_REF == node->type_ && node->num_child_ >= 3 && node->children_[2] != nullptr
             && node->children_[2]->str_value_ != nullptr) {
    const char *name = node->children_[2]->str_value_;
    int name_len = static_cast<int>(node->children_[2]->str_len_);
    if (ai_name_locked(name, name_len)) {
      chained = true;
    } else if (OB_SUCCESS != (ret = append_dep(deps, deps_len, name, name_len))) {
    }
  } else {
    for (int i = 0; OB_SUCCESS == ret && !chained && i < node->num_child_; ++i) {
      collect_deps(node->children_[i], deps, deps_len, chained, ret);
    }
  }
}

int split_args(const char *sql, int sql_len, const char **args, int *arg_lens, int max_args, int &arg_count)
{
  int ret = OB_SUCCESS;
  arg_count = 0;
  int i = 0;
  while (i < sql_len && isspace(static_cast<unsigned char>(sql[i]))) {
    ++i;
  }
  while (i < sql_len && (isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) {
    ++i;
  }
  while (i < sql_len && isspace(static_cast<unsigned char>(sql[i]))) {
    ++i;
  }
  if (i >= sql_len || sql[i] != '(') {
    ret = OB_ERR_PARSER_SYNTAX;
  } else {
    ++i;
    while (OB_SUCC(ret) && i < sql_len) {
      while (i < sql_len && isspace(static_cast<unsigned char>(sql[i]))) {
        ++i;
      }
      if (i < sql_len && sql[i] == ')') {
        break;
      }
      int start = i;
      int depth = 0;
      bool in_quote = false;
      char quote = 0;
      for (; i < sql_len; ++i) {
        char c = sql[i];
        if (in_quote) {
          if (c == quote) {
            if (i + 1 < sql_len && sql[i + 1] == quote) {
              ++i;
            } else {
              in_quote = false;
            }
          }
        } else if (c == '\'' || c == '"' || c == '`') {
          in_quote = true;
          quote = c;
        } else if (c == '(') {
          ++depth;
        } else if (c == ')') {
          if (depth == 0) {
            break;
          }
          --depth;
        } else if (c == ',' && depth == 0) {
          break;
        }
      }
      int end = i;
      while (end > start && isspace(static_cast<unsigned char>(sql[end - 1]))) {
        --end;
      }
      if (arg_count >= max_args) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        args[arg_count] = sql + start;
        arg_lens[arg_count] = end - start;
        ++arg_count;
      }
      if (OB_SUCC(ret) && i < sql_len && sql[i] == ',') {
        ++i;
      }
    }
  }
  return ret;
}

int64_t vector_dim(const share::schema::ObColumnSchemaV2 &column)
{
  int64_t dim = 0;
  if (column.is_collection() && column.get_extended_type_info().count() > 0) {
    const common::ObString &info = column.get_extended_type_info().at(0);
    const char *text = info.ptr();
    int len = info.length();
    for (int i = 0; i < len; ++i) {
      if (text[i] == '(') {
        dim = strtoll(text + i + 1, nullptr, 10);
        break;
      }
    }
  }
  return dim;
}

int add_hidden_column(share::schema::ObTableSchema &schema,
                      const char *name,
                      int64_t length,
                      const char *comment,
                      uint64_t &next_column_id,
                      uint64_t &column_id)
{
  int ret = OB_SUCCESS;
  share::schema::ObColumnSchemaV2 column;
  common::ObCollationType collation = schema.get_collation_type();
  if (CS_TYPE_INVALID == collation) {
    collation = CS_TYPE_UTF8MB4_BIN;
  }
  column_id = ++next_column_id;
  column.set_table_id(schema.get_table_id());
  column.set_column_id(column_id);
  column.set_rowkey_position(0);
  column.set_index_position(0);
  column.set_tbl_part_key_pos(0);
  column.set_data_type(common::ObVarcharType);
  column.set_data_length(length);
  column.set_collation_type(collation);
  column.set_charset_type(common::ObCharset::charset_type_by_coll(collation));
  column.set_nullable(true);
  column.set_is_hidden(true);
  column.set_prev_column_id(UINT64_MAX);
  column.set_next_column_id(UINT64_MAX);
  if (OB_FAIL(column.set_column_name(name))) {
  } else if (comment != nullptr && comment[0] != '\0' && OB_FAIL(column.set_comment(comment))) {
  } else if (OB_FAIL(schema.add_column(column))) {
    LOG_WARN("add hidden ai column failed", K(ret), K(name));
  }
  return ret;
}

int fill_hidden(Meta &meta, share::schema::ObTableSchema *schema, ObAlterTableStmt *stmt, uint64_t &next_column_id)
{
  int ret = OB_SUCCESS;
  char comment[EXPR_LEN];
  snprintf(comment, sizeof(comment), "AIv1");
  if (schema != nullptr) {
    uint64_t ignored = 0;
    if (OB_FAIL(add_hidden_column(*schema, meta.state_column_name, 32, nullptr, next_column_id, meta.state_column_id))) {
    } else if (OB_FAIL(add_hidden_column(*schema, meta.request_column_name, 80, nullptr, next_column_id, meta.request_column_id))) {
    } else if (OB_FAIL(add_hidden_column(*schema, meta.spec_column_name, 64, comment, next_column_id, meta.spec_column_id))) {
    } else if (OB_NOT_NULL(schema->get_column_schema(meta.column_name))) {
      meta.column_id = schema->get_column_schema(meta.column_name)->get_column_id();
    }
  } else if (stmt != nullptr) {
    const char *names[3] = {meta.state_column_name, meta.request_column_name, meta.spec_column_name};
    int64_t lengths[3] = {32, 80, 64};
    for (int i = 0; OB_SUCC(ret) && i < 3; ++i) {
      share::schema::AlterColumnSchema column;
      common::ObCollationType collation = CS_TYPE_UTF8MB4_BIN;
      column.alter_type_ = OB_DDL_ADD_COLUMN;
      column.set_data_type(common::ObVarcharType);
      column.set_data_length(lengths[i]);
      column.set_collation_type(collation);
      column.set_charset_type(common::ObCharset::charset_type_by_coll(collation));
      column.set_nullable(true);
      column.set_is_hidden(true);
      column.set_rowkey_position(0);
      column.set_index_position(0);
      column.set_tbl_part_key_pos(0);
      column.set_prev_column_id(UINT64_MAX);
      column.set_next_column_id(UINT64_MAX);
      if (i == 2) {
        column.is_set_comment_ = true;
        if (OB_FAIL(column.set_comment(comment))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(column.set_column_name(names[i]))) {
      } else if (OB_FAIL(stmt->add_column(column))) {
        LOG_WARN("add alter hidden ai column failed", K(ret), K(names[i]));
      }
    }
  }
  return ret;
}

Meta *upsert_meta_locked(const Meta &incoming)
{
  Meta *slot = nullptr;
  for (int i = 0; slot == nullptr && i < g_meta_count; ++i) {
    if (text_eq_raw(g_metas[i].database_name, incoming.database_name, static_cast<int>(strlen(incoming.database_name)))
        && text_eq_raw(g_metas[i].table_name, incoming.table_name, static_cast<int>(strlen(incoming.table_name)))
        && text_eq_raw(g_metas[i].column_name, incoming.column_name, static_cast<int>(strlen(incoming.column_name)))) {
      g_metas[i] = incoming;
      // A definition staged without a table id belongs to a brand new table, so any
      // binding from a previous table with the same name must be dropped.
      g_metas[i].persisted = false;
      g_metas[i].not_ready = false;
      slot = &g_metas[i];
    }
  }
  if (slot == nullptr && g_meta_count < MAX_META) {
    g_metas[g_meta_count] = incoming;
    slot = &g_metas[g_meta_count];
    ++g_meta_count;
  }
  return slot;
}

int proxy_write(const char *sql, int64_t &affected)
{
  int ret = OB_SUCCESS;
  affected = 0;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(GCTX.sql_proxy_->write(sql, 0, affected))) {
    LOG_WARN("ai column proxy write failed", K(ret), K(sql));
  }
  return ret;
}

int proxy_read(const char *sql, common::ObMySQLProxy::MySQLResult &result)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(GCTX.sql_proxy_->read(result, sql, 0))) {
    LOG_WARN("ai column proxy read failed", K(ret), K(sql));
  }
  return ret;
}

int user_write(ObSQLSessionInfo *session, const char *sql, int64_t *affected)
{
  int ret = OB_SUCCESS;
  common::sqlclient::ObISQLConnectionGuard guard;
  common::sqlclient::ObISQLConnection *base = nullptr;
  ObIInnerSQLConnection *conn = nullptr;
  ObSQLSessionInfo::StmtSavedValue saved_session;
  ObIInnerSQLConnection::SavedValue saved_conn;
  bool nested = false;
  int64_t rows = 0;
  if (OB_ISNULL(session) || OB_ISNULL(sql)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_spi_connection_with_external_session(session, guard))) {
  } else if (OB_ISNULL(base = guard.get_ptr()) || OB_ISNULL(conn = as_inner_sql_connection(base))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(conn->begin_nested_session(saved_session, saved_conn, true))) {
  } else {
    nested = true;
    ret = conn->execute_write(common::ObString(sql), rows, false);
  }
  if (nested) {
    int tmp = conn->end_nested_session(saved_session, saved_conn);
    if (OB_SUCC(ret)) {
      ret = tmp;
    }
  }
  if (affected != nullptr) {
    *affected = rows;
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("ai column user write failed", K(ret), K(sql));
  }
  return ret;
}

int user_query(ObSQLSessionInfo *session, const char *sql, bool &has_row)
{
  int ret = OB_SUCCESS;
  has_row = false;
  common::sqlclient::ObISQLConnectionGuard guard;
  common::sqlclient::ObISQLConnection *base = nullptr;
  ObIInnerSQLConnection *conn = nullptr;
  ObSQLSessionInfo::StmtSavedValue saved_session;
  ObIInnerSQLConnection::SavedValue saved_conn;
  bool nested = false;
  if (OB_ISNULL(session) || OB_ISNULL(sql)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_spi_connection_with_external_session(session, guard))) {
  } else if (OB_ISNULL(base = guard.get_ptr()) || OB_ISNULL(conn = as_inner_sql_connection(base))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(conn->begin_nested_session(saved_session, saved_conn, true))) {
  } else {
    nested = true;
    SMART_VAR(common::ObMySQLProxy::MySQLResult, result) {
      if (OB_FAIL(conn->execute_read(common::ObString(sql), result, false))) {
      } else if (OB_ISNULL(result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result.get_result()->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      } else {
        has_row = true;
      }
    }
  }
  if (nested) {
    int tmp = conn->end_nested_session(saved_session, saved_conn);
    if (OB_SUCC(ret)) {
      ret = tmp;
    }
  }
  return ret;
}

void ensure_worker();
void bootstrap_recovery();

int insert_metadata(const Meta &meta)
{
  int ret = OB_SUCCESS;
  SqlBuf sql;
  int64_t affected = 0;
  bool exists = false;
  if (OB_FAIL(sql.fmt("SELECT column_id FROM oceanbase.__all_ai_column WHERE table_id = %lu AND column_id = %lu",
                      meta.table_id, meta.column_id))) {
  } else {
    SMART_VAR(common::ObMySQLProxy::MySQLResult, result) {
      if (OB_FAIL(proxy_read(sql.data, result))) {
      } else if (OB_ISNULL(result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result.get_result()->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      } else {
        exists = true;
      }
    }
  }
  sql.len = 0;
  sql.data[0] = '\0';
  if (OB_FAIL(ret) || exists) {
  } else if (OB_FAIL(sql.append("INSERT INTO oceanbase.__all_ai_column("
                                "table_id, column_id, column_name, max_retries, state_column_id, "
                                "request_column_id, result_spec_column_id, normalized_expr, model_name, "
                                "expr_text, dep_columns, kind, result_type, database_name, table_name, "
                                "state_column_name, request_column_name, type_prompt, gmt_create, gmt_modified) VALUES (",
                                -1))) {
  } else if (OB_FAIL(sql.fmt("%lu, %lu, ", meta.table_id, meta.column_id))) {
  } else if (OB_FAIL(quote_literal(sql, meta.column_name, static_cast<int>(strlen(meta.column_name))))) {
  } else if (OB_FAIL(sql.fmt(", %ld, %lu, %lu, %lu, ",
                             meta.max_retries, meta.state_column_id, meta.request_column_id, meta.spec_column_id))) {
  } else if (OB_FAIL(quote_literal(sql, meta.normalized_expr, static_cast<int>(strlen(meta.normalized_expr))))) {
  } else if (OB_FAIL(sql.append(", ", 2))) {
  } else if (OB_FAIL(quote_literal(sql, meta.model_name, static_cast<int>(strlen(meta.model_name))))) {
  } else if (OB_FAIL(sql.append(", ", 2))) {
  } else if (OB_FAIL(quote_literal(sql, meta.expr_sql, static_cast<int>(strlen(meta.expr_sql))))) {
  } else if (OB_FAIL(sql.append(", ", 2))) {
  } else if (OB_FAIL(quote_literal(sql, meta.dep_columns, static_cast<int>(strlen(meta.dep_columns))))) {
  } else if (OB_FAIL(sql.fmt(", %d, %d, ", meta.kind, meta.result_type))) {
  } else if (OB_FAIL(quote_literal(sql, meta.database_name, static_cast<int>(strlen(meta.database_name))))) {
  } else if (OB_FAIL(sql.append(", ", 2))) {
  } else if (OB_FAIL(quote_literal(sql, meta.table_name, static_cast<int>(strlen(meta.table_name))))) {
  } else if (OB_FAIL(sql.append(", ", 2))) {
  } else if (OB_FAIL(quote_literal(sql, meta.state_column_name, static_cast<int>(strlen(meta.state_column_name))))) {
  } else if (OB_FAIL(sql.append(", ", 2))) {
  } else if (OB_FAIL(quote_literal(sql, meta.request_column_name, static_cast<int>(strlen(meta.request_column_name))))) {
  } else if (OB_FAIL(sql.append(", ", 2))) {
  } else if (OB_FAIL(quote_literal(sql, meta.type_prompt, static_cast<int>(strlen(meta.type_prompt))))) {
  } else if (OB_FAIL(sql.append(", now(6), now(6))", -1))) {
  } else if (OB_FAIL(proxy_write(sql.data, affected))) {
  }
  return ret;
}

int read_text(common::sqlclient::ObMySQLResult *rows, const char *column, char *dst, int dst_len)
{
  int ret = OB_SUCCESS;
  common::ObString text;
  if (OB_ISNULL(rows) || OB_ISNULL(column) || OB_ISNULL(dst)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(rows->get_varchar(column, text))) {
    if (OB_ERR_NULL_VALUE == ret) {
      dst[0] = '\0';
      ret = OB_SUCCESS;
    }
  } else {
    copy_text(dst, dst_len, text.ptr(), text.length());
  }
  return ret;
}

int load_persisted_metadata()
{
  int ret = OB_SUCCESS;
  SqlBuf sql;
  int loaded = 0;
  if (OB_FAIL(sql.append("SELECT table_id, column_id, state_column_id, request_column_id, "
                         "result_spec_column_id, column_name, max_retries, normalized_expr, "
                         "model_name, expr_text, dep_columns, kind, result_type, database_name, "
                         "table_name, state_column_name, request_column_name, type_prompt "
                         "FROM oceanbase.__all_ai_column", -1))) {
  } else {
    SMART_VAR(common::ObMySQLProxy::MySQLResult, result) {
      common::sqlclient::ObMySQLResult *rows = nullptr;
      if (OB_FAIL(proxy_read(sql.data, result))) {
      } else if (OB_ISNULL(rows = result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret) && loaded < MAX_META && OB_SUCC(rows->next())) {
        Meta meta;
        memset(&meta, 0, sizeof(meta));
        int64_t table_id = 0;
        int64_t column_id = 0;
        int64_t state_id = 0;
        int64_t request_id = 0;
        int64_t spec_id = 0;
        if (OB_FAIL(rows->get_int("table_id", table_id))) {
        } else if (OB_FAIL(rows->get_int("column_id", column_id))) {
        } else if (OB_FAIL(rows->get_int("state_column_id", state_id))) {
        } else if (OB_FAIL(rows->get_int("request_column_id", request_id))) {
        } else if (OB_FAIL(rows->get_int("result_spec_column_id", spec_id))) {
        } else if (OB_FAIL(rows->get_int("max_retries", meta.max_retries))) {
        } else if (OB_FAIL(rows->get_int("kind", meta.kind))) {
        } else if (OB_FAIL(rows->get_int("result_type", meta.result_type))) {
        } else if (OB_FAIL(read_text(rows, "column_name", meta.column_name, NAME_LEN))) {
        } else if (OB_FAIL(read_text(rows, "model_name", meta.model_name, NAME_LEN))) {
        } else if (OB_FAIL(read_text(rows, "expr_text", meta.expr_sql, EXPR_LEN))) {
        } else if (OB_FAIL(read_text(rows, "dep_columns", meta.dep_columns, DEP_LEN))) {
        } else if (OB_FAIL(read_text(rows, "database_name", meta.database_name, NAME_LEN))) {
        } else if (OB_FAIL(read_text(rows, "table_name", meta.table_name, NAME_LEN))) {
        } else if (OB_FAIL(read_text(rows, "state_column_name", meta.state_column_name, NAME_LEN))) {
        } else if (OB_FAIL(read_text(rows, "request_column_name", meta.request_column_name, NAME_LEN))) {
        } else if (OB_FAIL(read_text(rows, "type_prompt", meta.type_prompt, PROMPT_LEN))) {
        } else if (OB_FAIL(read_text(rows, "normalized_expr", meta.normalized_expr, EXPR_LEN))) {
        } else {
          meta.table_id = static_cast<uint64_t>(table_id);
          meta.column_id = static_cast<uint64_t>(column_id);
          meta.state_column_id = static_cast<uint64_t>(state_id);
          meta.request_column_id = static_cast<uint64_t>(request_id);
          meta.spec_column_id = static_cast<uint64_t>(spec_id);
          // The in-flight task rows are still in the system table, so the column
          // starts out not ready and publishes as soon as they drain.
          meta.persisted = true;
          meta.not_ready = true;
          LockGuard lock(g_lock);
          if (upsert_meta_locked(meta) == nullptr) {
            ret = OB_SIZE_OVERFLOW;
          } else {
            ++loaded;
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

int bind_published_ids(uint64_t table_id)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard guard;
  const share::schema::ObTableSchema *schema = nullptr;
  const share::schema::ObDatabaseSchema *database = nullptr;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, schema)) || OB_ISNULL(schema)) {
    ret = OB_ENTRY_NOT_EXIST;
  } else if (OB_FAIL(guard.get_database_schema(schema->get_database_id(), database)) || OB_ISNULL(database)) {
    ret = OB_ENTRY_NOT_EXIST;
  } else {
    LockGuard lock(g_lock);
    for (int i = 0; OB_SUCC(ret) && i < g_meta_count; ++i) {
      Meta &meta = g_metas[i];
      if (!text_eq(meta.database_name, database->get_database_name_str())
          || !text_eq(meta.table_name, schema->get_table_name_str())) {
      } else {
        const share::schema::ObColumnSchemaV2 *column = schema->get_column_schema(meta.column_name);
        const share::schema::ObColumnSchemaV2 *state = schema->get_column_schema(meta.state_column_name);
        const share::schema::ObColumnSchemaV2 *request = schema->get_column_schema(meta.request_column_name);
        const share::schema::ObColumnSchemaV2 *spec = schema->get_column_schema(meta.spec_column_name);
        if (OB_ISNULL(column) || OB_ISNULL(state) || OB_ISNULL(request) || OB_ISNULL(spec)) {
          ret = OB_ENTRY_NOT_EXIST;
        } else {
          meta.table_id = table_id;
          meta.column_id = column->get_column_id();
          meta.state_column_id = state->get_column_id();
          meta.request_column_id = request->get_column_id();
          meta.spec_column_id = spec->get_column_id();
        }
      }
    }
  }
  return ret;
}

int load_schema(uint64_t table_id, share::schema::ObSchemaGetterGuard &guard, const share::schema::ObTableSchema *&schema)
{
  int ret = OB_SUCCESS;
  schema = nullptr;
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, schema)) || OB_ISNULL(schema)) {
    ret = OB_TABLE_NOT_EXIST;
  }
  return ret;
}

int format_predicate(const share::schema::ObTableSchema &schema,
                     const ObDMLBaseCtDef &ctdef,
                     const common::ObChunkDatumStore::StoredRow *row,
                     char *predicate,
                     int predicate_len)
{
  int ret = OB_SUCCESS;
  SqlBuf sql;
  const common::ObRowkeyInfo &rowkey = schema.get_rowkey_info();
  if (OB_ISNULL(row)) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey.get_size(); ++i) {
    const common::ObRowkeyColumn *key = rowkey.get_column(i);
    const share::schema::ObColumnSchemaV2 *column = nullptr;
    int idx = -1;
    if (OB_ISNULL(key) || OB_ISNULL(column = schema.get_column_schema(key->column_id_))) {
      ret = OB_ERR_UNEXPECTED;
    } else if ((idx = find_idx(ctdef, key->column_id_)) < 0 || idx >= row->cnt_) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const common::ObDatum &datum = row->cells()[idx];
      if (i > 0 && OB_FAIL(sql.append(" AND ", 5))) {
      } else if (OB_FAIL(quote_ident(sql, column->get_column_name()))) {
      } else if (datum.is_null()) {
        ret = OB_ERR_UNEXPECTED;
      } else if (ob_is_integer_type(column->get_data_type())) {
        if (column->get_data_type() >= common::ObUTinyIntType && column->get_data_type() <= common::ObUInt64Type) {
          ret = sql.fmt(" = %lu", static_cast<unsigned long>(datum.get_uint()));
        } else {
          ret = sql.fmt(" = %ld", static_cast<long>(datum.get_int()));
        }
      } else {
        common::ObString text = datum.get_string();
        if (OB_FAIL(sql.append(" = ", 3))) {
        } else if (OB_FAIL(quote_literal(sql, text.ptr(), text.length()))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    copy_text(predicate, predicate_len, sql.data, sql.len);
  }
  return ret;
}

bool dep_changed(const Meta &meta,
                 const share::schema::ObTableSchema &schema,
                 const ObDMLBaseCtDef &ctdef,
                 const common::ObChunkDatumStore::StoredRow *old_row,
                 const common::ObChunkDatumStore::StoredRow *new_row)
{
  bool changed = false;
  const char *cursor = meta.dep_columns;
  while (!changed && cursor != nullptr && *cursor != '\0') {
    const char *comma = strchr(cursor, ',');
    int len = comma == nullptr ? static_cast<int>(strlen(cursor)) : static_cast<int>(comma - cursor);
    char name[NAME_LEN];
    copy_text(name, NAME_LEN, cursor, len);
    const share::schema::ObColumnSchemaV2 *column = schema.get_column_schema(name);
    int idx = column == nullptr ? -1 : find_idx(ctdef, column->get_column_id());
    if (column != nullptr && idx >= 0 && old_row != nullptr && new_row != nullptr
        && idx < old_row->cnt_ && idx < new_row->cnt_) {
      changed = !datum_same(old_row->cells()[idx], new_row->cells()[idx]);
    }
    cursor = comma == nullptr ? nullptr : comma + 1;
  }
  return changed;
}

bool pk_changed(const share::schema::ObTableSchema &schema,
                const ObDMLBaseCtDef &ctdef,
                const common::ObChunkDatumStore::StoredRow *old_row,
                const common::ObChunkDatumStore::StoredRow *new_row)
{
  bool changed = false;
  const common::ObRowkeyInfo &rowkey = schema.get_rowkey_info();
  for (int64_t i = 0; !changed && old_row != nullptr && new_row != nullptr && i < rowkey.get_size(); ++i) {
    const common::ObRowkeyColumn *key = rowkey.get_column(i);
    int idx = key == nullptr ? -1 : find_idx(ctdef, key->column_id_);
    if (idx >= 0 && idx < old_row->cnt_ && idx < new_row->cnt_) {
      changed = !datum_same(old_row->cells()[idx], new_row->cells()[idx]);
    }
  }
  return changed;
}

int delete_tasks(uint64_t table_id, const char *predicate, ObSQLSessionInfo *session)
{
  int ret = OB_SUCCESS;
  SqlBuf sql;
  int64_t affected = 0;
  if (OB_FAIL(sql.fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = %lu AND rowkey_predicate = ",
                      table_id))) {
  } else if (OB_FAIL(quote_literal(sql, predicate, static_cast<int>(strlen(predicate))))) {
  } else if (session != nullptr) {
    ret = user_write(session, sql.data, &affected);
  } else if (OB_FAIL(proxy_write(sql.data, affected))) {
  }
  return ret;
}

int build_task_sql(const Meta &meta, const char *predicate, const char *request_id, SqlBuf &sql)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.fmt("INSERT INTO oceanbase.__all_ai_column_task(table_id, column_id, rowkey_predicate, "
                      "request_id, state, retry_count, eligible_ts, gmt_create, gmt_modified) VALUES (%lu, %lu, ",
                      meta.table_id, meta.column_id))) {
  } else if (OB_FAIL(quote_literal(sql, predicate, static_cast<int>(strlen(predicate))))) {
  } else if (OB_FAIL(sql.append(", ", 2))) {
  } else if (OB_FAIL(quote_literal(sql, request_id, static_cast<int>(strlen(request_id))))) {
  } else if (OB_FAIL(sql.append(", 0, 0, now(6) - interval 2 second, now(6), now(6))", -1))) {
  }
  return ret;
}

int insert_task(ObSQLSessionInfo *session, const Meta &meta, const char *predicate, const char *request_id)
{
  int ret = OB_SUCCESS;
  SqlBuf del;
  SqlBuf sql;
  int64_t affected = 0;
  // A row may already carry a task from an earlier change in the same transaction
  // (or from before the update), so replace it instead of hitting a primary key
  // conflict on (table_id, column_id, rowkey_predicate).
  if (OB_FAIL(del.fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = %lu AND column_id = %lu "
                      "AND rowkey_predicate = ", meta.table_id, meta.column_id))) {
  } else if (OB_FAIL(quote_literal(del, predicate, static_cast<int>(strlen(predicate))))) {
  } else if (OB_FAIL(user_write(session, del.data, &affected))) {
  } else if (OB_FAIL(build_task_sql(meta, predicate, request_id, sql))) {
  } else if (OB_FAIL(user_write(session, sql.data, &affected))) {
  }
  return ret;
}

int invalidate_row(ObTableModifyOp *op,
                   ObSQLSessionInfo *session,
                   const share::schema::ObTableSchema &schema,
                   const ObDMLBaseCtDef &ctdef,
                   const common::ObChunkDatumStore::StoredRow *row,
                   const Meta *metas,
                   int meta_count,
                   const bool *selected)
{
  int ret = OB_SUCCESS;
  char predicate[PRED_LEN];
  char request_id[REQ_LEN];
  SqlBuf sql;
  int64_t affected = 0;
  int selected_count = 0;
  make_request_id(request_id, REQ_LEN);
  if (OB_FAIL(format_predicate(schema, ctdef, row, predicate, PRED_LEN))) {
  } else if (OB_FAIL(sql.append("UPDATE ", 7))) {
  } else if (OB_FAIL(quote_ident(sql, metas[0].database_name))) {
  } else if (OB_FAIL(sql.append(".", 1))) {
  } else if (OB_FAIL(quote_ident(sql, metas[0].table_name))) {
  } else if (OB_FAIL(sql.append(" SET ", 5))) {
  }
  for (int i = 0; OB_SUCC(ret) && i < meta_count; ++i) {
    if (!selected[i]) {
    } else {
      if (selected_count > 0 && OB_FAIL(sql.append(", ", 2))) {
      } else if (OB_FAIL(quote_ident(sql, metas[i].column_name))) {
      } else if (OB_FAIL(sql.append(" = NULL, ", 9))) {
      } else if (OB_FAIL(quote_ident(sql, metas[i].state_column_name))) {
      } else if (OB_FAIL(sql.append(" = 'PENDING', ", 14))) {
      } else if (OB_FAIL(quote_ident(sql, metas[i].request_column_name))) {
      } else if (OB_FAIL(sql.append(" = ", 3))) {
      } else if (OB_FAIL(quote_literal(sql, request_id, static_cast<int>(strlen(request_id))))) {
      } else {
        ++selected_count;
      }
    }
  }
  if (OB_FAIL(ret) || selected_count == 0) {
  } else if (OB_FAIL(sql.append(" WHERE ", 7))) {
  } else if (OB_FAIL(sql.append(predicate, static_cast<int>(strlen(predicate))))) {
  } else {
    bool &iter_uncommitted = op->get_exec_ctx().get_das_ctx().iter_uncommitted_row_;
    const bool saved_iter = iter_uncommitted;
    iter_uncommitted = true;
    ret = user_write(session, sql.data, &affected);
    iter_uncommitted = saved_iter;
  }
  for (int i = 0; OB_SUCC(ret) && i < meta_count; ++i) {
    if (selected[i] && OB_FAIL(insert_task(session, metas[i], predicate, request_id))) {
    }
  }
  UNUSED(affected);
  return ret;
}

struct TaskRow
{
  uint64_t table_id;
  uint64_t column_id;
  char predicate[PRED_LEN];
  char request_id[REQ_LEN];
  int64_t state;
  int64_t retry_count;
};

struct CacheItem
{
  bool used;
  bool failed;
  uint64_t column_id;
  char request_id[REQ_LEN];
  char result[RESULT_LEN];
};

static CacheItem g_cache[MAX_CACHE];

struct Scratch
{
  TaskRow tasks[32];
  Meta metas[32];
  TaskRow group_tasks[32];
  const CacheItem *cached[32];
  int members[32];
  bool used[32];
};

CacheItem *find_cache(const char *request_id, uint64_t column_id)
{
  CacheItem *found = nullptr;
  for (int i = 0; found == nullptr && i < MAX_CACHE; ++i) {
    if (g_cache[i].used && g_cache[i].column_id == column_id
        && 0 == strcmp(g_cache[i].request_id, request_id)) {
      found = &g_cache[i];
    }
  }
  return found;
}

CacheItem *add_cache(const char *request_id, uint64_t column_id, const char *result, bool failed)
{
  CacheItem *slot = find_cache(request_id, column_id);
  if (slot == nullptr) {
    for (int i = 0; slot == nullptr && i < MAX_CACHE; ++i) {
      if (!g_cache[i].used) {
        slot = &g_cache[i];
      }
    }
  }
  if (slot == nullptr) {
    // Cache is only an optimization; dropping it costs one extra model call.
    memset(g_cache, 0, sizeof(g_cache));
    slot = &g_cache[0];
  }
  if (slot != nullptr) {
    slot->used = true;
    slot->failed = failed;
    slot->column_id = column_id;
    copy_text(slot->request_id, REQ_LEN, request_id, static_cast<int>(strlen(request_id)));
    copy_text(slot->result, RESULT_LEN, result, result == nullptr ? 0 : static_cast<int>(strlen(result)));
  }
  return slot;
}

void clear_cache(const char *request_id)
{
  for (int i = 0; i < MAX_CACHE; ++i) {
    if (g_cache[i].used && 0 == strcmp(g_cache[i].request_id, request_id)) {
      g_cache[i].used = false;
    }
  }
}

int eval_input(const Meta &meta, const char *predicate, char *output, int output_len, bool &missing)
{
  int ret = OB_SUCCESS;
  SqlBuf sql;
  missing = false;
  output[0] = '\0';
  if (OB_FAIL(sql.append("SELECT ", 7))) {
  } else if (OB_FAIL(sql.append(meta.expr_sql, static_cast<int>(strlen(meta.expr_sql))))) {
  } else if (OB_FAIL(sql.append(" AS ai_col_input FROM ", -1))) {
  } else if (OB_FAIL(quote_ident(sql, meta.database_name))) {
  } else if (OB_FAIL(sql.append(".", 1))) {
  } else if (OB_FAIL(quote_ident(sql, meta.table_name))) {
  } else if (OB_FAIL(sql.append(" WHERE ", 7))) {
  } else if (OB_FAIL(sql.append(predicate, static_cast<int>(strlen(predicate))))) {
  } else {
    SMART_VAR(common::ObMySQLProxy::MySQLResult, result) {
      if (OB_FAIL(proxy_read(sql.data, result))) {
      } else if (OB_ISNULL(result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result.get_result()->next())) {
        if (OB_ITER_END == ret) {
          missing = true;
          ret = OB_SUCCESS;
        }
      } else {
        common::ObString value;
        if (OB_FAIL(result.get_result()->get_varchar("ai_col_input", value))) {
          if (OB_ERR_NULL_VALUE == ret) {
            ret = OB_SUCCESS;
            output[0] = '\0';
          }
        } else {
          copy_text(output, output_len, value.ptr(), value.length());
        }
      }
    }
  }
  return ret;
}

int check_result_shape(const Meta &meta, const char *result)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(result) || result[0] == '\0') {
    ret = OB_INVALID_DATA;
  } else if (KIND_EMBED == meta.kind) {
    if (result[0] != '[' || result[strlen(result) - 1] != ']') {
      ret = OB_INVALID_DATA;
    } else if (meta.vector_dim > 0) {
      int64_t count = 0;
      for (const char *p = result; OB_SUCC(ret) && *p != '\0'; ++p) {
        if (*p == ',') {
          ++count;
        }
      }
      if (count + 1 != meta.vector_dim) {
        ret = OB_ERR_INVALID_VECTOR_DIM;
        LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<uint32_t>(meta.vector_dim),
                       static_cast<uint32_t>(count + 1));
      }
    }
  } else if (ob_is_integer_type(static_cast<common::ObObjType>(meta.result_type))) {
    const char *p = result;
    if (*p == '-' || *p == '+') {
      ++p;
    }
    if (*p == '\0') {
      ret = OB_INVALID_DATA;
    }
    for (; OB_SUCC(ret) && *p != '\0'; ++p) {
      if (*p < '0' || *p > '9') {
        ret = OB_INVALID_DATA;
      }
    }
  } else if (common::ObJsonType == static_cast<common::ObObjType>(meta.result_type)) {
    common::ObArenaAllocator allocator;
    common::ObJsonNode *tree = nullptr;
    if (OB_FAIL(common::ObJsonParser::get_tree(&allocator,
                                               common::ObString(static_cast<int32_t>(strlen(result)), result),
                                               tree,
                                               common::ObJsonParser::JSN_RELAXED_FLAG))) {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

int call_model(const Meta &meta, const char *input, char *output, int output_len)
{
  int ret = OB_SUCCESS;
  common::ObArenaAllocator allocator;
  common::ObAIFuncExprInfo *info = nullptr;
  share::ObAiModelEndpointInfo endpoint;
  query::ObIAiEndpointResolver *resolver =
      ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
  common::ObString model_name(meta.model_name);
  common::ObString content(input);
  common::ObString prompt(meta.type_prompt);
  common::ObString result;
  output[0] = '\0';
  if (content.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(resolver)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(common::ObAIFuncUtils::get_ai_func_info(allocator, model_name, info))) {
  } else if (OB_FAIL(resolver->resolve_by_model_name(model_name, allocator, endpoint, false))) {
  } else {
    common::ObAIFuncModel model(allocator, *info, endpoint);
    if (KIND_EMBED == meta.kind) {
      if (OB_FAIL(model.call_dense_embedding(content, nullptr, result))) {
      }
    } else if (OB_FAIL(model.call_completion_with_system(prompt, content, nullptr, result))) {
    }
  }
  if (OB_SUCC(ret)) {
    if (KIND_EMBED == meta.kind && result.length() > 0 && result.ptr()[0] == '[') {
      SqlBuf formatted;
      const char *text = result.ptr();
      int len = result.length();
      int i = 0;
      bool first = true;
      if (OB_FAIL(formatted.append("[", 1))) {
      }
      while (OB_SUCC(ret) && i < len) {
        while (i < len && (text[i] == '[' || text[i] == ',' || text[i] == ' ' || text[i] == ']')) {
          ++i;
        }
        if (i >= len) {
          break;
        }
        char *end = nullptr;
        double value = strtod(text + i, &end);
        if (end == text + i) {
          break;
        }
        i = static_cast<int>(end - text);
        if (!first && OB_FAIL(formatted.append(",", 1))) {
        } else {
          first = false;
          double rounded = nearbyint(value);
          if (fabs(value - rounded) < 1e-6) {
            ret = formatted.fmt("%ld", static_cast<long>(rounded));
          } else {
            ret = formatted.fmt("%.6g", value);
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(formatted.append("]", 1))) {
      } else if (OB_SUCC(ret)) {
        copy_text(output, output_len, formatted.data, formatted.len);
      }
    } else {
      copy_text(output, output_len, result.ptr(), result.length());
    }
    if (OB_SUCC(ret) && OB_FAIL(check_result_shape(meta, output))) {
      LOG_WARN("ai column result does not match column type", K(ret), K(meta.column_name),
               K(meta.result_type), K(output));
    }
  }
  return ret;
}

int recompute_ready(const Meta &meta)
{
  int ret = OB_SUCCESS;
  SqlBuf sql;
  bool pending = false;
  if (OB_FAIL(sql.append("SELECT 1 FROM ", -1))) {
  } else if (OB_FAIL(quote_ident(sql, meta.database_name))) {
  } else if (OB_FAIL(sql.append(".", 1))) {
  } else if (OB_FAIL(quote_ident(sql, meta.table_name))) {
  } else if (OB_FAIL(sql.append(" WHERE ", 7))) {
  } else if (OB_FAIL(quote_ident(sql, meta.state_column_name))) {
  } else if (OB_FAIL(sql.append(" IS NULL OR ", 12))) {
  } else if (OB_FAIL(quote_ident(sql, meta.state_column_name))) {
  } else if (OB_FAIL(sql.append(" <> 'READY' LIMIT 1", -1))) {
  } else {
    SMART_VAR(common::ObMySQLProxy::MySQLResult, result) {
      if (OB_FAIL(proxy_read(sql.data, result))) {
      } else if (OB_ISNULL(result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(result.get_result()->next())) {
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      } else {
        pending = true;
      }
    }
  }
  if (OB_SUCC(ret)) {
    LockGuard lock(g_lock);
    Meta *slot = find_meta_locked(meta.table_id, meta.column_id);
    if (slot != nullptr) {
      slot->not_ready = pending;
    }
  }
  return ret;
}

int publish_group(const Meta *metas, const TaskRow *tasks, int count, const CacheItem **cached)
{
  int ret = OB_SUCCESS;
  SqlBuf sql;
  int64_t affected = 0;
  if (count <= 0) {
  } else if (OB_FAIL(sql.append("UPDATE ", 7))) {
  } else if (OB_FAIL(quote_ident(sql, metas[0].database_name))) {
  } else if (OB_FAIL(sql.append(".", 1))) {
  } else if (OB_FAIL(quote_ident(sql, metas[0].table_name))) {
  } else if (OB_FAIL(sql.append(" SET ", 5))) {
  }
  for (int i = 0; OB_SUCC(ret) && i < count; ++i) {
    if (i > 0 && OB_FAIL(sql.append(", ", 2))) {
    } else if (cached[i]->failed) {
      if (OB_FAIL(quote_ident(sql, metas[i].column_name))) {
      } else if (OB_FAIL(sql.append(" = NULL, ", 9))) {
      } else if (OB_FAIL(quote_ident(sql, metas[i].state_column_name))) {
      } else if (OB_FAIL(sql.append(" = 'FAILED'", 11))) {
      }
    } else if (OB_FAIL(quote_ident(sql, metas[i].column_name))) {
    } else if (OB_FAIL(sql.append(" = ", 3))) {
    } else if (KIND_EMBED == metas[i].kind) {
      ret = sql.append(cached[i]->result, static_cast<int>(strlen(cached[i]->result)));
    } else if (ob_is_integer_type(static_cast<common::ObObjType>(metas[i].result_type))
               && cached[i]->result[0] != '\0') {
      bool numeric = true;
      const char *p = cached[i]->result;
      if (*p == '-' || *p == '+') {
        ++p;
      }
      if (*p == '\0') {
        numeric = false;
      }
      for (; numeric && *p != '\0'; ++p) {
        numeric = *p >= '0' && *p <= '9';
      }
      if (numeric) {
        ret = sql.append(cached[i]->result, static_cast<int>(strlen(cached[i]->result)));
      } else {
        ret = quote_literal(sql, cached[i]->result, static_cast<int>(strlen(cached[i]->result)));
      }
    } else if (OB_FAIL(quote_literal(sql, cached[i]->result, static_cast<int>(strlen(cached[i]->result))))) {
    } else if (OB_FAIL(sql.append(", ", 2))) {
    } else if (OB_FAIL(quote_ident(sql, metas[i].state_column_name))) {
    } else if (OB_FAIL(sql.append(" = 'READY'", 10))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append(" WHERE ", 7))) {
  } else if (OB_FAIL(sql.append(tasks[0].predicate, static_cast<int>(strlen(tasks[0].predicate))))) {
  }
  for (int i = 0; OB_SUCC(ret) && i < count; ++i) {
    if (OB_FAIL(sql.append(" AND ", 5))) {
    } else if (OB_FAIL(quote_ident(sql, metas[i].request_column_name))) {
    } else if (OB_FAIL(sql.append(" = ", 3))) {
    } else if (OB_FAIL(quote_literal(sql, tasks[i].request_id, static_cast<int>(strlen(tasks[i].request_id))))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(proxy_write(sql.data, affected))) {
  } else {
    SqlBuf del;
    if (OB_FAIL(del.fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE request_id = "))) {
    } else if (OB_FAIL(quote_literal(del, tasks[0].request_id, static_cast<int>(strlen(tasks[0].request_id))))) {
    } else if (OB_FAIL(proxy_write(del.data, affected))) {
    }
    for (int i = 0; OB_SUCC(ret) && i < count; ++i) {
      ret = recompute_ready(metas[i]);
    }
    clear_cache(tasks[0].request_id);
  }
  return ret;
}

int mark_retry(const TaskRow &task, int64_t retry_count)
{
  int ret = OB_SUCCESS;
  SqlBuf sql;
  int64_t affected = 0;
  if (OB_FAIL(sql.fmt("UPDATE oceanbase.__all_ai_column_task SET state = 3, retry_count = %ld "
                      "WHERE table_id = %lu AND column_id = %lu AND rowkey_predicate = ",
                      retry_count, task.table_id, task.column_id))) {
  } else if (OB_FAIL(quote_literal(sql, task.predicate, static_cast<int>(strlen(task.predicate))))) {
  } else if (OB_FAIL(proxy_write(sql.data, affected))) {
  }
  return ret;
}

int process_task_rows(Scratch *scratch, TaskRow *tasks, int task_count)
{
  int ret = OB_SUCCESS;
  bool *used = scratch->used;
  memset(used, 0, sizeof(scratch->used));
  bool slept = false;
  for (int begin = 0; OB_SUCC(ret) && begin < task_count; ++begin) {
    if (used[begin]) {
      continue;
    }
    int *members = scratch->members;
    int member_count = 0;
    for (int i = begin; i < task_count; ++i) {
      if (!used[i] && 0 == strcmp(tasks[i].request_id, tasks[begin].request_id)) {
        used[i] = true;
        members[member_count++] = i;
      }
    }
    Meta *metas = scratch->metas;
    TaskRow *group_tasks = scratch->group_tasks;
    const CacheItem **cached = scratch->cached;
    memset(scratch->cached, 0, sizeof(scratch->cached));
    bool ready = true;
    bool retrying = false;
    int live_count = 0;
    for (int m = 0; OB_SUCC(ret) && m < member_count; ++m) {
      const TaskRow &task = tasks[members[m]];
      Meta meta;
      memset(&meta, 0, sizeof(meta));
      {
        LockGuard lock(g_lock);
        Meta *slot = find_meta_locked(task.table_id, task.column_id);
        if (slot != nullptr) {
          meta = *slot;
        }
      }
      if (meta.column_id == 0) {
        int64_t affected = 0;
        SqlBuf del;
        if (OB_FAIL(del.fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = %lu AND column_id = %lu AND rowkey_predicate = ",
                            task.table_id, task.column_id))) {
          LOG_WARN("build orphan task delete failed", K(ret));
        } else if (OB_FAIL(quote_literal(del, task.predicate, static_cast<int>(strlen(task.predicate))))) {
        } else if (OB_FAIL(proxy_write(del.data, affected))) {
        }
      } else {
        CacheItem *item = find_cache(task.request_id, task.column_id);
        if (item == nullptr) {
          char input[RESULT_LEN];
          char output[RESULT_LEN];
          bool missing = false;
          if (OB_FAIL(eval_input(meta, task.predicate, input, RESULT_LEN, missing))) {
          } else if (missing) {
            int64_t affected = 0;
            SqlBuf del;
            if (OB_FAIL(del.fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE table_id = %lu AND column_id = %lu AND rowkey_predicate = ",
                                task.table_id, task.column_id))) {
            } else if (OB_FAIL(quote_literal(del, task.predicate, static_cast<int>(strlen(task.predicate))))) {
            } else if (OB_FAIL(proxy_write(del.data, affected))) {
            }
            ready = false;
          } else if (OB_FAIL(call_model(meta, input, output, RESULT_LEN))) {
            if (task.retry_count < meta.max_retries) {
              if (OB_FAIL(mark_retry(task, task.retry_count + 1))) {
              } else {
                retrying = true;
                ready = false;
                ret = OB_SUCCESS;
              }
            } else if (OB_ISNULL(item = add_cache(task.request_id, task.column_id, "", true))) {
              ret = OB_SIZE_OVERFLOW;
            } else {
              ret = OB_SUCCESS;
            }
          } else if (OB_ISNULL(item = add_cache(task.request_id, task.column_id, output, false))) {
            ret = OB_SIZE_OVERFLOW;
          }
        }
        if (OB_SUCC(ret) && item != nullptr) {
          metas[live_count] = meta;
          group_tasks[live_count] = task;
          cached[live_count] = item;
          ++live_count;
        } else {
          ready = false;
        }
      }
    }
    if (OB_SUCC(ret) && retrying && !slept) {
      ob_usleep(1000 * 1000);
      slept = true;
    }
    if (OB_SUCC(ret) && ready && !retrying && live_count > 0) {
      ret = publish_group(metas, group_tasks, live_count, cached);
    }
  }
  return ret;
}

int process_tasks()
{
  int ret = OB_SUCCESS;
  // The worker thread stack is small, so the per-round scratch space lives on the heap.
  Scratch *scratch = static_cast<Scratch *>(ob_malloc(sizeof(Scratch), 
      common::ObMemAttr(common::ObModIds::OB_SQL_EXPR_CALC)));
  int task_count = 0;
  TaskRow *tasks = OB_ISNULL(scratch) ? nullptr : scratch->tasks;
  SqlBuf sql;
  if (OB_ISNULL(scratch) || OB_ISNULL(tasks)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(sql.append("SELECT table_id, column_id, rowkey_predicate, request_id, state, retry_count "
                         "FROM oceanbase.__all_ai_column_task WHERE eligible_ts <= now(6) LIMIT 32", -1))) {
  } else {
    SMART_VAR(common::ObMySQLProxy::MySQLResult, result) {
      common::sqlclient::ObMySQLResult *rows = nullptr;
      if (OB_FAIL(proxy_read(sql.data, result))) {
      } else if (OB_ISNULL(rows = result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret) && task_count < 32 && OB_SUCC(rows->next())) {
        TaskRow &task = tasks[task_count];
        common::ObString predicate;
        common::ObString request_id;
        int64_t table_id = 0;
        int64_t column_id = 0;
        memset(&task, 0, sizeof(task));
        if (OB_FAIL(rows->get_int("table_id", table_id))) {
        } else if (OB_FAIL(rows->get_int("column_id", column_id))) {
        } else if (OB_FAIL(rows->get_varchar("rowkey_predicate", predicate))) {
        } else if (OB_FAIL(rows->get_varchar("request_id", request_id))) {
        } else if (OB_FAIL(rows->get_int("state", task.state))) {
        } else if (OB_FAIL(rows->get_int("retry_count", task.retry_count))) {
        } else {
          task.table_id = static_cast<uint64_t>(table_id);
          task.column_id = static_cast<uint64_t>(column_id);
          copy_text(task.predicate, PRED_LEN, predicate.ptr(), predicate.length());
          copy_text(task.request_id, REQ_LEN, request_id.ptr(), request_id.length());
          ++task_count;
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
  }
  if (OB_SUCC(ret)) {
    ret = process_task_rows(scratch, tasks, task_count);
  }
  if (OB_NOT_NULL(scratch)) {
    ob_free(scratch);
    scratch = nullptr;
  }
  return ret;
}

class AIColumnWorker : public lib::Threads
{
public:
  void run1() override
  {
    while (!has_set_stop()) {
      int ret = process_tasks();
      if (OB_FAIL(ret)) {
        LOG_WARN("ai column worker round failed", K(ret));
      }
      ob_usleep(50 * 1000);
    }
  }
};

static AIColumnWorker g_worker;
static bool g_worker_started = false;

void ensure_worker()
{
  LockGuard lock(g_lock);
  if (!g_worker_started) {
    g_worker.set_thread_count(1);
    if (OB_SUCCESS == g_worker.start()) {
      g_worker_started = true;
    }
  }
}

void bootstrap_recovery()
{
  static bool recovered = false;
  if (!recovered) {
    int ret = OB_SUCCESS;
    if (OB_FAIL(load_persisted_metadata())) {
      LOG_WARN("reload ai column metadata failed", K(ret));
    } else {
      recovered = true;
      ensure_worker();
    }
  }
}

} // namespace

bool ob_table_has_ai_column(const uint64_t table_id)
{
  if (OB_INVALID_ID == table_id || 0 == table_id) {
    return false;
  }
  bool found = false;
  bool empty = false;
  {
    LockGuard lock(g_lock);
    for (int i = 0; !found && i < g_meta_count; ++i) {
      found = g_metas[i].table_id == table_id;
    }
    empty = (g_meta_count == 0);
  }
  if (!found && empty) {
    // After a restart the registry is cold but the metadata and any in-flight
    // tasks are still in the system tables, so reload before deciding.
    bootstrap_recovery();
    LockGuard lock(g_lock);
    for (int i = 0; !found && i < g_meta_count; ++i) {
      found = g_metas[i].table_id == table_id;
    }
  }
  return found;
}

void ObAIColumnService::clear_staged()
{
  g_staged_count = 0;
  memset(g_staged, 0, sizeof(g_staged));
}

int ObAIColumnService::reject_staged()
{
  int ret = OB_SUCCESS;
  if (g_staged_count > 0) {
    clear_staged();
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "modifying an AI column in place");
  }
  return ret;
}

int ObAIColumnService::stage_column(const share::schema::ObColumnSchemaV2 &column, ParseNode *attr)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(attr) || attr->num_child_ < 2 || OB_ISNULL(attr->children_[0]) || OB_ISNULL(attr->children_[1])) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else if (g_staged_count >= MAX_STAGED) {
    ret = OB_SIZE_OVERFLOW;
  } else {
    ParseNode *call = attr->children_[0];
    ParseNode *options = attr->children_[1];
    const char *args[4] = {nullptr, nullptr, nullptr, nullptr};
    int arg_lens[4] = {0, 0, 0, 0};
    int arg_count = 0;
    Staged staged;
    memset(&staged, 0, sizeof(staged));
    copy_ob(staged.column_name, NAME_LEN, column.get_column_name_str());
    staged.max_retries = 0;
    staged.kind = KIND_COMPLETE;
    staged.result_type = column.get_data_type();
    if (column.is_json()) {
      copy_text(staged.type_prompt, PROMPT_LEN, JSON_PROMPT, static_cast<int>(strlen(JSON_PROMPT)));
    } else if (ob_is_integer_type(column.get_data_type())) {
      copy_text(staged.type_prompt, PROMPT_LEN, INT_PROMPT, static_cast<int>(strlen(INT_PROMPT)));
    }
    if (OB_ISNULL(call->str_value_)) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else if (OB_FAIL(split_args(call->str_value_, static_cast<int>(call->str_len_), args, arg_lens, 4, arg_count))) {
    } else if (arg_count < 2) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else {
      const char *full = call->str_value_;
      int name_len = 0;
      while (name_len < call->str_len_ && (isalnum(static_cast<unsigned char>(full[name_len])) || full[name_len] == '_')) {
        ++name_len;
      }
      if (text_eq_raw("AI_EMBED", full, name_len)) {
        staged.kind = KIND_EMBED;
        staged.type_prompt[0] = '\0';
      } else if (!text_eq_raw("AI_COMPLETE", full, name_len)) {
        ret = OB_ERR_PARSER_SYNTAX;
      }
      if (OB_FAIL(ret)) {
      } else if (arg_lens[0] >= 2 && args[0][0] == '\'' && args[0][arg_lens[0] - 1] == '\'') {
        copy_text(staged.model_name, NAME_LEN, args[0] + 1, arg_lens[0] - 2);
      } else {
        ret = OB_ERR_PARSER_SYNTAX;
      }
      if (OB_SUCC(ret)) {
        copy_text(staged.expr_sql, EXPR_LEN, args[1], arg_lens[1]);
      }
      if (OB_SUCC(ret) && arg_count >= 3 && staged.kind == KIND_EMBED) {
        int64_t got = strtoll(args[2], nullptr, 10);
        int64_t expected = vector_dim(column);
        if (expected > 0 && got != expected) {
          ret = OB_ERR_INVALID_VECTOR_DIM;
          LOG_USER_ERROR(OB_ERR_INVALID_VECTOR_DIM, static_cast<uint32_t>(expected), static_cast<uint32_t>(got));
        } else {
          staged.vector_dim = got;
        }
      }
      if (OB_SUCC(ret) && staged.kind == KIND_EMBED && staged.vector_dim == 0) {
        staged.vector_dim = vector_dim(column);
      }
    }
    if (OB_SUCC(ret)) {
      bool chained = false;
      ParseNode *input = nullptr;
      if (T_FUN_SYS == call->type_ && call->num_child_ >= 2 && call->children_[1] != nullptr
          && call->children_[1]->num_child_ >= 2) {
        input = call->children_[1]->children_[1];
      }
      LockGuard lock(g_lock);
      collect_deps(input, staged.dep_columns, DEP_LEN, chained, ret);
      if (chained) {
        ret = OB_NOT_SUPPORTED;
      }
    }
    for (int i = 0; OB_SUCC(ret) && options != nullptr && i + 1 < options->num_child_; i += 2) {
      ParseNode *name = options->children_[i];
      ParseNode *value = options->children_[i + 1];
      if (OB_ISNULL(name) || OB_ISNULL(value) || OB_ISNULL(name->str_value_)) {
        ret = OB_ERR_PARSER_SYNTAX;
      } else if (text_eq_raw("sync_mode", name->str_value_, static_cast<int>(name->str_len_))) {
        if (OB_ISNULL(value->str_value_)
            || !text_eq_raw("async", value->str_value_, static_cast<int>(value->str_len_))) {
          ret = OB_NOT_SUPPORTED;
        }
      } else if (text_eq_raw("max_retries", name->str_value_, static_cast<int>(name->str_len_))) {
        staged.max_retries = value->value_;
      }
    }
    if (OB_SUCC(ret)) {
      g_staged[g_staged_count] = staged;
      ++g_staged_count;
    }
  }
  return ret;
}

int materialize_staged(share::schema::ObTableSchema *schema,
                       ObAlterTableStmt *stmt,
                       const common::ObString &database_name,
                       const common::ObString &table_name,
                       uint64_t table_id,
                       uint64_t &next_column_id)
{
  int ret = OB_SUCCESS;
  for (int i = 0; OB_SUCC(ret) && i < g_staged_count; ++i) {
    Meta meta;
    memset(&meta, 0, sizeof(meta));
    const Staged &staged = g_staged[i];
    copy_ob(meta.database_name, NAME_LEN, database_name);
    copy_ob(meta.table_name, NAME_LEN, table_name);
    copy_text(meta.column_name, NAME_LEN, staged.column_name, static_cast<int>(strlen(staged.column_name)));
    copy_text(meta.model_name, NAME_LEN, staged.model_name, static_cast<int>(strlen(staged.model_name)));
    copy_text(meta.expr_sql, EXPR_LEN, staged.expr_sql, static_cast<int>(strlen(staged.expr_sql)));
    copy_text(meta.dep_columns, DEP_LEN, staged.dep_columns, static_cast<int>(strlen(staged.dep_columns)));
    copy_text(meta.type_prompt, PROMPT_LEN, staged.type_prompt, static_cast<int>(strlen(staged.type_prompt)));
    if (staged.type_prompt[0] != '\0') {
      snprintf(meta.normalized_expr, EXPR_LEN, "%s %s", staged.expr_sql, staged.type_prompt);
    } else {
      copy_text(meta.normalized_expr, EXPR_LEN, staged.expr_sql, static_cast<int>(strlen(staged.expr_sql)));
    }
    snprintf(meta.state_column_name, NAME_LEN, "__ai_st_%s", staged.column_name);
    snprintf(meta.request_column_name, NAME_LEN, "__ai_rq_%s", staged.column_name);
    snprintf(meta.spec_column_name, NAME_LEN, "__ai_sp_%s", staged.column_name);
    meta.max_retries = staged.max_retries;
    meta.kind = staged.kind;
    meta.result_type = staged.result_type;
    meta.vector_dim = staged.vector_dim;
    meta.table_id = table_id;
    if (OB_FAIL(fill_hidden(meta, schema, stmt, next_column_id))) {
    } else {
      LockGuard lock(g_lock);
      if (OB_ISNULL(upsert_meta_locked(meta))) {
        ret = OB_SIZE_OVERFLOW;
      }
    }
  }
  g_staged_count = 0;
  return ret;
}

int ObAIColumnService::append_hidden_columns(share::schema::ObTableSchema &schema,
                                             const common::ObString &database_name,
                                             const common::ObString &table_name,
                                             uint64_t &next_column_id)
{
  return materialize_staged(&schema, nullptr, database_name, table_name, 0, next_column_id);
}

int ObAIColumnService::append_hidden_alter(ObAlterTableStmt &stmt,
                                           const common::ObString &database_name,
                                           const common::ObString &table_name,
                                           uint64_t table_id)
{
  uint64_t ignored = 0;
  return materialize_staged(nullptr, &stmt, database_name, table_name, table_id, ignored);
}

int ObAIColumnService::persist_table(ObSQLSessionInfo *session, uint64_t table_id)
{
  int ret = OB_SUCCESS;
  bool pending = false;
  UNUSED(session);
  {
    LockGuard lock(g_lock);
    for (int i = 0; !pending && i < g_meta_count; ++i) {
      pending = !g_metas[i].persisted;
    }
  }
  if (!pending) {
    return OB_SUCCESS;
  }
  for (int attempt = 0; attempt < 8; ++attempt) {
    ret = bind_published_ids(table_id);
    if (OB_SUCC(ret)) {
      break;
    }
    ob_usleep(100 * 1000);
  }
  if (OB_SUCC(ret)) {
    Meta to_persist[MAX_META];
    int pending_count = 0;
    {
      LockGuard lock(g_lock);
      for (int i = 0; i < g_meta_count && pending_count < MAX_META; ++i) {
        if (g_metas[i].table_id == table_id && !g_metas[i].persisted && g_metas[i].column_id != 0) {
          to_persist[pending_count++] = g_metas[i];
        }
      }
    }
    for (int i = 0; OB_SUCC(ret) && i < pending_count; ++i) {
      if (OB_FAIL(insert_metadata(to_persist[i]))) {
      } else {
        LockGuard lock(g_lock);
        Meta *slot = find_meta_locked(to_persist[i].table_id, to_persist[i].column_id);
        if (slot != nullptr) {
          slot->persisted = true;
        }
      }
    }
    if (pending_count > 0) {
      ensure_worker();
    }
  }
  return ret;
}

int ObAIColumnService::on_after_dml(ObTableModifyOp *op, ObDMLModifyRowsList *rows)
{
  int ret = OB_SUCCESS;
  if (g_internal_depth > 0 || OB_ISNULL(op) || OB_ISNULL(rows) || rows->size() <= 0) {
  } else if (FALSE_IT(bootstrap_recovery())) {
  } else {
    ++g_internal_depth;
    ObSQLSessionInfo *session = op->get_exec_ctx().get_my_session();
    for (ObDMLModifyRowsList::iterator it = rows->begin(); OB_SUCC(ret) && it != rows->end(); ++it) {
      ObDMLModifyRowNode &node = *it;
      if (OB_ISNULL(node.dml_ctdef_) || !node.dml_ctdef_->is_primary_index_) {
        continue;
      }
      uint64_t table_id = node.dml_ctdef_->das_base_ctdef_.table_id_;
      Meta metas[8];
      int meta_count = 0;
      {
        LockGuard lock(g_lock);
        for (int i = 0; i < g_meta_count && meta_count < 8; ++i) {
          if (g_metas[i].table_id == table_id) {
            metas[meta_count++] = g_metas[i];
          }
        }
      }
      if (meta_count == 0) {
        continue;
      }
      share::schema::ObSchemaGetterGuard guard;
      const share::schema::ObTableSchema *schema = nullptr;
      if (OB_FAIL(load_schema(table_id, guard, schema))) {
      } else if (ObDmlEventType::DE_DELETING == node.dml_event_) {
        char predicate[PRED_LEN];
        if (OB_FAIL(format_predicate(*schema, *node.dml_ctdef_, node.old_row_, predicate, PRED_LEN))) {
        } else if (OB_FAIL(delete_tasks(table_id, predicate, session))) {
        }
      } else {
        const bool is_insert = ObDmlEventType::DE_INSERTING == node.dml_event_;
        bool changed_pk = !is_insert && pk_changed(*schema, *node.dml_ctdef_, node.old_row_, node.new_row_);
        bool selected[8] = {false};
        bool any = false;
        if (changed_pk && node.old_row_ != nullptr) {
          char old_predicate[PRED_LEN];
          if (OB_FAIL(format_predicate(*schema, *node.dml_ctdef_, node.old_row_, old_predicate, PRED_LEN))) {
          } else if (OB_FAIL(delete_tasks(table_id, old_predicate, session))) {
          }
        }
        for (int i = 0; OB_SUCC(ret) && i < meta_count; ++i) {
          selected[i] = is_insert || changed_pk
                        || dep_changed(metas[i], *schema, *node.dml_ctdef_, node.old_row_, node.new_row_);
          any = any || selected[i];
        }
        if (OB_SUCC(ret) && any && node.new_row_ != nullptr) {
          ret = invalidate_row(op, session, *schema, *node.dml_ctdef_, node.new_row_, metas, meta_count, selected);
        }
      }
    }
    --g_internal_depth;
    ensure_worker();
  }
  return ret;
}

int rewrite_status(const char *src, int src_len, const char *state_column, char *dst, int dst_len)
{
  int ret = OB_SUCCESS;
  int written = 0;
  int i = 0;
  while (OB_SUCC(ret) && i < src_len) {
    char c = src[i];
    if (c == '\'' || c == '"' || c == '`') {
      int start = i++;
      while (i < src_len) {
        if (src[i] == c) {
          if (i + 1 < src_len && src[i + 1] == c) {
            i += 2;
          } else {
            ++i;
            break;
          }
        } else {
          ++i;
        }
      }
      if (written + (i - start) >= dst_len) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        memcpy(dst + written, src + start, i - start);
        written += i - start;
      }
    } else if (isalpha(static_cast<unsigned char>(c)) || c == '_') {
      int start = i++;
      while (i < src_len && (isalnum(static_cast<unsigned char>(src[i])) || src[i] == '_')) {
        ++i;
      }
      bool replace = (i - start) == 6 && 0 == strncasecmp(src + start, "status", 6);
      if (replace && start > 0 && (src[start - 1] == '.' || src[start - 1] == '`')) {
        replace = false;
      }
      if (replace) {
        int name_len = static_cast<int>(strlen(state_column));
        if (written + name_len + 2 >= dst_len) {
          ret = OB_SIZE_OVERFLOW;
        } else {
          dst[written++] = '`';
          memcpy(dst + written, state_column, name_len);
          written += name_len;
          dst[written++] = '`';
        }
      } else if (written + (i - start) >= dst_len) {
        ret = OB_SIZE_OVERFLOW;
      } else {
        memcpy(dst + written, src + start, i - start);
        written += i - start;
      }
    } else if (written + 1 >= dst_len) {
      ret = OB_SIZE_OVERFLOW;
    } else {
      dst[written++] = src[i++];
    }
  }
  if (OB_SUCC(ret)) {
    dst[written] = '\0';
  }
  return ret;
}

int ObAIColumnService::prepare_retry(common::ObIAllocator &allocator,
                                     uint64_t table_id,
                                     ParseNode *column_node,
                                     ParseNode *where_node,
                                     common::ObString &column_name,
                                     common::ObString &where_sql)
{
  int ret = OB_SUCCESS;
  char name[NAME_LEN];
  char rewritten[SQL_LEN];
  Meta meta;
  memset(&meta, 0, sizeof(meta));
  if (OB_ISNULL(column_node) || OB_ISNULL(column_node->str_value_)) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else {
    copy_text(name, NAME_LEN, column_node->str_value_, static_cast<int>(column_node->str_len_));
    LockGuard lock(g_lock);
    Meta *slot = find_meta_name_locked(table_id, name);
    if (slot != nullptr) {
      meta = *slot;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (meta.column_id == 0) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
  } else if (where_node != nullptr && where_node->str_value_ != nullptr
             && OB_FAIL(rewrite_status(where_node->str_value_, static_cast<int>(where_node->str_len_),
                                       meta.state_column_name, rewritten, SQL_LEN))) {
  } else if (OB_FAIL(ob_write_string(allocator, common::ObString(name), column_name))) {
  } else if (where_node == nullptr) {
    where_sql.reset();
  } else if (OB_FAIL(ob_write_string(allocator, common::ObString(rewritten), where_sql))) {
  }
  return ret;
}

int ObAIColumnService::retry(ObSQLSessionInfo *session,
                             uint64_t table_id,
                             const common::ObString &column_name,
                             const common::ObString &where_sql)
{
  int ret = OB_SUCCESS;
  ++g_internal_depth;
  Meta meta;
  memset(&meta, 0, sizeof(meta));
  {
    char name[NAME_LEN];
    copy_ob(name, NAME_LEN, column_name);
    LockGuard lock(g_lock);
    Meta *slot = find_meta_name_locked(table_id, name);
    if (slot != nullptr) {
      meta = *slot;
    }
  }
  share::schema::ObSchemaGetterGuard guard;
  const share::schema::ObTableSchema *schema = nullptr;
  if (meta.column_id == 0) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
  } else if (OB_FAIL(load_schema(table_id, guard, schema))) {
  } else {
    SqlBuf query;
    if (OB_FAIL(query.append("SELECT ", 7))) {
    }
    const common::ObRowkeyInfo &rowkey = schema->get_rowkey_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey.get_size(); ++i) {
      const common::ObRowkeyColumn *key = rowkey.get_column(i);
      const share::schema::ObColumnSchemaV2 *column = key == nullptr ? nullptr : schema->get_column_schema(key->column_id_);
      if (OB_ISNULL(column)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (i > 0 && OB_FAIL(query.append(", ", 2))) {
      } else if (OB_FAIL(quote_ident(query, column->get_column_name()))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(query.append(" FROM ", 6))) {
    } else if (OB_FAIL(quote_ident(query, meta.database_name))) {
    } else if (OB_FAIL(query.append(".", 1))) {
    } else if (OB_FAIL(quote_ident(query, meta.table_name))) {
    } else if (!where_sql.empty() && OB_FAIL(query.append(" WHERE ", 7))) {
    } else if (!where_sql.empty() && OB_FAIL(query.append(where_sql.ptr(), where_sql.length()))) {
    } else {
      char (*predicates)[PRED_LEN] = 
          static_cast<char (*)[PRED_LEN]>(ob_malloc(MAX_RETRY_ROWS * PRED_LEN,
                                                   common::ObMemAttr(common::ObModIds::OB_SQL_EXPR_CALC)));
      int64_t row_count = 0;
      common::sqlclient::ObISQLConnectionGuard conn_guard;
      common::sqlclient::ObISQLConnection *base = nullptr;
      ObIInnerSQLConnection *conn = nullptr;
      ObSQLSessionInfo::StmtSavedValue saved_session;
      ObIInnerSQLConnection::SavedValue saved_conn;
      bool nested = false;
      if (OB_ISNULL(predicates)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(query::ObInnerSQLConnectionAccess::create_spi_connection_with_external_session(session, conn_guard))) {
      } else if (OB_ISNULL(base = conn_guard.get_ptr()) || OB_ISNULL(conn = as_inner_sql_connection(base))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(conn->begin_nested_session(saved_session, saved_conn, true))) {
      } else {
        nested = true;
        {
          SMART_VAR(common::ObMySQLProxy::MySQLResult, result) {
            common::sqlclient::ObMySQLResult *rows = nullptr;
            if (OB_FAIL(conn->execute_read(common::ObString(query.data), result, false))) {
            } else if (OB_ISNULL(rows = result.get_result())) {
              ret = OB_ERR_UNEXPECTED;
            }
            while (OB_SUCC(ret) && row_count < MAX_RETRY_ROWS && OB_SUCC(rows->next())) {
              char *predicate = predicates[row_count];
              int pos = 0;
              predicate[0] = '\0';
              for (int64_t i = 0; OB_SUCC(ret) && i < rowkey.get_size(); ++i) {
                const common::ObRowkeyColumn *key = rowkey.get_column(i);
                const share::schema::ObColumnSchemaV2 *column = schema->get_column_schema(key->column_id_);
                int64_t value = 0;
                if (OB_ISNULL(column)) {
                  ret = OB_ERR_UNEXPECTED;
                } else if (OB_FAIL(rows->get_int(column->get_column_name(), value))) {
                } else {
                  int wrote = snprintf(predicate + pos, PRED_LEN - pos, "%s`%s` = %ld",
                                       i == 0 ? "" : " AND ", column->get_column_name(), static_cast<long>(value));
                  if (wrote < 0 || pos + wrote >= PRED_LEN) {
                    ret = OB_SIZE_OVERFLOW;
                  } else {
                    pos += wrote;
                  }
                }
              }
              if (OB_SUCC(ret)) {
                ++row_count;
              }
            }
            if (OB_ITER_END == ret) {
              ret = OB_SUCCESS;
            }
          }
        }
        for (int64_t r = 0; OB_SUCC(ret) && r < row_count; ++r) {
          char request_id[REQ_LEN];
          SqlBuf update;
          int64_t affected = 0;
          make_request_id(request_id, REQ_LEN);
          if (OB_FAIL(update.append("UPDATE ", 7))) {
          } else if (OB_FAIL(quote_ident(update, meta.database_name))) {
          } else if (OB_FAIL(update.append(".", 1))) {
          } else if (OB_FAIL(quote_ident(update, meta.table_name))) {
          } else if (OB_FAIL(update.append(" SET ", 5))) {
          } else if (OB_FAIL(quote_ident(update, meta.column_name))) {
          } else if (OB_FAIL(update.append(" = NULL, ", 9))) {
          } else if (OB_FAIL(quote_ident(update, meta.state_column_name))) {
          } else if (OB_FAIL(update.append(" = 'PENDING', ", 14))) {
          } else if (OB_FAIL(quote_ident(update, meta.request_column_name))) {
          } else if (OB_FAIL(update.append(" = ", 3))) {
          } else if (OB_FAIL(quote_literal(update, request_id, static_cast<int>(strlen(request_id))))) {
          } else if (OB_FAIL(update.append(" WHERE ", 7))) {
          } else if (OB_FAIL(update.append(predicates[r], static_cast<int>(strlen(predicates[r]))))) {
          } else if (OB_FAIL(conn->execute_write(common::ObString(update.data), affected, false))) {
          } else if (affected > 0 && OB_FAIL(insert_task(session, meta, predicates[r], request_id))) {
          }
        }
      }
      if (nested) {
        int tmp = conn->end_nested_session(saved_session, saved_conn);
        if (OB_SUCC(ret)) {
          ret = tmp;
        }
      }
      if (OB_NOT_NULL(predicates)) {
        ob_free(predicates);
        predicates = nullptr;
      }
    }
  }
  --g_internal_depth;
  if (OB_SUCC(ret)) {
    ensure_worker();
  }
  return ret;
}

int ObAIColumnService::strict_enabled(ObSQLSessionInfo *session, bool &enabled)
{
  int ret = OB_SUCCESS;
  enabled = false;
  if (g_internal_depth > 0) {
    enabled = false;
  } else if (OB_NOT_NULL(session)
      && OB_FAIL(session->get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, enabled))) {
    enabled = false;
    ret = OB_SUCCESS;
  } else if (enabled) {
    bootstrap_recovery();
  }
  return ret;
}

int ObAIColumnService::check_strict_column(ObSQLSessionInfo *session,
                                           uint64_t table_id,
                                           uint64_t column_id)
{
  int ret = OB_SUCCESS;
  Meta meta;
  memset(&meta, 0, sizeof(meta));
  bool matched = false;
  {
    LockGuard lock(g_lock);
    Meta *slot = find_meta_locked(table_id, column_id);
    if (slot != nullptr) {
      meta = *slot;
      matched = true;
    }
  }
  if (!matched) {
  } else if (meta.not_ready) {
    ret = OB_AI_COLUMN_NOT_READY;
    LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, static_cast<int64_t>(table_id), static_cast<int64_t>(column_id));
  } else {
    SqlBuf sql;
    bool has_row = false;
    if (OB_FAIL(sql.fmt("SELECT 1 FROM oceanbase.__all_ai_column_task WHERE table_id = %lu AND column_id = %lu LIMIT 1",
                        table_id, column_id))) {
    } else if (OB_FAIL(user_query(session, sql.data, has_row))) {
    } else if (has_row) {
      ret = OB_AI_COLUMN_NOT_READY;
      LOG_USER_ERROR(OB_AI_COLUMN_NOT_READY, static_cast<int64_t>(table_id), static_cast<int64_t>(column_id));
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase

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

#define USING_LOG_PREFIX SERVER

#include "observer/ai_column/ob_ai_column_worker.h"

#include <stdio.h>

#include "common/mysqlclient/ob_mysql_result.h"
#include "common/object/ob_object.h"
#include "lib/time/ob_time_utility.h"
#include "observer/ai_column/ob_ai_column_util.h"
#include "observer/ob_inner_sql_connection.h"
#include "observer/ob_server_runtime_access.h"
#include "share/ob_server_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_ai_column_mgr.h"
#include "share/schema/ob_ai_column_schema.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"

using namespace oceanbase::common;
using namespace oceanbase::common::sqlclient;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace observer
{

namespace
{

// The names of the tables and of the columns come from the schema, but they still are
// quoted and escaped: a table called "a`b" must not be able to change the statement the
// worker runs.
int append_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(name.empty())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("identifier is empty", KR(ret));
  } else if (OB_FAIL(sql.append("`"))) {
    LOG_WARN("fail to append the identifier quote", KR(ret));
  } else {
    for (int32_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
      const char c = name[i];
      if ('`' == c) {
        if (OB_FAIL(sql.append("`", 1))) {
          LOG_WARN("fail to escape the identifier quote", KR(ret));
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(sql.append(&c, 1))) {
        LOG_WARN("fail to append the identifier", KR(ret));
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append("`"))) {
      LOG_WARN("fail to append the identifier quote", KR(ret));
    }
  }
  return ret;
}

// A rowkey is written raw when it is an integer and quoted when it is character like.
// A table whose rowkey is of another type is skipped with a warning rather than being
// addressed with a value that would silently match the wrong row.
int build_rowkey_literal(ObIAllocator &allocator, const ObObj &value, ObString &literal)
{
  int ret = OB_SUCCESS;
  if (value.is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("a rowkey column is null", KR(ret));
  } else if (value.is_integer_type()) {
    int64_t int_value = 0;
    char buffer[32];
    int len = 0;
    if (OB_FAIL(value.get_int(int_value))) {
      LOG_WARN("fail to get the rowkey integer value", KR(ret));
    } else if (FALSE_IT(len = snprintf(buffer, sizeof(buffer), "%ld", int_value))) {
    } else if (len <= 0 || len >= static_cast<int>(sizeof(buffer))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected rowkey literal length", KR(ret), K(len));
    } else if (OB_FAIL(ob_write_string(allocator, ObString(len, buffer), literal))) {
      LOG_WARN("fail to copy the rowkey literal", KR(ret));
    }
  } else if (value.is_string_type()) {
    ObString str_value;
    if (OB_FAIL(value.get_string(str_value))) {
      LOG_WARN("fail to get the rowkey string value", KR(ret));
    } else if (OB_FAIL(ObAiColumnUtil::build_string_literal(allocator, str_value, literal))) {
      LOG_WARN("fail to quote the rowkey literal", KR(ret));
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("the rowkey type of an ai column table is not supported",
             KR(ret), "type", value.get_type());
  }
  return ret;
}

} // anonymous namespace

const int64_t ObAiColumnWorker::LEASE_DURATION_SEC;
const int64_t ObAiColumnWorker::RETRY_BACKOFF_SEC;
const int64_t ObAiColumnWorker::DISCOVER_LIMIT;
const int64_t ObAiColumnWorker::TASK_LIMIT;
const int64_t ObAiColumnWorker::IDLE_INTERVAL_US;

ObAiColumnWorker::ObAiColumnWorker()
  : is_inited_(false), allocator_(), conn_guard_(), conn_(NULL)
{
}

ObAiColumnWorker::~ObAiColumnWorker()
{
  destroy();
}

int ObAiColumnWorker::init()
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("the ai column worker is already initialized", KR(ret));
  } else {
    allocator_.set_label("AiColumnWorker");
    is_inited_ = true;
  }
  return ret;
}

void ObAiColumnWorker::destroy()
{
  reset_connection();
  allocator_.reset();
  is_inited_ = false;
}

int ObAiColumnWorker::run_once()
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard guard;
  ObSEArray<ObAiColumnTask, 8> tasks;
  allocator_.reuse();
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("the ai column worker is not initialized", KR(ret));
  } else if (OB_ISNULL(GCTX.schema_service_) || OB_ISNULL(get_observer_sql_engine())) {
    // The server is still starting up, the next round tries again.
    LOG_INFO("the server is not ready for the ai column service yet", KR(ret));
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
    // A schema that is being refreshed restarts the round from scratch.
    LOG_WARN("fail to get the schema guard of the ai column service", KR(ret));
  } else if (OB_FAIL(collect_tasks(guard, tasks))) {
    LOG_WARN("fail to collect the ai column tasks", KR(ret));
  } else if (tasks.empty()) {
    // nothing to do in this round
  } else if (OB_FAIL(get_connection(conn_))) {
    LOG_WARN("fail to create the inner sql connection of the ai column service", KR(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < tasks.count(); ++i) {
      // One broken definition must not stop the whole service.
      const int tmp_ret = run_task(tasks.at(i));
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("fail to run one ai column task", K(tmp_ret), K(tasks.at(i).ai_column_));
      }
    }
  }
  if (OB_NOT_NULL(conn_)) {
    reset_connection();
  }
  return ret;
}

int ObAiColumnWorker::collect_tasks(ObSchemaGetterGuard &guard,
                                    ObIArray<ObAiColumnTask> &tasks)
{
  int ret = OB_SUCCESS;
  ObArray<const ObTableSchema *> table_schemas;
  tasks.reuse();
  if (OB_FAIL(guard.get_table_schemas_in_runtime(table_schemas))) {
    LOG_WARN("fail to get the user tables of the runtime schema", KR(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table_schemas.count(); ++i) {
    const ObTableSchema *table_schema = table_schemas.at(i);
    bool has_ai_column = false;
    ObSEArray<const ObAiColumnSchema *, 4> ai_columns;
    if (OB_ISNULL(table_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the table schema is null", KR(ret), K(i));
    } else if (!table_schema->is_user_table()) {
      // indexes and inner tables never hold an ai column
    } else if (OB_FAIL(ObAiColumnMgr::has_ai_column(*table_schema, has_ai_column))) {
      LOG_WARN("fail to check whether the table has an ai column", KR(ret),
               K(table_schema->get_table_id()));
    } else if (!has_ai_column) {
      // an ordinary table costs one flag scan per round
    } else if (OB_FAIL(ObAiColumnMgr::get_ai_columns(allocator_, *table_schema, ai_columns))) {
      // A definition that no longer matches the table (a dropped dependency column for
      // instance) is reported once per round and skipped, it must not stop the service.
      LOG_WARN("fail to parse the ai columns of a table", KR(ret),
               K(table_schema->get_table_id()));
      ret = OB_SUCCESS;
    } else if (ai_columns.empty()) {
      // nothing to schedule
    } else {
      const ObDatabaseSchema *database_schema = NULL;
      if (OB_FAIL(guard.get_database_schema(table_schema->get_database_id(),
                                            database_schema))) {
        LOG_WARN("fail to get the database of the table", KR(ret),
                 K(table_schema->get_database_id()));
      } else if (OB_ISNULL(database_schema)) {
        LOG_INFO("the database of the table is not visible, skip it", K(i));
      } else {
        for (int64_t j = 0; OB_SUCC(ret) && j < ai_columns.count(); ++j) {
          const ObAiColumnSchema *ai_column = ai_columns.at(j);
          ObAiColumnTask task;
          if (OB_ISNULL(ai_column)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("the ai column schema is null", KR(ret), K(j));
          } else if (OB_FAIL(build_task(*table_schema,
                                        *database_schema,
                                        *ai_column,
                                        task))) {
            LOG_WARN("fail to build the ai column task", KR(ret), KPC(ai_column));
            ret = OB_SUCCESS;
          } else if (OB_FAIL(tasks.push_back(task))) {
            LOG_WARN("fail to push back the ai column task", KR(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::build_task(const ObTableSchema &table_schema,
                                 const ObDatabaseSchema &database_schema,
                                 const ObAiColumnSchema &ai_column,
                                 ObAiColumnTask &task)
{
  int ret = OB_SUCCESS;
  task.table_schema_ = &table_schema;
  task.ai_column_ = &ai_column;
  task.database_name_ = database_schema.get_database_name_str();
  task.table_name_ = table_schema.get_table_name_str();
  task.result_column_name_ = ai_column.get_column_name();
  if (OB_UNLIKELY(OB_INVALID_ID == ai_column.get_status_column_id()
                  || OB_INVALID_ID == ai_column.get_lease_column_id()
                  || OB_INVALID_ID == ai_column.get_retry_column_id())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the state columns of the ai column are not resolved", KR(ret), K(ai_column));
  } else if (OB_FAIL(ObAiColumnSchema::build_status_column_name(
                       allocator_, ai_column.get_column_name(), task.status_column_name_))) {
    LOG_WARN("fail to build the status column name", KR(ret), K(ai_column));
  } else if (OB_FAIL(ObAiColumnSchema::build_lease_column_name(
                       allocator_, ai_column.get_column_name(), task.lease_column_name_))) {
    LOG_WARN("fail to build the lease column name", KR(ret), K(ai_column));
  } else if (OB_FAIL(ObAiColumnSchema::build_retry_column_name(
                       allocator_, ai_column.get_column_name(), task.retry_column_name_))) {
    LOG_WARN("fail to build the retry column name", KR(ret), K(ai_column));
  }
  return ret;
}

int ObAiColumnWorker::run_task(const ObAiColumnTask &task)
{
  int ret = OB_SUCCESS;
  const int64_t now_sec = ObTimeUtility::current_time() / 1000000L;
  ObSEArray<ObAiColumnRow, 8> rows;
  if (OB_FAIL(discover_rows(task, now_sec, rows))) {
    LOG_WARN("fail to discover the pending rows of an ai column", KR(ret),
             K(task.database_name_), K(task.table_name_), K(task.result_column_name_));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rows.count() && i < TASK_LIMIT; ++i) {
    const ObAiColumnRow &row = rows.at(i);
    // The lease deadline doubles as the claim token, so a publish can be matched against
    // the claim that produced it.
    const int64_t claim_token = now_sec + LEASE_DURATION_SEC;
    bool claimed = false;
    bool has_answer = false;
    bool is_null = false;
    ObString answer;
    ObString literal;
    int tmp_ret = claim_row(task, row, claim_token, claimed);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("fail to claim a row of an ai column", K(tmp_ret),
               K(task.table_name_), K(row.row_filter_));
    } else if (!claimed) {
      // another worker won the row
    } else if (OB_SUCCESS != (tmp_ret = compute_answer(task, row.row_filter_, answer, has_answer))) {
      LOG_WARN("fail to compute the answer of an ai column", K(tmp_ret),
               K(task.table_name_), K(row.row_filter_));
      const int tmp_ret2 = publish_failure(task, row, claim_token, now_sec);
      if (OB_SUCCESS != tmp_ret2) {
        LOG_WARN("fail to record the failed attempt of an ai column", K(tmp_ret2),
                 K(task.table_name_), K(row.row_filter_));
      }
    } else if (!has_answer) {
      // The model was not called at all because a dependency is NULL: the result stays
      // NULL and the row is done, it must not be retried forever.
      tmp_ret = publish_result(task, row, claim_token, ObString::make_string("NULL"), true);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("fail to publish an empty result", K(tmp_ret), K(row.row_filter_));
      }
    } else if (OB_SUCCESS != (tmp_ret = ObAiColumnUtil::build_result_literal(
                                allocator_, *task.ai_column_, answer, literal, is_null))) {
      // The answer does not fit the declared result type. The result column keeps its
      // previous value only until the retry budget is exhausted, so a value that can
      // never match the type ends up FAILED instead of being silently stored.
      LOG_WARN("the answer of an ai column is rejected", K(tmp_ret), K(answer),
               K(task.table_name_), K(task.result_column_name_));
      const int tmp_ret2 = publish_failure(task, row, claim_token, now_sec);
      if (OB_SUCCESS != tmp_ret2) {
        LOG_WARN("fail to record the rejected answer", K(tmp_ret2), K(row.row_filter_));
      }
    } else {
      tmp_ret = publish_result(task, row, claim_token, literal, is_null);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("fail to publish the ai column result", K(tmp_ret), K(row.row_filter_));
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::build_table_name(const ObAiColumnTask &task, ObSqlString &sql) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(append_identifier(sql, task.database_name_))) {
    LOG_WARN("fail to append the database name", KR(ret), K(task.database_name_));
  } else if (OB_FAIL(sql.append("."))) {
    LOG_WARN("fail to append the name separator", KR(ret));
  } else if (OB_FAIL(append_identifier(sql, task.table_name_))) {
    LOG_WARN("fail to append the table name", KR(ret), K(task.table_name_));
  }
  return ret;
}

int ObAiColumnWorker::get_rowkey_column_ids(const ObAiColumnTask &task,
                                            ObIArray<uint64_t> &column_ids) const
{
  int ret = OB_SUCCESS;
  column_ids.reuse();
  if (OB_FAIL(task.table_schema_->get_rowkey_column_ids(column_ids))) {
    LOG_WARN("fail to get the rowkey columns of the table", KR(ret),
             K(task.table_schema_->get_table_id()));
  } else if (column_ids.empty()) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("the table has no rowkey", KR(ret), K(task.table_schema_->get_table_id()));
  }
  return ret;
}

int ObAiColumnWorker::read_row_filter(const ObAiColumnTask &task,
                                      const ObIArray<uint64_t> &rowkey_column_ids,
                                      const ObMySQLResult &res,
                                      ObString &row_filter)
{
  int ret = OB_SUCCESS;
  ObSqlString filter;
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_column_ids.count(); ++i) {
    const ObColumnSchemaV2 *column_schema =
        task.table_schema_->get_column_schema(rowkey_column_ids.at(i));
    ObObj value;
    ObString literal;
    if (OB_ISNULL(column_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the rowkey column does not exist", KR(ret), K(rowkey_column_ids.at(i)));
    } else if (OB_FAIL(res.get_obj(i, value, NULL, &allocator_))) {
      LOG_WARN("fail to read the rowkey value", KR(ret), K(i));
    } else if (OB_FAIL(build_rowkey_literal(allocator_, value, literal))) {
      LOG_WARN("fail to build the rowkey literal", KR(ret), K(i));
    } else if (i > 0 && OB_FAIL(filter.append(" AND "))) {
      LOG_WARN("fail to append the filter separator", KR(ret));
    } else if (OB_FAIL(append_identifier(filter, column_schema->get_column_name_str()))) {
      LOG_WARN("fail to append the rowkey column name", KR(ret));
    } else if (OB_FAIL(filter.append(" = "))) {
      LOG_WARN("fail to append the comparison", KR(ret));
    } else if (OB_FAIL(filter.append(literal))) {
      LOG_WARN("fail to append the rowkey literal", KR(ret));
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator_, filter.string(), row_filter))) {
    LOG_WARN("fail to copy the row filter", KR(ret));
  }
  return ret;
}

int ObAiColumnWorker::discover_rows(const ObAiColumnTask &task,
                                    const int64_t now_sec,
                                    ObIArray<ObAiColumnRow> &rows)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObSEArray<uint64_t, 4> rowkey_column_ids;
  ObInnerSQLConnection *conn = NULL;
  ObISQLClient::ReadResult read_result;
  ObMySQLResult *mysql_result = NULL;
  rows.reuse();
  if (OB_FAIL(get_rowkey_column_ids(task, rowkey_column_ids))) {
    LOG_WARN("fail to get the rowkey columns", KR(ret));
  } else if (OB_FAIL(get_connection(conn))) {
    LOG_WARN("fail to get the inner sql connection", KR(ret));
  } else if (OB_FAIL(sql.append("SELECT "))) {
    LOG_WARN("fail to start the discovery statement", KR(ret));
  }
  // The rowkey, then the three state columns: the state is read here so that the claim
  // and the publish can compare the exact version of the row they were planned against.
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_column_ids.count(); ++i) {
    const ObColumnSchemaV2 *column_schema =
        task.table_schema_->get_column_schema(rowkey_column_ids.at(i));
    if (OB_ISNULL(column_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the rowkey column does not exist", KR(ret), K(i));
    } else if (i > 0 && OB_FAIL(sql.append(", "))) {
      LOG_WARN("fail to append the column separator", KR(ret));
    } else if (OB_FAIL(append_identifier(sql, column_schema->get_column_name_str()))) {
      LOG_WARN("fail to append the rowkey column name", KR(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(append_identifier(sql, task.status_column_name_))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(append_identifier(sql, task.lease_column_name_))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(append_identifier(sql, task.retry_column_name_))) {
  } else if (OB_FAIL(sql.append(" FROM "))) {
  } else if (OB_FAIL(build_table_name(task, sql))) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  } else if (OB_FAIL(append_identifier(sql, task.status_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" < %ld AND ", AI_COLUMN_STATUS_READY))) {
  } else if (OB_FAIL(append_identifier(sql, task.lease_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" <= %ld ORDER BY ", now_sec))) {
  } else if (OB_FAIL(append_identifier(sql, task.lease_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" LIMIT %ld", DISCOVER_LIMIT))) {
  } else if (OB_FAIL(conn->execute_read(sql.string(), read_result, false))) {
    LOG_WARN("fail to execute the discovery statement", KR(ret), K(sql));
  } else if (OB_ISNULL(mysql_result = read_result.get_result())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the discovery result is null", KR(ret), K(sql));
  } else {
    const int64_t base = rowkey_column_ids.count();
    int tmp_ret = mysql_result->next();
    while (OB_SUCCESS == tmp_ret) {
      ObAiColumnRow row;
      if (OB_SUCCESS != (tmp_ret = read_row_filter(task, rowkey_column_ids,
                                                   *mysql_result, row.row_filter_))) {
        LOG_WARN("fail to read the row identity", K(tmp_ret));
      } else if (OB_SUCCESS != (tmp_ret = mysql_result->get_int(base + 0, row.status_))) {
        LOG_WARN("fail to read the status of the row", K(tmp_ret));
      } else if (OB_SUCCESS != (tmp_ret = mysql_result->get_int(base + 1, row.lease_))) {
        LOG_WARN("fail to read the lease of the row", K(tmp_ret));
      } else if (OB_SUCCESS != (tmp_ret = mysql_result->get_int(base + 2, row.retry_))) {
        LOG_WARN("fail to read the retry of the row", K(tmp_ret));
      } else if (OB_SUCCESS != (tmp_ret = rows.push_back(row))) {
        LOG_WARN("fail to keep the discovered row", K(tmp_ret));
      } else {
        tmp_ret = mysql_result->next();
      }
    }
    if (OB_ITER_END == tmp_ret) {
      tmp_ret = OB_SUCCESS;
    }
    if (OB_SUCCESS != tmp_ret) {
      ret = tmp_ret;
    }
  }
  if (OB_NOT_NULL(mysql_result)) {
    read_result.close();
  }
  return ret;
}

int ObAiColumnWorker::claim_row(const ObAiColumnTask &task,
                                const ObAiColumnRow &row,
                                const int64_t claim_token,
                                bool &claimed)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObInnerSQLConnection *conn = NULL;
  int64_t affected_rows = 0;
  claimed = false;
  if (OB_FAIL(get_connection(conn))) {
    LOG_WARN("fail to get the inner sql connection", KR(ret));
  } else if (OB_FAIL(sql.append("UPDATE "))) {
  } else if (OB_FAIL(build_table_name(task, sql))) {
  } else if (OB_FAIL(sql.append(" SET "))) {
  } else if (OB_FAIL(append_identifier(sql, task.status_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld, ", AI_COLUMN_STATUS_PROCESSING))) {
  } else if (OB_FAIL(append_identifier(sql, task.lease_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld WHERE ", claim_token))) {
  } else if (OB_FAIL(sql.append(row.row_filter_))) {
  } else if (OB_FAIL(sql.append(" AND "))) {
  } else if (OB_FAIL(append_identifier(sql, task.status_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld", row.status_))) {
  } else if (OB_FAIL(sql.append(" AND "))) {
  } else if (OB_FAIL(append_identifier(sql, task.lease_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld", row.lease_))) {
  } else if (OB_FAIL(sql.append(" AND "))) {
  } else if (OB_FAIL(append_identifier(sql, task.retry_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld", row.retry_))) {
  } else if (OB_FAIL(conn->execute_write(sql.string(), affected_rows, false))) {
    LOG_WARN("fail to execute the claim statement", KR(ret), K(sql));
  } else {
    claimed = affected_rows > 0;
  }
  return ret;
}

int ObAiColumnWorker::build_result_expr(const ObAiColumnTask &task, ObSqlString &expr) const
{
  int ret = OB_SUCCESS;
  const ObAiColumnSchema &ai_column = *task.ai_column_;
  const ObIArray<uint64_t> &depend_column_ids = ai_column.get_depend_column_ids();
  const ObString &prompt_hint = ai_column.get_prompt_hint();
  ObSqlString content_template;
  ObString template_literal;
  ObString model_literal;
  // The prompt is the hint chosen by the result type at DDL time followed by one
  // placeholder per dependency, so the model is told both what to answer and what to
  // answer about.
  if (!prompt_hint.empty() && OB_FAIL(content_template.append(prompt_hint))) {
    LOG_WARN("fail to append the prompt hint", KR(ret), K(prompt_hint));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < depend_column_ids.count(); ++i) {
    if ((i > 0 || !prompt_hint.empty()) && OB_FAIL(content_template.append(" "))) {
      LOG_WARN("fail to append the prompt separator", KR(ret));
    } else if (OB_FAIL(content_template.append_fmt("{%ld}", i))) {
      LOG_WARN("fail to append the prompt placeholder", KR(ret), K(i));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAiColumnUtil::build_string_literal(allocator_,
                                                          content_template.string(),
                                                          template_literal))) {
    LOG_WARN("fail to quote the prompt template", KR(ret));
  } else if (OB_FAIL(ObAiColumnUtil::build_string_literal(allocator_,
                                                          ai_column.get_model_name(),
                                                          model_literal))) {
    LOG_WARN("fail to quote the model name", KR(ret));
  }
  // A NULL dependency never reaches the model, ai_prompt rejects NULL arguments and a
  // completion of "NULL" would be stored as a real answer.
  for (int64_t i = 0; OB_SUCC(ret) && i < depend_column_ids.count(); ++i) {
    const ObColumnSchemaV2 *column_schema =
        task.table_schema_->get_column_schema(depend_column_ids.at(i));
    if (OB_ISNULL(column_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the depend column of the ai column does not exist", KR(ret),
               K(depend_column_ids.at(i)));
    } else if (OB_FAIL(expr.append(0 == i ? "CASE WHEN " : " OR "))) {
      LOG_WARN("fail to append the null guard", KR(ret));
    } else if (OB_FAIL(append_identifier(expr, column_schema->get_column_name_str()))) {
      LOG_WARN("fail to append the depend column name", KR(ret));
    } else if (OB_FAIL(expr.append(" IS NULL"))) {
      LOG_WARN("fail to append the null test", KR(ret));
    }
  }
  if (OB_SUCC(ret) && !depend_column_ids.empty()) {
    if (OB_FAIL(expr.append(" THEN NULL ELSE "))) {
      LOG_WARN("fail to append the null branch", KR(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(expr.append(ai_column.is_embedding() ? "ai_embed(" : "ai_complete("))) {
  } else if (OB_FAIL(expr.append(model_literal))) {
  } else if (OB_FAIL(expr.append(", "))) {
  } else if (depend_column_ids.empty()) {
    if (OB_FAIL(expr.append(template_literal))) {
      LOG_WARN("fail to append the prompt", KR(ret));
    }
  } else if (OB_FAIL(expr.append("ai_prompt("))) {
  } else if (OB_FAIL(expr.append(template_literal))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < depend_column_ids.count(); ++i) {
      const ObColumnSchemaV2 *column_schema =
          task.table_schema_->get_column_schema(depend_column_ids.at(i));
      if (OB_ISNULL(column_schema)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the depend column of the ai column does not exist", KR(ret),
                 K(depend_column_ids.at(i)));
      } else if (OB_FAIL(expr.append(", "))) {
        LOG_WARN("fail to append the argument separator", KR(ret));
      } else if (OB_FAIL(append_identifier(expr, column_schema->get_column_name_str()))) {
        LOG_WARN("fail to append the depend column name", KR(ret));
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(expr.append(")"))) {
      LOG_WARN("fail to close the prompt", KR(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (ai_column.is_embedding()
             && OB_FAIL(expr.append_fmt(", %ld", ai_column.get_vector_dimension()))) {
    // The dimension is persisted in the metadata, the answer is checked against it and
    // the model is asked for exactly that many values.
    LOG_WARN("fail to append the vector dimension", KR(ret), K(ai_column));
  } else if (OB_FAIL(expr.append(")"))) {
    LOG_WARN("fail to close the ai function", KR(ret));
  } else if (!depend_column_ids.empty() && OB_FAIL(expr.append(" END"))) {
    LOG_WARN("fail to close the null guard", KR(ret));
  }
  return ret;
}

int ObAiColumnWorker::compute_answer(const ObAiColumnTask &task,
                                     const ObString &row_filter,
                                     ObString &answer,
                                     bool &has_answer)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObInnerSQLConnection *conn = NULL;
  ObISQLClient::ReadResult read_result;
  ObMySQLResult *mysql_result = NULL;
  has_answer = false;
  if (OB_FAIL(get_connection(conn))) {
    LOG_WARN("fail to get the inner sql connection", KR(ret));
  } else if (OB_FAIL(sql.append("SELECT "))) {
  } else if (OB_FAIL(build_result_expr(task, sql))) {
    LOG_WARN("fail to build the ai function expression", KR(ret), KPC(task.ai_column_));
  } else if (OB_FAIL(sql.append(" FROM "))) {
  } else if (OB_FAIL(build_table_name(task, sql))) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  } else if (OB_FAIL(sql.append(row_filter))) {
  } else if (OB_FAIL(conn->execute_read(sql.string(), read_result, false))) {
    LOG_WARN("fail to execute the ai function statement", KR(ret), K(sql));
  } else if (OB_ISNULL(mysql_result = read_result.get_result())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the ai function result is null", KR(ret), K(sql));
  } else {
    int tmp_ret = mysql_result->next();
    if (OB_ITER_END == tmp_ret) {
      // The row was deleted between the claim and the computation, nothing to publish.
      tmp_ret = OB_SUCCESS;
    } else if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("fail to read the ai function result", K(tmp_ret));
    } else {
      ObObj value;
      if (OB_SUCCESS != (tmp_ret = mysql_result->get_obj(0, value, NULL, &allocator_))) {
        LOG_WARN("fail to read the ai function value", K(tmp_ret));
      } else if (value.is_null()) {
        has_answer = false;
      } else if (!value.is_string_type()) {
        tmp_ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the ai function did not return a text answer", K(tmp_ret),
                 "type", value.get_type());
      } else {
        ObString str_value;
        if (OB_SUCCESS != (tmp_ret = value.get_string(str_value))) {
          LOG_WARN("fail to get the answer text", K(tmp_ret));
        } else if (OB_SUCCESS != (tmp_ret = ob_write_string(allocator_, str_value, answer))) {
          LOG_WARN("fail to copy the answer text", K(tmp_ret));
        } else {
          has_answer = true;
        }
      }
    }
    if (OB_SUCCESS != tmp_ret) {
      ret = tmp_ret;
    }
    read_result.close();
  }
  return ret;
}

int ObAiColumnWorker::publish_result(const ObAiColumnTask &task,
                                     const ObAiColumnRow &row,
                                     const int64_t claim_token,
                                     const ObString &literal,
                                     const bool is_null)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObInnerSQLConnection *conn = NULL;
  int64_t affected_rows = 0;
  UNUSED(is_null);
  if (OB_FAIL(get_connection(conn))) {
    LOG_WARN("fail to get the inner sql connection", KR(ret));
  } else if (OB_FAIL(sql.append("UPDATE "))) {
  } else if (OB_FAIL(build_table_name(task, sql))) {
  } else if (OB_FAIL(sql.append(" SET "))) {
  } else if (OB_FAIL(append_identifier(sql, task.result_column_name_))) {
  } else if (OB_FAIL(sql.append(" = "))) {
  } else if (OB_FAIL(sql.append(literal))) {
  } else if (OB_FAIL(sql.append(", "))) {
  } else if (OB_FAIL(append_identifier(sql, task.status_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld, ", AI_COLUMN_STATUS_READY))) {
  } else if (OB_FAIL(append_identifier(sql, task.lease_column_name_))) {
  } else if (OB_FAIL(sql.append(" = 0 WHERE "))) {
  } else if (OB_FAIL(sql.append(row.row_filter_))) {
  } else if (OB_FAIL(sql.append(" AND "))) {
  } else if (OB_FAIL(append_identifier(sql, task.status_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld", AI_COLUMN_STATUS_PROCESSING))) {
  } else if (OB_FAIL(sql.append(" AND "))) {
  } else if (OB_FAIL(append_identifier(sql, task.lease_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld", claim_token))) {
  } else if (OB_FAIL(sql.append(" AND "))) {
  } else if (OB_FAIL(append_identifier(sql, task.retry_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld", row.retry_))) {
  } else if (OB_FAIL(conn->execute_write(sql.string(), affected_rows, false))) {
    LOG_WARN("fail to execute the publish statement", KR(ret), K(sql));
  } else if (0 == affected_rows) {
    // The row was invalidated, re-claimed or dropped while the model was running: the
    // newer state wins and this answer is thrown away instead of overwriting it.
    LOG_INFO("the publish of an ai column result lost the race against a newer task",
             K(task.table_name_), K(task.result_column_name_), K(row.row_filter_));
  }
  return ret;
}

int ObAiColumnWorker::publish_failure(const ObAiColumnTask &task,
                                      const ObAiColumnRow &row,
                                      const int64_t claim_token,
                                      const int64_t now_sec)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObInnerSQLConnection *conn = NULL;
  int64_t affected_rows = 0;
  const int64_t max_retry = task.ai_column_->get_max_retry();
  // A failed attempt waits before it is retried, and the last one ends in the terminal
  // FAILED state instead of being retried forever.
  const bool is_last_attempt = row.retry_ + 1 >= max_retry;
  if (OB_FAIL(get_connection(conn))) {
    LOG_WARN("fail to get the inner sql connection", KR(ret));
  } else if (OB_FAIL(sql.append("UPDATE "))) {
  } else if (OB_FAIL(build_table_name(task, sql))) {
  } else if (OB_FAIL(sql.append(" SET "))) {
  } else if (OB_FAIL(append_identifier(sql, task.status_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld, ",
                                    is_last_attempt ? AI_COLUMN_STATUS_FAILED
                                                    : AI_COLUMN_STATUS_PENDING))) {
  } else if (OB_FAIL(append_identifier(sql, task.lease_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld, ", is_last_attempt ? 0 : now_sec + RETRY_BACKOFF_SEC))) {
  } else if (OB_FAIL(append_identifier(sql, task.retry_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld, ", row.retry_ + 1))) {
  } else if (OB_FAIL(sql.append(" WHERE "))) {
  } else if (OB_FAIL(sql.append(row.row_filter_))) {
  } else if (OB_FAIL(sql.append(" AND "))) {
  } else if (OB_FAIL(append_identifier(sql, task.status_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld", AI_COLUMN_STATUS_PROCESSING))) {
  } else if (OB_FAIL(sql.append(" AND "))) {
  } else if (OB_FAIL(append_identifier(sql, task.lease_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld", claim_token))) {
  } else if (OB_FAIL(sql.append(" AND "))) {
  } else if (OB_FAIL(append_identifier(sql, task.retry_column_name_))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld", row.retry_))) {
  } else if (OB_FAIL(conn->execute_write(sql.string(), affected_rows, false))) {
    LOG_WARN("fail to execute the retry statement", KR(ret), K(sql));
  } else if (0 == affected_rows) {
    LOG_INFO("the retry of an ai column task lost the race against a newer task",
             K(task.table_name_), K(task.result_column_name_), K(row.row_filter_));
  }
  return ret;
}

int ObAiColumnWorker::get_connection(ObInnerSQLConnection *&conn)
{
  int ret = OB_SUCCESS;
  conn = NULL;
  if (OB_NOT_NULL(conn_)) {
    conn = conn_;
  } else if (OB_FAIL(ObInnerSQLConnection::create_connection_with_owned_session(
                       false, 0, conn_guard_))) {
    LOG_WARN("fail to create the inner sql connection", KR(ret));
  } else if (OB_ISNULL(conn_ = static_cast<ObInnerSQLConnection *>(conn_guard_.get()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the inner sql connection is null", KR(ret));
  } else {
    conn = conn_;
  }
  return ret;
}

void ObAiColumnWorker::reset_connection()
{
  conn_ = NULL;
  conn_guard_.reset();
}

} // namespace observer
} // namespace oceanbase
